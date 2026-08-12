// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/layout.h"

#include <algorithm>
#include <cassert>

#include "render/place.h"
#include "render/sides.h"

namespace manta::render {

// This is the one place outside symbols.cpp that may call buildSymbol: every
// placer reads the cache, so a page's geometry is measured exactly once and
// the reservation pass and the drawing pass can never disagree.
SymbolCache buildSymbolCache(const RenderModel& m, const std::vector<SidePlan>* plans,
                             const RenderPage* page) {
    const Design& d = *m.design;
    SymbolCache cache;
    cache.reserve(d.components.size());
    for (std::size_t i = 0; i < d.components.size(); ++i) {
        const SidePlan* plan = nullptr;
        if (plans && i < plans->size() && !(*plans)[i].byPin.empty()) plan = &(*plans)[i];
        // The page matters only alongside a plan (it feeds the net names the
        // plan asked to show); planless symbols stay the builtin heuristic.
        cache.push_back(buildSymbol(d.components[i], m.kinds[i], plan, plan ? page : nullptr));
    }
    return cache;
}

SheetLayout layoutPage(const RenderModel& model, const RenderPage& page) {
    // The flow placer plans pin sides from each room's flow graph before any
    // geometry is measured, so it builds its own per-page cache.
    SheetLayout sheet = layoutPageFlow(model, page);

#ifndef NDEBUG
    // The layout's own guarantee: no two placed symbol bodies intersect. The
    // release build trusts it; the debug and sanitizer builds prove it on
    // every render the test suite makes.
    for (std::size_t i = 0; i < sheet.symbols.size(); ++i) {
        const PlacedSymbol& a = sheet.symbols[i];
        Rect ra{a.x, a.y, a.x + rotatedW(a.geom, a.rot), a.y + rotatedH(a.geom, a.rot)};
        for (std::size_t j = i + 1; j < sheet.symbols.size(); ++j) {
            const PlacedSymbol& b = sheet.symbols[j];
            Rect rb{b.x, b.y, b.x + rotatedW(b.geom, b.rot), b.y + rotatedH(b.geom, b.rot)};
            assert(!overlaps(ra, rb) && "two symbol bodies overlap");
        }
    }

    // The guarantee an idiom's fallback exists to protect: a pin showing
    // neither a wire nor a mark shows no connection at all, which is the one
    // outcome no degradation may produce. Every placed pin therefore has a
    // conductor leaving it -- a stub to its mark, a leg to a node's spine, a
    // tap off a trunk, or a chain's own wire.
    for (const PlacedSymbol& s : sheet.symbols) {
        for (std::size_t gp = 0; gp < s.geom.pins.size(); ++gp) {
            int px = 0, py = 0;
            Side side = Side::Left;
            pinPos(s, gp, px, py, side);
            bool wired = false;
            for (const WireItem& w : sheet.wires) {
                for (std::size_t i = 0; i + 1 < w.pts.size(); i += 2) {
                    if (w.pts[i] == px && w.pts[i + 1] == py) wired = true;
                }
            }
            assert(wired && "a placed pin has no conductor leaving it");
        }
    }

    // The placed room rectangles of a multi-room page partition their
    // bounding box exactly -- pairwise disjoint, areas summing to the
    // bounding box's area. tileRooms guarantees it; this proves the plumbing
    // from RoomPlace to RoomItem kept it.
    if (sheet.rooms.size() >= 2) {
        int x0 = sheet.rooms[0].x, y0 = sheet.rooms[0].y;
        int x1 = x0, y1 = y0;
        long long sum = 0;
        for (std::size_t i = 0; i < sheet.rooms.size(); ++i) {
            const RoomItem& r = sheet.rooms[i];
            x0 = std::min(x0, r.x);
            y0 = std::min(y0, r.y);
            x1 = std::max(x1, r.x + r.w);
            y1 = std::max(y1, r.y + r.h);
            sum += static_cast<long long>(r.w) * r.h;
            Rect ra{r.x, r.y, r.x + r.w, r.y + r.h};
            for (std::size_t j = i + 1; j < sheet.rooms.size(); ++j) {
                const RoomItem& o = sheet.rooms[j];
                Rect rb{o.x, o.y, o.x + o.w, o.y + o.h};
                assert(!overlaps(ra, rb) && "two placed rooms overlap");
            }
        }
        assert(sum == static_cast<long long>(x1 - x0) * (y1 - y0) &&
               "the placed rooms do not partition their bounding box");
    }
#endif

    return sheet;
}

}  // namespace manta::render
