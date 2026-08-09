// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic placement: symbols packed shelf-wise, in Design order.
#pragma once

#include <string>
#include <vector>

#include "render/model.h"
#include "render/symbols.h"

namespace manta::render {

struct PlacedSymbol {
    std::uint32_t component = 0;  // index into Design::components
    SymbolGeom geom;
    int x = 0, y = 0;  // body top-left, sheet coordinates
};

// The title block never carries a timestamp or a version string: the HTML is
// inside the determinism guarantee of spec 15.8.
struct TitleBlock {
    std::string title;
    std::string sheet;  // "Sheet 1 of 1"
};

struct SheetLayout {
    int w = 0, h = 0;  // whole sheet, frame included
    std::vector<PlacedSymbol> symbols;
    TitleBlock tb;  // filled by the orchestrator
};

// The margin a pin's stub and mark need beyond the body edge, and the sheet
// packing built on top of it. Placement idioms (decoupling clusters, pull-up
// rails, rooms) later replace the shelf packer; the cell-extent arithmetic
// stays theirs to reuse.
[[nodiscard]] SheetLayout layoutPage(const RenderModel& model, const RenderPage& page);

}  // namespace manta::render
