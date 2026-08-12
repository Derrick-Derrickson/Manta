// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Shared placement geometry: the integer primitives every placer draws with.
//
// Everything here is an integer number of grid units: spec 15.8 demands
// byte-identical output, so no float ever enters the render path, and every
// container iterates in insertion order.
#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "render/model.h"
#include "render/symbols.h"

namespace manta::render {

constexpr int P = kPinPitch;
inline constexpr int kRoomPad = 2 * P;

[[nodiscard]] inline int textW(std::string_view s) {
    return kCharWidth * static_cast<int>(s.size());
}

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

[[nodiscard]] inline bool overlaps(const Rect& a, const Rect& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

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

// Body-local position of a geometry pin, before rotation.
void localPin(const SymbolGeom& g, const SymPin& p, int& x, int& y);

// Sheet position of a geometry pin, and the side its stub leaves on.
void pinPos(const PlacedSymbol& s, std::size_t gp, int& x, int& y, Side& side);

// The geometry pin carrying component pin `compPin`, -1 when the symbol does
// not expose it.
[[nodiscard]] int geomPinFor(const SymbolGeom& g, std::uint32_t compPin);

// Rotation that puts `compPin` at the TOP terminal of a two-terminal symbol.
[[nodiscard]] Rot verticalRot(const SymbolGeom& g, std::uint32_t compPin);

// A drawn conductor: an x,y polyline in sheet coordinates.
struct WireItem {
    std::vector<int> pts;
    std::int32_t net = -1;
};

// A filled junction dot. It belongs only where three or more conductors meet
// -- a ladder cap tapping its rail bar, a node's tap tapping its trunk. A
// corner in a wire, or two conductors joined end to end, takes none.
struct DotItem {
    int x = 0, y = 0;
    std::int32_t net = -1;
};

// What terminates a stub. `dir` is the direction the mark extends away from
// its attach point (the free end of the stub).
enum class MarkKind : std::uint8_t { Ground, RailFlag, Label, PortFlag, NoConnect };

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

// A child block instance drawn on its parent's page: the green sheet-symbol
// rectangle whose left-edge port entries connect like pins, wrapped in a link
// to the definition's page.
struct SheetSymItem {
    int x = 0, y = 0, w = 0, h = 0;
    std::uint32_t block = 0;  // index into Design::blocks
};

inline constexpr int kSheetSymHeader = 26;  // name strip inside the top edge

// The port rows are geometry both the layout and the SVG emitter derive from
// these, so the two can never disagree. Rows sit at double pitch, like a box
// symbol's Left/Right pins.
[[nodiscard]] constexpr int sheetSymPortY(int i) {
    return kSheetSymHeader + (i + 1) * 2 * kPinPitch;
}
[[nodiscard]] constexpr int sheetSymBodyH(int nPorts) {
    return kSheetSymHeader + (nPorts + 1) * 2 * kPinPitch;
}

// ---------------------------------------------------------------------------
// Net classes as the placers see them.
// ---------------------------------------------------------------------------

[[nodiscard]] bool isGround(const RenderPage& p, std::int32_t net);
[[nodiscard]] bool isRail(const RenderPage& p, std::int32_t net);

[[nodiscard]] MarkKind markKindFor(const RenderPage& p, std::int32_t net);

// A NC pin is a no-connect wherever it appears and whatever binding it carries
// -- including none at all, which is why this takes the pin and not just its
// net: an unbound NC pin has no net to classify (spec 11.6).
[[nodiscard]] MarkKind markKindForPin(const RenderPage& p, const SymPin& sp);

[[nodiscard]] const std::string& netName(const RenderPage& p, std::int32_t net);

// How far a horizontal strip runs past its last element to carry the mark
// that terminates it: a chain's end mark, a node trunk's terminal mark.
[[nodiscard]] int markTail(const RenderPage& p, std::int32_t net);

// How far a pin's stub and its mark reach beyond the body edge.
[[nodiscard]] int markExtent(const RenderPage& p, std::int32_t net, Side side);

// ---------------------------------------------------------------------------
// Junction counting.
// ---------------------------------------------------------------------------

// An axis-aligned conductor segment, for counting junctions.
struct Seg {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// Conductor ends meeting at a point: a segment ending there counts one, a
// segment passing straight through counts two (it continues both ways).
[[nodiscard]] int conductorsAt(const std::vector<Seg>& segs, int x, int y);

// Junction dots are counted, never assumed. Three or more conductor ends at a
// point is a junction and takes a dot; a corner and a plain end-to-end join
// both count two and take none. Points are visited in segment order, so the
// dot list is a function of the geometry alone (spec 15.8).
void addJunctionDots(std::vector<DotItem>& dots, const std::vector<Seg>& segs, std::int32_t net);

// ---------------------------------------------------------------------------
// Occupancy: a uniform grid-bucket index over rects.
// ---------------------------------------------------------------------------

// Each added rect's id lands in every 4P-square cell it covers; a query tests
// only the cells the probe covers, so a collision test stops being linear in
// the room's history. Buckets are row-major over the grid's current bounds
// and hold ids in insertion order; growth rebuilds the grid by re-inserting
// every rect in id order, so bucket contents are a function of the add
// sequence alone (spec 15.8).
struct Occupancy {
    static constexpr int kCell = 4 * P;

    void add(const Rect& r);
    [[nodiscard]] bool hits(const Rect& r) const;

private:
    // Floor division, explicit about negative coordinates.
    [[nodiscard]] static int cellOf(int v) {
        return v >= 0 ? v / kCell : -((-v - 1) / kCell) - 1;
    }
    void insert(std::uint32_t id, const Rect& r);

    std::vector<Rect> rects;
    std::vector<std::vector<std::uint32_t>> buckets;  // row-major: [row * cw + col]
    int cx0 = 0, cy0 = 0;  // cell coordinate of buckets[0]
    int cw = 0, ch = 0;    // grid size in cells
};

// The room's draw buffer, in room-content coordinates (0,0 top-left).
//
// Occupancy is two classes. Solids (bodies, text, marks, vertical cells) may
// never overlap anything. Wires may cross other wires -- that is ordinary
// schematic drawing -- but nothing solid may sit on a wire, and no wire may
// run through a solid.
struct RoomBuf {
    std::vector<PlacedSymbol> symbols;
    std::vector<SheetSymItem> children;
    std::vector<WireItem> wires;
    std::vector<DotItem> dots;
    std::vector<MarkItem> marks;
    std::vector<RailBarItem> bars;
    Occupancy solids;
    Occupancy wireRects;
    int maxX = 0, maxY = 0;

    void grow(const Rect& r) {
        maxX = std::max(maxX, r.x1);
        maxY = std::max(maxY, r.y1);
    }
    void reserve(const Rect& r) {
        solids.add(r);
        grow(r);
    }
    void reserveWire(const Rect& r) {
        wireRects.add(r);
        grow(r);
    }
    [[nodiscard]] bool collides(const Rect& r) const { return solids.hits(r); }
    [[nodiscard]] bool collidesAny(const Rect& r) const {
        return solids.hits(r) || wireRects.hits(r);
    }
    void wire(std::initializer_list<int> pts, std::int32_t net) {
        WireItem w;
        w.pts = pts;
        w.net = net;
        wires.push_back(std::move(w));
    }
    void mark(MarkKind kind, int x, int y, Side dir, std::int32_t net) {
        marks.push_back(MarkItem{kind, x, y, dir, net});
    }
};

}  // namespace manta::render
