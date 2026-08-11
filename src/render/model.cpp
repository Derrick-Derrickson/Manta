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

// Spec 11.6: "'&TYPE=NC' means the datasheet forbids connection". Such a net
// conducts nothing, so it is drawn as a no-connect cross and never labelled --
// the name a lone NC pin's net carries reads as a signal that is not there.
// Connecting a NC pin is error E-25, so a net mixing NC with ordinary pins is
// already diagnosed; only an all-NC net takes the mark here.
bool isNoConnectNet(const Design& d, const Net& n) {
    if (n.pins.empty()) return false;
    for (const PinRef& p : n.pins) {
        if (d.components[p.component].pins[p.pin].type != PinType::NC) return false;
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

// A CLASS value whose '-'/'_'-separated tokens contain "power" declares a
// supply: "power" and "raw-power" qualify, "powerful" does not -- the token
// must stand whole, not merely prefix the value.
bool classSaysPower(std::string_view v) {
    std::size_t at = 0;
    while (at <= v.size()) {
        std::size_t end = v.find_first_of("-_", at);
        if (end == std::string_view::npos) end = v.size();
        if (v.substr(at, end - at) == "power") return true;
        at = end + 1;
    }
    return false;
}

// Rail by evidence on the Net itself; the name heuristic is applied per page,
// over the page-local spelling.
bool isRailNet(const Design& d, const Net& n) {
    if (const NetDirective* c = n.directives.find("CLASS"); c && classSaysPower(c->value))
        return true;
    for (const PinRef& p : n.pins) {
        const ComponentPin& pin = d.components[p.component].pins[p.pin];
        if (pin.type == PinType::Power && pin.direction == PortDir::Out) return true;
    }
    return false;
}

// Groups a page's components and child sheet symbols into rooms by their
// `section` string, in order of first appearance (components first). With no
// sections at all the page is one unframed region; otherwise sectionless
// entries form an untitled framed room placed first.
void partitionRooms(const Design& design, RenderPage& page) {
    bool anySection = false;
    bool anyBare = false;
    for (std::uint32_t idx : page.components) {
        if (design.components[idx].section.empty()) anyBare = true;
        else anySection = true;
    }
    for (std::uint32_t idx : page.children) {
        if (design.blocks[idx].section.empty()) anyBare = true;
        else anySection = true;
    }

    if (!anySection) {
        RenderRoom room;
        room.framed = false;
        room.components = page.components;
        room.children = page.children;
        page.rooms.push_back(std::move(room));
        return;
    }

    if (anyBare) page.rooms.push_back(RenderRoom{});  // untitled, framed, first
    auto roomFor = [&](const std::string& section) -> RenderRoom& {
        for (RenderRoom& r : page.rooms) {
            if (r.title == section) return r;
        }
        RenderRoom fresh;
        fresh.title = section;
        page.rooms.push_back(std::move(fresh));
        return page.rooms.back();
    };
    for (std::uint32_t idx : page.components) {
        roomFor(design.components[idx].section).components.push_back(idx);
    }
    for (std::uint32_t idx : page.children) {
        roomFor(design.blocks[idx].section).children.push_back(idx);
    }
}

// The page's view of every net: local display names on a definition page, the
// port directions of the represented block, marks from the underlying Net, and
// the room-crossing flags for this page's own partition.
void buildPageNets(const Design& design, RenderPage& page, const BlockInstance* rep) {
    page.nets.reserve(design.nets.size());
    for (const Net& n : design.nets) {
        RenderNet rn;
        rn.display = n.name;
        // A definition page shows the block from inside: the flat design-wide
        // direction belongs to the top page only.
        rn.direction = rep ? PortDir::None : n.direction;
        page.nets.push_back(std::move(rn));
    }
    if (rep) {
        for (const auto& [name, net] : rep->localNets) {
            if (net >= 0) page.nets[static_cast<std::size_t>(net)].display = name;
        }
        for (const BlockPort& p : rep->ports) {
            if (p.net < 0) continue;
            RenderNet& rn = page.nets[static_cast<std::size_t>(p.net)];
            rn.display = p.name;
            rn.direction = p.direction;
        }
    }

    // Ground and rail from the Net's own evidence; the rail-name heuristic
    // runs over the page-local spelling, so a block's "VCC" is a rail here
    // whatever its flat name became.
    for (std::size_t i = 0; i < page.nets.size(); ++i) {
        RenderNet& rn = page.nets[i];
        const Net& n = design.nets[i];
        if (isGroundNet(design, n)) rn.mark = NetMark::Ground;
        else if (isRailNet(design, n) || railName(rn.display)) rn.mark = NetMark::Rail;
    }

    // On a schematic, labels connect by name: a net that shares its displayed
    // spelling with a ground or rail net is the same conductor to a reader,
    // so it gets the same mark even where its own scope carried no directive.
    for (std::size_t i = 0; i < page.nets.size(); ++i) {
        if (page.nets[i].mark == NetMark::Label) continue;
        for (std::size_t j = 0; j < page.nets.size(); ++j) {
            if (page.nets[j].display == page.nets[i].display) {
                page.nets[j].mark = page.nets[i].mark;
            }
        }
    }

    // A no-connect is settled after the spelling pass above, so it neither
    // spreads by name -- two unrelated NC pins may both display "NC" -- nor
    // displaces a ground or rail mark.
    for (std::size_t i = 0; i < page.nets.size(); ++i) {
        RenderNet& rn = page.nets[i];
        if (rn.mark != NetMark::Label) continue;
        if (isNoConnectNet(design, design.nets[i])) rn.mark = NetMark::NoConnect;
    }

    // A net touching two or more rooms of this page -- through a component pin
    // or a sheet-symbol port -- is a crossing: drawn as a port flag at every
    // appearance.
    if (page.rooms.size() < 2) return;
    std::vector<std::int32_t> firstRoom(design.nets.size(), -1);
    for (std::size_t r = 0; r < page.rooms.size(); ++r) {
        auto touch = [&](std::int32_t net) {
            if (net < 0) return;
            std::int32_t& first = firstRoom[static_cast<std::size_t>(net)];
            if (first < 0) first = static_cast<std::int32_t>(r);
            else if (first != static_cast<std::int32_t>(r)) {
                page.nets[static_cast<std::size_t>(net)].crossing = true;
            }
        };
        for (std::uint32_t idx : page.rooms[r].components) {
            for (const ComponentPin& p : design.components[idx].pins) touch(p.net);
        }
        for (std::uint32_t idx : page.rooms[r].children) {
            for (const BlockPort& p : design.blocks[idx].ports) touch(p.net);
        }
    }
}

// True when `path` is `base` plus exactly one trailing element.
bool directlyUnder(const std::vector<std::string>& path, const std::vector<std::string>& base) {
    if (path.size() != base.size() + 1) return false;
    for (std::size_t i = 0; i < base.size(); ++i) {
        if (path[i] != base[i]) return false;
    }
    return true;
}

std::string joinPath(const std::vector<std::string>& path) {
    std::string out;
    for (const std::string& p : path) {
        if (!out.empty()) out += '.';
        out += p;
    }
    return out;
}

}  // namespace

std::string pageId(std::string_view name) {
    std::string out = "page-";
    for (char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        out += ok ? c : '-';
    }
    return out;
}

RenderModel buildRenderModel(const Design& design) {
    RenderModel m;
    m.design = &design;

    m.kinds.reserve(design.components.size());
    for (const Component& c : design.components) m.kinds.push_back(classifySymbol(c));

    const std::string topName = design.top.empty() ? std::string("schematic") : design.top;

    if (design.blocks.empty()) {
        // A flat design -- or a cable, whose netlist carries no block records:
        // one page holding everything, exactly as before hierarchy existed.
        RenderPage page;
        page.id = pageId(design.top.empty() ? std::string("1") : design.top);
        page.title = topName;
        for (std::uint32_t i = 0; i < design.components.size(); ++i) {
            page.components.push_back(i);
        }
        partitionRooms(design, page);
        buildPageNets(design, page, nullptr);
        m.pages.push_back(std::move(page));
        return m;
    }

    // The top page: components and child instances sitting directly in the
    // top block.
    RenderPage top;
    top.id = pageId(design.top.empty() ? std::string("1") : design.top);
    top.title = topName;
    for (std::uint32_t i = 0; i < design.components.size(); ++i) {
        if (design.components[i].path.size() == 1) top.components.push_back(i);
    }
    for (std::uint32_t i = 0; i < design.blocks.size(); ++i) {
        if (design.blocks[i].path.size() == 1) top.children.push_back(i);
    }
    partitionRooms(design, top);
    buildPageNets(design, top, nullptr);
    m.pages.push_back(std::move(top));

    // One page per block DEFINITION, in first-appearance order, drawn from the
    // first instance. The later instances get no page: their sheet symbols on
    // the parent page link to this one.
    for (std::uint32_t r = 0; r < design.blocks.size(); ++r) {
        const BlockInstance& rep = design.blocks[r];
        bool seen = false;
        for (std::uint32_t j = 0; j < r; ++j) {
            if (design.blocks[j].block == rep.block) seen = true;
        }
        if (seen) continue;

        RenderPage page;
        page.id = pageId(rep.block);
        page.title = rep.block;
        page.definition = true;
        for (std::uint32_t i = 0; i < design.components.size(); ++i) {
            if (directlyUnder(design.components[i].path, rep.path)) page.components.push_back(i);
        }
        for (std::uint32_t i = 0; i < design.blocks.size(); ++i) {
            if (directlyUnder(design.blocks[i].path, rep.path)) page.children.push_back(i);
        }
        page.note = "instances: ";
        bool first = true;
        for (const BlockInstance& b : design.blocks) {
            if (b.block != rep.block) continue;
            if (!first) page.note += ", ";
            first = false;
            page.note += joinPath(b.path);
        }
        partitionRooms(design, page);
        buildPageNets(design, page, &rep);
        m.pages.push_back(std::move(page));
    }

    return m;
}

}  // namespace manta::render
