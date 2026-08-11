// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/layout.h"

#include <cassert>

#include "render/place.h"
#include "render/place_classic.h"
#include "render/sides.h"

namespace manta::render {

// This is the one place outside symbols.cpp that may call buildSymbol: every
// placer reads the cache, so a page's geometry is measured exactly once and
// the reservation pass and the drawing pass can never disagree.
SymbolCache buildSymbolCache(const RenderModel& m, const std::vector<SidePlan>* plans) {
    const Design& d = *m.design;
    SymbolCache cache;
    cache.reserve(d.components.size());
    for (std::size_t i = 0; i < d.components.size(); ++i) {
        const SidePlan* plan = nullptr;
        if (plans && i < plans->size() && !(*plans)[i].byPin.empty()) plan = &(*plans)[i];
        cache.push_back(buildSymbol(d.components[i], m.kinds[i], plan));
    }
    return cache;
}

SheetLayout layoutPage(const RenderModel& model, const RenderPage& page,
                       RenderOptions::Pipeline pipeline) {
    // WP2 feeds per-component SidePlans in here; today every component keeps
    // the builtin side heuristic on both pipelines.
    SymbolCache cache = buildSymbolCache(model, nullptr);

    SheetLayout sheet = pipeline == RenderOptions::Pipeline::Flow
                            ? layoutPageFlow(model, page, cache)
                            : layoutPageClassic(model, page, cache);

#ifndef NDEBUG
    // The layout's own guarantee, whichever pipeline produced the sheet: no
    // two placed symbol bodies intersect. The release build trusts it; the
    // debug and sanitizer builds prove it on every render the test suite
    // makes.
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
#endif

    return sheet;
}

}  // namespace manta::render
