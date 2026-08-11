// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/geometry.h"

namespace manta::render {

// ---------------------------------------------------------------------------
// Rotation. Geometry stays pin-relative; only points map.
// ---------------------------------------------------------------------------

void localPin(const SymbolGeom& g, const SymPin& p, int& x, int& y) {
    switch (p.side) {
        case Side::Left: x = 0, y = p.offset; break;
        case Side::Right: x = g.w, y = p.offset; break;
        case Side::Top: x = p.offset, y = 0; break;
        case Side::Bottom: x = p.offset, y = g.h; break;
    }
}

int rotatedW(const SymbolGeom& g, Rot r) {
    return r == Rot::R90 || r == Rot::R270 ? g.h : g.w;
}

int rotatedH(const SymbolGeom& g, Rot r) {
    return r == Rot::R90 || r == Rot::R270 ? g.w : g.h;
}

void rotatePoint(const SymbolGeom& g, Rot r, int px, int py, int& ox, int& oy) {
    switch (r) {
        case Rot::R0: ox = px, oy = py; break;
        case Rot::R90: ox = g.h - py, oy = px; break;
        case Rot::R180: ox = g.w - px, oy = g.h - py; break;
        case Rot::R270: ox = py, oy = g.w - px; break;
    }
}

Side rotatedSide(Side s, Rot r) {
    static constexpr Side kMap[4][4] = {
        // L            R             T            B
        {Side::Left, Side::Right, Side::Top, Side::Bottom},     // R0
        {Side::Top, Side::Bottom, Side::Right, Side::Left},     // R90
        {Side::Right, Side::Left, Side::Bottom, Side::Top},     // R180
        {Side::Bottom, Side::Top, Side::Left, Side::Right},     // R270
    };
    return kMap[static_cast<int>(r)][static_cast<int>(s)];
}

void pinPos(const PlacedSymbol& s, std::size_t gp, int& x, int& y, Side& side) {
    int lx = 0, ly = 0, rx = 0, ry = 0;
    localPin(s.geom, s.geom.pins[gp], lx, ly);
    rotatePoint(s.geom, s.rot, lx, ly, rx, ry);
    x = s.x + rx;
    y = s.y + ry;
    side = rotatedSide(s.geom.pins[gp].side, s.rot);
}

int geomPinFor(const SymbolGeom& g, std::uint32_t compPin) {
    for (std::size_t i = 0; i < g.pins.size(); ++i) {
        if (g.pins[i].pin == compPin) return static_cast<int>(i);
    }
    return -1;
}

Rot verticalRot(const SymbolGeom& g, std::uint32_t compPin) {
    return !g.pins.empty() && g.pins[0].pin == compPin ? Rot::R90 : Rot::R270;
}

// ---------------------------------------------------------------------------
// Net classes as the placers see them.
// ---------------------------------------------------------------------------

bool isGround(const RenderPage& p, std::int32_t net) {
    return net >= 0 && p.nets[static_cast<std::size_t>(net)].mark == NetMark::Ground;
}

bool isRail(const RenderPage& p, std::int32_t net) {
    return net >= 0 && p.nets[static_cast<std::size_t>(net)].mark == NetMark::Rail;
}

MarkKind markKindFor(const RenderPage& p, std::int32_t net) {
    const RenderNet& rn = p.nets[static_cast<std::size_t>(net)];
    if (rn.mark == NetMark::NoConnect) return MarkKind::NoConnect;
    if (rn.mark == NetMark::Ground) return MarkKind::Ground;
    if (rn.mark == NetMark::Rail) return MarkKind::RailFlag;
    // Only a block-port net -- a signal crossing pages -- earns the flag. A
    // net merely crossing rooms of one page keeps the plain label; the
    // crossing bit still exists on RenderNet for the placers to weigh.
    if (rn.direction != PortDir::None) return MarkKind::PortFlag;
    return MarkKind::Label;
}

MarkKind markKindForPin(const RenderPage& p, const SymPin& sp) {
    return sp.nc ? MarkKind::NoConnect : markKindFor(p, sp.net);
}

const std::string& netName(const RenderPage& p, std::int32_t net) {
    return p.nets[static_cast<std::size_t>(net)].display;
}

int markTail(const RenderPage& p, std::int32_t net) {
    if (net < 0) return P;
    switch (markKindFor(p, net)) {
        case MarkKind::Ground:
        case MarkKind::NoConnect:  // the cross, no label: the same tail as a glyph
        case MarkKind::RailFlag: return P + 10;
        case MarkKind::Label: return P + textW(netName(p, net)) + 6;
        case MarkKind::PortFlag: return P + textW(netName(p, net)) + 18;
    }
    return P;
}

int markExtent(const RenderPage& p, std::int32_t net, Side side) {
    if (net < 0) return kStubLen + 4;
    bool vertical = side == Side::Top || side == Side::Bottom;
    int nameW = textW(netName(p, net));
    switch (markKindFor(p, net)) {
        case MarkKind::Ground: return vertical ? kStubLen + 10 : kStubLen + 14;
        case MarkKind::RailFlag: return vertical ? kStubLen + 18 : kStubLen + nameW + 10;
        case MarkKind::PortFlag: return vertical ? kStubLen + 22 : kStubLen + nameW + 18;
        case MarkKind::Label: return vertical ? kStubLen + 16 : kStubLen + nameW + 8;
        case MarkKind::NoConnect: return kStubLen + 4;  // the cross, no label
    }
    return kStubLen + 4;
}

// ---------------------------------------------------------------------------
// Junction counting.
// ---------------------------------------------------------------------------

int conductorsAt(const std::vector<Seg>& segs, int x, int y) {
    int n = 0;
    for (const Seg& s : segs) {
        if (s.x1 == s.x2 && s.y1 == s.y2) continue;  // degenerate: not a conductor
        if ((s.x1 == x && s.y1 == y) || (s.x2 == x && s.y2 == y)) {
            n += 1;
        } else if (s.x1 == s.x2 && x == s.x1 && y > std::min(s.y1, s.y2) &&
                   y < std::max(s.y1, s.y2)) {
            n += 2;
        } else if (s.y1 == s.y2 && y == s.y1 && x > std::min(s.x1, s.x2) &&
                   x < std::max(s.x1, s.x2)) {
            n += 2;
        }
    }
    return n;
}

void addJunctionDots(std::vector<DotItem>& dots, const std::vector<Seg>& segs,
                     std::int32_t net) {
    std::vector<Seg> seen;  // reused as a point list: (x1,y1) is the point
    for (const Seg& s : segs) {
        const int px[2] = {s.x1, s.x2};
        const int py[2] = {s.y1, s.y2};
        for (int e = 0; e < 2; ++e) {
            bool dup = false;
            for (const Seg& p : seen) {
                if (p.x1 == px[e] && p.y1 == py[e]) dup = true;
            }
            if (dup) continue;
            seen.push_back(Seg{px[e], py[e], 0, 0});
            if (conductorsAt(segs, px[e], py[e]) >= 3) {
                dots.push_back(DotItem{px[e], py[e], net});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Occupancy.
// ---------------------------------------------------------------------------

void Occupancy::insert(std::uint32_t id, const Rect& r) {
    // `overlaps` compares with strict <, so a rect occupies cells only up to
    // cellOf(x1 - 1) -- but a degenerate rect (x0 == x1) can still overlap a
    // probe that strictly contains its edge, so the range always covers at
    // least the cell of x0. The same clamp on the query side makes the two
    // meet in that cell.
    int cxa = cellOf(r.x0), cxb = cellOf(std::max(r.x0, r.x1 - 1));
    int cya = cellOf(r.y0), cyb = cellOf(std::max(r.y0, r.y1 - 1));
    for (int cy = cya; cy <= cyb; ++cy) {
        for (int cx = cxa; cx <= cxb; ++cx) {
            buckets[static_cast<std::size_t>(cy - cy0) * static_cast<std::size_t>(cw) +
                    static_cast<std::size_t>(cx - cx0)]
                .push_back(id);
        }
    }
}

void Occupancy::add(const Rect& r) {
    int cxa = cellOf(r.x0), cxb = cellOf(std::max(r.x0, r.x1 - 1));
    int cya = cellOf(r.y0), cyb = cellOf(std::max(r.y0, r.y1 - 1));

    if (cw == 0 || cxa < cx0 || cya < cy0 || cxb >= cx0 + cw || cyb >= cy0 + ch) {
        // Grow to the union, padded four cells each way so steady downward or
        // rightward growth -- the placers' one collision response -- does not
        // rebuild on every add. Rebuilding re-inserts in id order, which keeps
        // every bucket in insertion order.
        int nx0 = cw == 0 ? cxa : std::min(cx0, cxa);
        int ny0 = cw == 0 ? cya : std::min(cy0, cya);
        int nx1 = cw == 0 ? cxb : std::max(cx0 + cw - 1, cxb);
        int ny1 = cw == 0 ? cyb : std::max(cy0 + ch - 1, cyb);
        cx0 = nx0 - 4;
        cy0 = ny0 - 4;
        cw = (nx1 + 4) - cx0 + 1;
        ch = (ny1 + 4) - cy0 + 1;
        buckets.assign(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch), {});
        for (std::size_t i = 0; i < rects.size(); ++i) {
            insert(static_cast<std::uint32_t>(i), rects[i]);
        }
    }

    std::uint32_t id = static_cast<std::uint32_t>(rects.size());
    rects.push_back(r);
    insert(id, r);
}

bool Occupancy::hits(const Rect& r) const {
    if (cw == 0) return false;
    int cxa = std::max(cellOf(r.x0), cx0);
    int cxb = std::min(cellOf(std::max(r.x0, r.x1 - 1)), cx0 + cw - 1);
    int cya = std::max(cellOf(r.y0), cy0);
    int cyb = std::min(cellOf(std::max(r.y0, r.y1 - 1)), cy0 + ch - 1);
    for (int cy = cya; cy <= cyb; ++cy) {
        for (int cx = cxa; cx <= cxb; ++cx) {
            const auto& bucket =
                buckets[static_cast<std::size_t>(cy - cy0) * static_cast<std::size_t>(cw) +
                        static_cast<std::size_t>(cx - cx0)];
            for (std::uint32_t id : bucket) {
                if (overlaps(rects[id], r)) return true;
            }
        }
    }
    return false;
}

}  // namespace manta::render
