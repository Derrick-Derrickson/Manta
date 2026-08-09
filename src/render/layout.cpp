// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/layout.h"

#include <algorithm>

namespace manta::render {

namespace {

constexpr int kMargin = 44;    // sheet edge to content: frame band plus padding
constexpr int kWrapW = 1160;   // content width before a row wraps
constexpr int kGapX = 30;      // between cells in a row
constexpr int kGapY = 44;      // between rows
constexpr int kTitleBlockH = 70;

// How far a pin's stub and its mark reach beyond the body edge.
int markExtent(const RenderModel& m, const SymPin& p) {
    if (p.net < 0) return kStubLen + 4;
    const RenderNet& rn = m.nets[static_cast<std::size_t>(p.net)];
    int nameW = kCharWidth * static_cast<int>(rn.display.size());
    bool vertical = p.side == Side::Top || p.side == Side::Bottom;
    switch (rn.mark) {
        case NetMark::Ground: return vertical ? kStubLen + 18 : kStubLen + 12;
        // A vertical rail flag is a bar with the name above it; a horizontal
        // one reads like a label with a bar at the wire end.
        case NetMark::Rail: return vertical ? kStubLen + 22 : kStubLen + nameW + 10;
        case NetMark::Label: break;
    }
    return vertical ? kStubLen + 16 : kStubLen + nameW + 8;
}

struct Extents {
    int l = 0, r = 0, t = 14, b = 14;  // 14: designator above, part name below
};

Extents cellExtents(const RenderModel& m, const SymbolGeom& g) {
    Extents e;
    for (const SymPin& p : g.pins) {
        int ext = markExtent(m, p);
        switch (p.side) {
            case Side::Left: e.l = std::max(e.l, ext); break;
            case Side::Right: e.r = std::max(e.r, ext); break;
            case Side::Top: e.t = std::max(e.t, ext); break;
            case Side::Bottom: e.b = std::max(e.b, ext); break;
        }
    }
    return e;
}

}  // namespace

SheetLayout layoutPage(const RenderModel& model, const RenderPage& page) {
    SheetLayout sheet;
    const Design& design = *model.design;

    int x = kMargin;
    int y = kMargin + 6;
    int rowH = 0;
    int maxX = kMargin;

    for (std::uint32_t idx : page.components) {
        PlacedSymbol placed;
        placed.component = idx;
        placed.geom = buildSymbol(design.components[idx], model.kinds[idx]);
        Extents e = cellExtents(model, placed.geom);

        int cw = e.l + placed.geom.w + e.r;
        int ch = e.t + placed.geom.h + e.b;
        if (x > kMargin && x + cw > kMargin + kWrapW) {
            x = kMargin;
            y += rowH + kGapY;
            rowH = 0;
        }
        placed.x = x + e.l;
        placed.y = y + e.t;
        maxX = std::max(maxX, x + cw);
        x += cw + kGapX;
        rowH = std::max(rowH, ch);
        sheet.symbols.push_back(std::move(placed));
    }

    sheet.w = std::max(maxX + kMargin, 680);
    sheet.h = y + rowH + kMargin + kTitleBlockH;
    return sheet;
}

}  // namespace manta::render
