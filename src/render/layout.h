// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Placement idioms instead of a router.
//
// Every drawn wire comes from an idiom that reserves its own space -- a
// decoupling ladder under its rail bar, a pull-up standing over its anchor, a
// chain walking out of an anchor pin. Any connection no idiom claims is a 10u
// stub and a mark (label, ground, rail or port flag), which is both the
// reference style and always electrically correct. If an idiom's wire cannot
// be placed without collision the connection downgrades to marks; correctness
// never depends on routing.
#pragma once

#include <string>
#include <vector>

#include "render/model.h"
#include "render/symbols.h"

namespace manta::render {

// Two-terminal symbols only ever rotate; everything else stays at R0. The
// geometry is kept pin-relative in SymbolGeom, so a rotation is pure integer
// point mapping at emission time.
enum class Rot : std::uint8_t { R0, R90, R180, R270 };

struct PlacedSymbol {
    std::uint32_t component = 0;  // index into Design::components
    SymbolGeom geom;
    int x = 0, y = 0;  // top-left of the *rotated* bounding box, sheet coords
    Rot rot = Rot::R0;
};

// Rotated bounding box, and a body-local point through the rotation.
[[nodiscard]] int rotatedW(const SymbolGeom& g, Rot r);
[[nodiscard]] int rotatedH(const SymbolGeom& g, Rot r);
void rotatePoint(const SymbolGeom& g, Rot r, int px, int py, int& ox, int& oy);
[[nodiscard]] Side rotatedSide(Side s, Rot r);

// A drawn conductor: an x,y polyline in sheet coordinates.
struct WireItem {
    std::vector<int> pts;
    std::int32_t net = -1;
};

// A junction dot where a ladder cap taps its rail bar.
struct DotItem {
    int x = 0, y = 0;
    std::int32_t net = -1;
};

// What terminates a stub. `dir` is the direction the mark extends away from
// its attach point (the free end of the stub).
enum class MarkKind : std::uint8_t { Ground, RailFlag, Label, PortFlag };

struct MarkItem {
    MarkKind kind = MarkKind::Label;
    int x = 0, y = 0;
    Side dir = Side::Right;
    std::int32_t net = -1;
};

// A horizontal rail bar with its name at the left end; ladder caps hang below.
struct RailBarItem {
    int x1 = 0, x2 = 0, y = 0;
    std::int32_t net = -1;
};

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
    std::vector<WireItem> wires;
    std::vector<DotItem> dots;
    std::vector<MarkItem> marks;
    std::vector<RailBarItem> bars;
    TitleBlock tb;  // filled by the orchestrator
};

[[nodiscard]] SheetLayout layoutPage(const RenderModel& model, const RenderPage& page);

}  // namespace manta::render
