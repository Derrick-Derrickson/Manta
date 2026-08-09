// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The display model between the Design and the drawing.
#pragma once

#include <string>
#include <vector>

#include "link/netlist.h"
#include "render/classify.h"

namespace manta::render {

// How a net is shown at each pin stub. Grounds and rails get a mark instead of
// a text label, which is most of what makes a sheet read like a schematic.
enum class NetMark : std::uint8_t { Label, Ground, Rail };

struct RenderNet {
    std::string display;  // the name drawn at each stub
    NetMark mark = NetMark::Label;
};

// One printed sheet. The skeleton puts the whole design on one page; the
// hierarchy walk that gives each block instance its own page hangs off this,
// and the sidebar and print CSS already handle any number of them.
struct RenderPage {
    std::string id;     // "page-<name>": the <section> id and sidebar anchor
    std::string title;
    std::vector<std::uint32_t> components;  // indices into Design::components
};

struct RenderModel {
    const Design* design = nullptr;
    std::vector<RenderNet> nets;    // parallel to Design::nets
    std::vector<SymbolKind> kinds;  // parallel to Design::components
    std::vector<RenderPage> pages;
};

[[nodiscard]] RenderModel buildRenderModel(const Design& design);

}  // namespace manta::render
