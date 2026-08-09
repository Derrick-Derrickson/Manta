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

bool isRailNet(const Net& n) {
    const NetDirective* c = n.directives.find("CLASS");
    return c && c->value == "power";
}

}  // namespace

RenderModel buildRenderModel(const Design& design) {
    RenderModel m;
    m.design = &design;

    m.nets.reserve(design.nets.size());
    for (const Net& n : design.nets) {
        RenderNet rn;
        rn.display = n.name;
        if (isGroundNet(design, n)) rn.mark = NetMark::Ground;
        else if (isRailNet(n)) rn.mark = NetMark::Rail;
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
    m.pages.push_back(std::move(page));

    return m;
}

}  // namespace manta::render
