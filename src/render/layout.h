// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Page layout: the orchestrator over the placement pipelines.
//
// Two pipelines produce a SheetLayout. Classic places by idiom -- a
// decoupling ladder under its rail bar, a pull-up standing over its anchor, a
// chain walking out of an anchor pin, a multi-way node's trunk with its taps
// -- and any connection no idiom claims is a 10u stub and a mark, which is
// both the reference style and always electrically correct. Flow (in
// progress, behind RenderOptions::Pipeline) ranks each room's signal flow
// first and routes room-locally, degrading to the same marks. Whichever
// pipeline runs, the debug invariants here hold: no two bodies overlap, and
// every placed pin has a conductor leaving it.
#pragma once

#include <string>
#include <vector>

#include "render/geometry.h"
#include "render/render.h"

namespace manta::render {

struct SidePlan;

struct RoomItem {
    int x = 0, y = 0, w = 0, h = 0;
    std::string title;   // already uppercased; empty for the untitled room
    bool framed = true;  // false only for the single sectionless region
};

// The title block never carries a timestamp or a version string: the HTML is
// inside the determinism guarantee of spec 15.8.
struct TitleBlock {
    std::string title;
    std::string sheet;  // "Sheet 1 of 1"
};

struct SheetLayout {
    int w = 0, h = 0;  // whole sheet, frame included
    std::vector<RoomItem> rooms;
    std::vector<PlacedSymbol> symbols;
    std::vector<SheetSymItem> children;
    std::vector<WireItem> wires;
    std::vector<DotItem> dots;
    std::vector<MarkItem> marks;
    std::vector<RailBarItem> bars;
    std::string note;  // "instances: ..." under the frame's top edge
    TitleBlock tb;     // filled by the orchestrator
};

// Symbol geometry for every component, indexed by component index, built once
// per page before placement so no placer rebuilds what another already
// measured. `plans` (parallel to components; an empty plan means "builtin
// heuristic") is the Flow pipeline's side assignment; null today.
using SymbolCache = std::vector<SymbolGeom>;
[[nodiscard]] SymbolCache buildSymbolCache(const RenderModel& m,
                                           const std::vector<SidePlan>* plans);

[[nodiscard]] SheetLayout layoutPage(const RenderModel& model, const RenderPage& page,
                                     RenderOptions::Pipeline pipeline);

}  // namespace manta::render
