// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/model.h"

namespace manta::render {

namespace {

bool isGroundNet(const Design& d, const Net& n) {
    if (n.ground) return true;
    if (const NetDirective* t = n.directives.find("TYPE"); t && t->value == "GROUND") return true;
    if (n.pins.empty()) return false;
    for (const PinRef& p : n.pins) {
        if (d.components[p.component].pins[p.pin].type != PinType::Ground) return false;
    }
    return true;
}

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isWord(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The anchored spellings a supply net is recognised by: "3V3", "12V", VBUS,
// VCC, VDD, VEE, AVDD, VIN, VOUT, VBAT, and "V-<word>". Whole-name compare,
// case-folded to upper.
bool railName(std::string_view name) {
    std::string u;
    u.reserve(name.size());
    for (char c : name) u += upper(c);

    // ^\d+V\d*$
    std::size_t i = 0;
    while (i < u.size() && isDigit(u[i])) ++i;
    if (i > 0 && i < u.size() && u[i] == 'V') {
        std::size_t j = i + 1;
        while (j < u.size() && isDigit(u[j])) ++j;
        if (j == u.size()) return true;
    }

    for (std::string_view w : {"VBUS", "VCC", "VDD", "VEE", "AVDD", "VIN", "VOUT", "VBAT"}) {
        if (u == w) return true;
    }

    // V-\w+
    if (u.size() > 2 && u[0] == 'V' && u[1] == '-') {
        for (std::size_t k = 2; k < u.size(); ++k) {
            if (!isWord(u[k])) return false;
        }
        return true;
    }
    return false;
}

bool isRailNet(const Design& d, const Net& n) {
    if (const NetDirective* c = n.directives.find("CLASS"); c && c->value == "power") return true;
    for (const PinRef& p : n.pins) {
        const ComponentPin& pin = d.components[p.component].pins[p.pin];
        if (pin.type == PinType::Power && pin.direction == PortDir::Out) return true;
    }
    return railName(n.name);
}

// Groups a page's components into rooms by their `section` string, in order of
// first appearance. With no sections at all the page is one unframed region;
// otherwise sectionless components form an untitled framed room placed first.
void partitionRooms(const Design& design, RenderPage& page) {
    bool anySection = false;
    bool anyBare = false;
    for (std::uint32_t idx : page.components) {
        if (design.components[idx].section.empty()) anyBare = true;
        else anySection = true;
    }

    if (!anySection) {
        RenderRoom room;
        room.framed = false;
        room.components = page.components;
        page.rooms.push_back(std::move(room));
        return;
    }

    if (anyBare) page.rooms.push_back(RenderRoom{});  // untitled, framed, first
    for (std::uint32_t idx : page.components) {
        const std::string& section = design.components[idx].section;
        RenderRoom* room = nullptr;
        for (RenderRoom& r : page.rooms) {
            if (r.title == section) {
                room = &r;
                break;
            }
        }
        if (!room) {
            RenderRoom fresh;
            fresh.title = section;
            page.rooms.push_back(std::move(fresh));
            room = &page.rooms.back();
        }
        room->components.push_back(idx);
    }
}

}  // namespace

RenderModel buildRenderModel(const Design& design) {
    RenderModel m;
    m.design = &design;

    m.nets.reserve(design.nets.size());
    for (const Net& n : design.nets) {
        RenderNet rn;
        rn.display = n.name;
        rn.direction = n.direction;
        if (isGroundNet(design, n)) rn.mark = NetMark::Ground;
        else if (isRailNet(design, n)) rn.mark = NetMark::Rail;
        m.nets.push_back(std::move(rn));
    }

    // On a schematic, labels connect by name: a block-local net that shares its
    // name with a ground or rail net is the same conductor to a reader, so it
    // gets the same mark even where its own scope carried no directive.
    for (std::size_t i = 0; i < m.nets.size(); ++i) {
        if (m.nets[i].mark == NetMark::Label) continue;
        for (std::size_t j = 0; j < m.nets.size(); ++j) {
            if (m.nets[j].display == m.nets[i].display) m.nets[j].mark = m.nets[i].mark;
        }
    }

    m.kinds.reserve(design.components.size());
    for (const Component& c : design.components) m.kinds.push_back(classifySymbol(c));

    RenderPage page;
    page.id = "page-" + (design.top.empty() ? std::string("1") : design.top);
    page.title = design.top.empty() ? std::string("schematic") : design.top;
    for (std::uint32_t i = 0; i < design.components.size(); ++i) page.components.push_back(i);
    partitionRooms(design, page);
    m.pages.push_back(std::move(page));

    // A net whose pins land in two or more rooms of one page is a crossing:
    // drawn as a port flag at every appearance. Room indices per component are
    // recomputed here rather than stored -- pages own their partition.
    for (const RenderPage& p : m.pages) {
        if (p.rooms.size() < 2) continue;
        std::vector<std::int32_t> roomOf(design.components.size(), -1);
        for (std::size_t r = 0; r < p.rooms.size(); ++r) {
            for (std::uint32_t idx : p.rooms[r].components) {
                roomOf[idx] = static_cast<std::int32_t>(r);
            }
        }
        for (std::size_t n = 0; n < design.nets.size(); ++n) {
            std::int32_t first = -1;
            for (const PinRef& pr : design.nets[n].pins) {
                std::int32_t room = roomOf[pr.component];
                if (room < 0) continue;
                if (first < 0) first = room;
                else if (room != first) {
                    m.nets[n].crossing = true;
                    break;
                }
            }
        }
    }

    return m;
}

}  // namespace manta::render
