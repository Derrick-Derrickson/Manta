// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Page layout: the orchestrator over the flow placer.
//
// The placer ranks each room's signal flow -- sources on the left, loads on
// the right -- lays rank columns, hangs rails as bars with taps, and routes
// room-local nets; any connection it cannot draw degrades to a 10u stub and
// a mark, which is both the reference style and always electrically correct.
// The debug invariants here hold on every sheet: no two bodies overlap,
// every placed pin has a conductor leaving it, and the placed rooms of a
// multi-room page partition their bounding box exactly.
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
// per page before placement so the reservation pass and the drawing pass can
// never disagree. `plans` (parallel to components; an empty plan means
// "builtin heuristic") is the flow placer's side assignment, and `page`
// supplies the page-local net names a planned pin shows inside the body;
// null for both is byte-identically the builtin heuristic.
using SymbolCache = std::vector<SymbolGeom>;
[[nodiscard]] SymbolCache buildSymbolCache(const RenderModel& m,
                                           const std::vector<SidePlan>* plans,
                                           const RenderPage* page);

[[nodiscard]] SheetLayout layoutPage(const RenderModel& model, const RenderPage& page);

}  // namespace manta::render
