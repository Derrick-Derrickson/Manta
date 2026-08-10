// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Parametric symbol geometry, on an integer grid.
//
// Every dimension here and downstream is an integer number of grid units:
// spec 15.8 demands byte-identical output, so no float ever enters the render
// path. Text widths are estimated from a monospace advance, which is why the
// emitted CSS pins the pin/label font to a monospace family.
#pragma once

#include <string>
#include <vector>

#include "link/netlist.h"
#include "render/classify.h"

namespace manta::render {

inline constexpr int kPinPitch = 10;  // grid units between adjacent pins
inline constexpr int kCharWidth = 6;  // monospace advance at font-size 10
inline constexpr int kStubLen = 10;   // wire stub from every pin to its mark

enum class Side : std::uint8_t { Left, Right, Top, Bottom };

struct SymPin {
    std::string number;      // the physical pin, drawn outside the body
    std::string name;        // the logical name, drawn inside
    Side side = Side::Left;
    int offset = 0;          // units along the side, from the top or left edge
    bool nc = false;         // drawn greyed
    std::uint32_t pin = 0;   // index into Component::pins
    std::int32_t net = -1;   // index into Design::nets, -1 when unconnected
};

// One stroke of a classic symbol, in body-local integer coordinates (the
// renderer translates by the placed position). Keeping the artwork relative
// to the body -- whose two terminals sit on the grid at (0, cy) and (w, cy) --
// is what lets a later layout stage rotate a symbol without touching this.
struct Prim {
    enum class Kind : std::uint8_t { Line, Polyline, Polygon, Circle, Arc, Text };
    Kind kind = Kind::Line;
    // Line/Polyline/Polygon: x,y pairs. Circle: {cx, cy, r}.
    // Arc: {x1, y1, x2, y2, r, sweep}. Text: {x, y}, anchored middle.
    std::vector<int> pts;
    bool fill = false;  // Polygon/Circle: filled with the wire colour
    std::string text;   // Text only
};

struct SymbolGeom {
    int w = 0, h = 0;  // body rectangle
    std::vector<SymPin> pins;
    // Reserved widths at the left end of the top and bottom edges, kept clear
    // of pins so the designator (top) and part name (bottom) have room.
    int topReserve = 0, botReserve = 0;
    // Non-empty for a classic symbol: drawn instead of the body rectangle,
    // with pin name/number text suppressed and the designator and value
    // placed at the spots below.
    std::vector<Prim> prims;
    // Classic only. Each builder knows which corners its stubs and net marks
    // leave free, so the text anchors are geometry, not renderer guesswork.
    // anchor: 0 = middle, 1 = end, 2 = start.
    struct LabelSpot {
        int x = 0, y = 0;
        std::uint8_t anchor = 0;
    };
    LabelSpot refdesAt, valueAt;
};

// "2" < "10", "A2" < "A10". Ties fall back to plain byte order.
[[nodiscard]] bool naturalLess(std::string_view a, std::string_view b);

[[nodiscard]] SymbolGeom buildSymbol(const Component& c, SymbolKind kind);

}  // namespace manta::render
