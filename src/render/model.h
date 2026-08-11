// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The display model between the Design and the drawing.
#pragma once

#include <string>
#include <vector>

#include "link/netlist.h"
#include "render/classify.h"

namespace manta::render {

// How a net is shown at each pin stub. Grounds, rails and no-connects get a
// mark instead of a text label, which is most of what makes a sheet read like
// a schematic.
enum class NetMark : std::uint8_t { Label, Ground, Rail, NoConnect };

// One page's view of a design net. The display name is the page-local spelling
// -- a block page labels "BLK1.LED-ANODE" as "LED-ANODE" -- while data-net in
// the emitted HTML always carries the flat design-wide name, so a click on any
// page highlights the conductor on every page.
struct RenderNet {
    std::string display;  // the name drawn at each stub, page-local
    NetMark mark = NetMark::Label;
    // Pins in two or more rooms of its page: shown as a port flag, the same
    // shape a block port (direction != None) gets. Ground and rail marks win
    // over the flag -- a rail crossing rooms is still a rail.
    bool crossing = false;
    PortDir direction = PortDir::None;
};

// A titled rectangle on the sheet, grouping one section's components and the
// sheet symbols of child block instances placed under that section. The
// untitled room (empty title) collects what was placed before any marker;
// `framed` is false only when the whole page has no sections at all.
struct RenderRoom {
    std::string title;
    bool framed = true;
    std::vector<std::uint32_t> components;  // indices into Design::components
    std::vector<std::uint32_t> children;    // indices into Design::blocks
};

// One printed sheet: the top block, or one page per block DEFINITION drawn
// from its first instance (the representative). The 2nd..Nth instances draw
// no page of their own -- their sheet symbols on the parent page all link to
// the definition's single page.
struct RenderPage {
    std::string id;     // "page-<name>": the <section> id and sidebar anchor
    std::string title;
    std::string note;   // "instances: BLK1, BLK2" on a shared definition page
    bool definition = false;  // a block page: names and designators go local
    std::vector<std::uint32_t> components;  // indices into Design::components
    std::vector<std::uint32_t> children;    // indices into Design::blocks
    std::vector<RenderRoom> rooms;          // partition of both lists above
    std::vector<RenderNet> nets;            // parallel to Design::nets
};

struct RenderModel {
    const Design* design = nullptr;
    std::vector<SymbolKind> kinds;  // parallel to Design::components
    std::vector<RenderPage> pages;
};

// "page-<name>" with anything outside [A-Za-z0-9_-] replaced by '-'.
[[nodiscard]] std::string pageId(std::string_view name);

[[nodiscard]] RenderModel buildRenderModel(const Design& design);

}  // namespace manta::render
