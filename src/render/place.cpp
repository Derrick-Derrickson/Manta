// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WP4: the flow placer. Each room is laid out from its RoomFlow -- rank
// columns left to right, barycentre order top to bottom -- instead of the
// classic bands. Rails with two or more consumers become one horizontal bar
// spanning the room, named once at its left end (the SVG emitter draws the
// name from the RailBarItem itself), with decoupling ladders hanging from it
// and taps dropping to top-side consumer pins. Room-local label nets are
// routed by routeNet; a net the router declines keeps stub+label, which is
// always electrically correct. Rooms are tiled by tileRooms, whose placed
// rectangles partition their bounding box exactly.
//
// Structural choices, documented per the work package:
//   - Rail bars are two-pass: the bars and their ladders go in first at the
//     top of the room, the columns after, and only then is each bar
//     stretched to the room's content width and its taps joined -- straight
//     corridor drop first, routed fallback second, and only when both refuse
//     the classic per-pin rail flag. Every bar-rail consumer taps: top pins
//     by a riser to the cell's top edge, pull-up tops, series-string ends,
//     and side pins by their bare stub. Bars sit at y = 12 (mod P), never on
//     the P routing grid, so a routed wire can never run collinearly along a
//     bar.
//   - Bare stubs (pins of nets the router will attempt) are snapped OUTWARD
//     to the P grid, since routeNet refuses off-grid endpoints; marked stubs
//     keep the classic kStubLen.
//   - Series vertices sit in their rank column at their barycentre order --
//     the "y of its context" refinement is left to the router, which joins
//     the string's end nets across the gutters.
//   - The alignment pass is a group-max prepass on each vertex's body
//     offset: vertices with identical (SymbolKind of comps[0], partName,
//     Kind, rank) share the maximum of their natural offsets. Cells stack
//     without overlap by construction, so the snap can never collide.
//   - Route fallback marks are added after routing, into space the cell
//     extents kept free of every body; a routed foreign wire may in rare
//     crowded cases pass under such a late label, which is cosmetic only.
//
// Determinism (spec 15.8): integer math, vectors in index/insertion order,
// explicit tie-breaks; nothing here reads an unordered container or a float.
#include "render/place.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "render/flow.h"
#include "render/route.h"
#include "render/sides.h"
#include "render/tile.h"

namespace manta::render {

namespace {

constexpr int kSheetMargin = 40;
constexpr int kTitleStrip = 14;   // the room's blue title band
constexpr int kTitleBlockH = 70;

[[nodiscard]] int roundUpP(int v) { return (v + P - 1) / P * P; }
[[nodiscard]] int roundDownP(int v) { return v >= 0 ? v / P * P : -roundUpP(-v); }

// Integer ceil(sqrt(v)).
std::int64_t isqrtCeil(std::int64_t v) {
    if (v <= 0) return 0;
    std::int64_t r = 1;
    while (r * r < v) {
        std::int64_t next = (r + v / r) / 2;
        if (next >= r) break;
        r = next;
    }
    while (r * r < v) ++r;
    while (r > 1 && (r - 1) * (r - 1) >= v) --r;
    return r;
}

// Shift a finished room to its sheet position.
void translate(RoomBuf& b, int ox, int oy) {
    for (PlacedSymbol& s : b.symbols) {
        s.x += ox;
        s.y += oy;
    }
    for (SheetSymItem& c : b.children) {
        c.x += ox;
        c.y += oy;
    }
    for (WireItem& w : b.wires) {
        for (std::size_t i = 0; i + 1 < w.pts.size(); i += 2) {
            w.pts[i] += ox;
            w.pts[i + 1] += oy;
        }
    }
    for (DotItem& d : b.dots) {
        d.x += ox;
        d.y += oy;
    }
    for (MarkItem& mk : b.marks) {
        mk.x += ox;
        mk.y += oy;
    }
    for (RailBarItem& bar : b.bars) {
        bar.x1 += ox;
        bar.x2 += ox;
        bar.y += oy;
    }
}

std::string upperCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    return out;
}

// What a Vertical vertex stands for. Re-derived from the component's two
// nets, exactly as flow.cpp classified it (RoomFlow does not export roles).
enum class VRole : std::uint8_t { Ladder, PullUp, PullDown, Other };

// ---------------------------------------------------------------------------
// One room, placed from its flow graph.
// ---------------------------------------------------------------------------

struct RoomPlacer {
    const RenderModel& m;
    const RenderPage& pg;
    const Design& d;
    const SymbolCache& cache;
    const RenderRoom& room;
    const RoomFlow& flow;
    RoomBuf buf;

    RoomPlacer(const RenderModel& model, const RenderPage& page, const SymbolCache& geoms,
               const RenderRoom& rm, const RoomFlow& fl)
        : m(model), pg(page), d(*model.design), cache(geoms), room(rm), flow(fl) {}

    void run();

private:
    struct StubPt {
        int x = 0, y = 0;
        Side side = Side::Right;
    };
    // One rail-bar consumer. The placer pre-draws the riser from the pin to
    // the tap point -- a grid point on the cell's top edge (or a side stub's
    // free end) that no solid covers -- so finishBars can join it to the bar
    // three ways: the straight corridor drop, the routed fallback, or, when
    // both fail, the classic per-pin rail flag drawn at the tap point.
    struct Tap {
        int px = 0, py = 0;  // the tap point, room coords, on the P grid
        std::int32_t net = -1;
        Side fbSide = Side::Top;  // the failure mark's direction
    };
    // One element of a series string, with its orientation resolved so the
    // entry pin faces the string's source (placeRun's R0/R180 rule).
    struct SeriesElem {
        std::uint32_t comp = 0;
        std::uint32_t entryPin = 0;  // component pin facing the source
        Rot rot = Rot::R0;
    };
    struct SeriesGeom {
        std::vector<SeriesElem> elems;
        std::int32_t startNet = -1, endNet = -1;
        int up = 16, dn = 14;  // strip half-heights around the pin row
    };

    std::vector<char> inRoom;                 // component -> member of this room
    std::vector<char> routable;               // net -> the router may claim it
    std::vector<std::vector<StubPt>> netPts;  // net -> bare stub ends collected
    std::vector<std::int32_t> barOf;          // net -> index into buf.bars, -1
    std::vector<Tap> taps;
    std::vector<char> vertPlaced;             // vertex -> consumed by the bar phase
    std::vector<VRole> vrole;                 // vertex -> vertical sub-role
    std::vector<std::uint32_t> vTopPin;       // vertex -> component pin facing up
    std::vector<std::int32_t> vTopNet, vBotNet;
    std::vector<SeriesGeom> sgeom;            // vertex -> series drawing plan
    std::vector<int> effOff;                  // vertex -> aligned body offset

    [[nodiscard]] bool isRoutable(std::int32_t net) const {
        return net >= 0 && routable[static_cast<std::size_t>(net)] != 0;
    }

    void classifyVerticals();
    void computeRoutable();
    int placeBars();
    void computeMetrics();
    void placeColumns(int yStart);
    void finishBars();
    void routeAll();

    void pinStub(const PlacedSymbol& s, std::size_t gp);
    void placeBody(std::uint32_t vi, int colX, int off, int y, int& cellW, int& cellH);
    void placeVerticalCell(std::uint32_t vi, int colX, int off, int y, int& cellW, int& cellH);
    void placeSeries(std::uint32_t vi, int colX, int off, int y, int& cellW, int& cellH);
    void placeChild(std::uint32_t vi, int colX, int off, int y, int& cellW, int& cellH);

    [[nodiscard]] int verticalCellW(std::int32_t topNet, std::int32_t botNet) const;
    void bodyExtents(std::uint32_t comp, int& extL, int& extR, int& extT, int& extB) const;
};

void RoomPlacer::classifyVerticals() {
    const std::size_t nv = flow.verts.size();
    vrole.assign(nv, VRole::Other);
    vTopPin.assign(nv, 0);
    vTopNet.assign(nv, -1);
    vBotNet.assign(nv, -1);
    vertPlaced.assign(nv, 0);
    for (std::size_t vi = 0; vi < nv; ++vi) {
        const FlowVertex& v = flow.verts[vi];
        if (v.kind != FlowVertex::Kind::Vertical) continue;
        const Component& c = d.components[v.comps[0]];
        if (c.pins.size() != 2) continue;
        std::int32_t a = c.pins[0].net, b = c.pins[1].net;
        bool railA = isRail(pg, a), railB = isRail(pg, b);
        bool gndA = isGround(pg, a), gndB = isGround(pg, b);
        if ((railA && gndB) || (railB && gndA)) {
            vrole[vi] = VRole::Ladder;
            vTopPin[vi] = railA ? 0u : 1u;
            vTopNet[vi] = railA ? a : b;
            vBotNet[vi] = railA ? b : a;
        } else if (railA != railB && !gndA && !gndB) {
            vrole[vi] = VRole::PullUp;
            vTopPin[vi] = railA ? 0u : 1u;
            vTopNet[vi] = railA ? a : b;
            vBotNet[vi] = railA ? b : a;
        } else if (gndA != gndB && !railA && !railB) {
            vrole[vi] = VRole::PullDown;
            vTopPin[vi] = gndA ? 1u : 0u;  // the signal pin faces up
            vTopNet[vi] = gndA ? b : a;
            vBotNet[vi] = gndA ? a : b;
        } else {
            vTopPin[vi] = 0;
            vTopNet[vi] = a;
            vBotNet[vi] = b;
        }
    }
}

// A net the router may claim: plain room-local label net, two or more pins,
// every pin a live pin of this room's components, and no block port anywhere
// on it (a routed wire with no name at a port's mark would leave the reader
// unable to join the halves -- the same caution classic's node idiom takes).
void RoomPlacer::computeRoutable() {
    inRoom.assign(d.components.size(), 0);
    for (std::uint32_t idx : room.components) inRoom[idx] = 1;

    routable.assign(pg.nets.size(), 0);
    netPts.assign(pg.nets.size(), {});
    for (std::size_t ni = 0; ni < d.nets.size(); ++ni) {
        const RenderNet& rn = pg.nets[ni];
        if (rn.mark != NetMark::Label || rn.crossing || rn.direction != PortDir::None) continue;
        const Net& n = d.nets[ni];
        if (n.pins.size() < 2) continue;
        bool ok = true;
        for (const PinRef& pr : n.pins) {
            if (!inRoom[pr.component]) {
                ok = false;
                break;
            }
            const ComponentPin& p = d.components[pr.component].pins[pr.pin];
            if (p.type == PinType::NC || p.unbound) {
                ok = false;
                break;
            }
        }
        for (std::size_t bi = 0; ok && bi < d.blocks.size(); ++bi) {
            for (const BlockPort& p : d.blocks[bi].ports) {
                if (p.net == static_cast<std::int32_t>(ni)) ok = false;
            }
        }
        if (ok) routable[ni] = 1;
    }
}

// ---------------------------------------------------------------------------
// Phase 1: rail bars and their ladders, at the top of the room. The ladder
// drawing is the classic one (bar tap dot, hanging body, ground below).
// ---------------------------------------------------------------------------

int RoomPlacer::placeBars() {
    barOf.assign(pg.nets.size(), -1);
    int yCur = 0;
    for (std::int32_t rail : flow.roomRails) {
        // Pins, not components: an MCU drinking a rail through five supply
        // pins wants the bar exactly as much as five separate consumers do.
        int touch = 0;
        for (std::uint32_t idx : room.components) {
            for (const ComponentPin& p : d.components[idx].pins) {
                if (p.net == rail) ++touch;
            }
        }
        if (touch < 2) continue;  // a single consumer pin keeps its flag

        // 12 above a P-multiple: never on the routing grid, so no routed wire
        // can ever run collinearly along the bar.
        const int barY = yCur + 12;
        barOf[static_cast<std::size_t>(rail)] = static_cast<std::int32_t>(buf.bars.size());
        buf.bars.push_back(RailBarItem{0, 2 * P, barY, rail});

        int tapX = 2 * P;
        int bottom = barY;
        int nCaps = 0;
        for (std::size_t vi = 0; vi < flow.verts.size(); ++vi) {
            if (flow.verts[vi].kind != FlowVertex::Kind::Vertical) continue;
            if (vrole[vi] != VRole::Ladder || vTopNet[vi] != rail || vertPlaced[vi]) continue;
            vertPlaced[vi] = 1;
            ++nCaps;
            std::uint32_t comp = flow.verts[vi].comps[0];
            buf.dots.push_back(DotItem{tapX, barY, rail});
            buf.wire({tapX, barY, tapX, barY + P}, rail);
            PlacedSymbol s;
            s.component = comp;
            s.geom = cache[comp];
            s.rot = verticalRot(s.geom, vTopPin[vi]);
            int gp = geomPinFor(s.geom, vTopPin[vi]);
            int lx = 0, ly = 0, rx = 0, ry = 0;
            localPin(s.geom, s.geom.pins[static_cast<std::size_t>(gp)], lx, ly);
            rotatePoint(s.geom, s.rot, lx, ly, rx, ry);
            s.x = tapX - rx;
            s.y = barY + P;
            int bodyBot = s.y + rotatedH(s.geom, s.rot);
            std::int32_t gnd = vBotNet[vi];
            buf.wire({tapX, bodyBot, tapX, bodyBot + P}, gnd);
            buf.mark(MarkKind::Ground, tapX, bodyBot + P, Side::Bottom, gnd);
            buf.symbols.push_back(std::move(s));
            bottom = std::max(bottom, bodyBot + P + 10);
            tapX += 3 * P;
        }

        if (nCaps > 0) {
            buf.reserve(Rect{0, yCur, (tapX - 3 * P) + 2 * P + 30, bottom + 4});
            yCur = roundUpP(bottom + 4) + P;
        } else {
            // A bar with taps only: reserve just the name at the left end.
            buf.reserve(Rect{0, yCur, textW(netName(pg, rail)) + 10, barY + 4});
            yCur = roundUpP(barY + 4) + P;
        }
    }
    return yCur == 0 ? 0 : yCur + P;
}

// ---------------------------------------------------------------------------
// Cell metrics and the alignment prepass.
// ---------------------------------------------------------------------------

void RoomPlacer::bodyExtents(std::uint32_t comp, int& extL, int& extR, int& extT,
                             int& extB) const {
    const SymbolGeom& g = cache[comp];
    extL = extR = 0;
    extT = extB = 14;
    for (const SymPin& p : g.pins) {
        int e = markExtent(pg, p.net, p.side);
        switch (p.side) {
            case Side::Left: extL = std::max(extL, e); break;
            case Side::Right: extR = std::max(extR, e); break;
            case Side::Top: extT = std::max(extT, e); break;
            case Side::Bottom: extB = std::max(extB, e); break;
        }
    }
    extL = roundUpP(std::max(extL, kStubLen + 4));
    extR = roundUpP(std::max(extR, kStubLen + 4));
    extT = roundUpP(extT);
    extB = roundUpP(extB);
}

int RoomPlacer::verticalCellW(std::int32_t topNet, std::int32_t botNet) const {
    int w = 3 * P;
    if (topNet >= 0) w = std::max(w, textW(netName(pg, topNet)) + 4);
    if (botNet >= 0) w = std::max(w, textW(netName(pg, botNet)) + 4);
    return w;
}

void RoomPlacer::computeMetrics() {
    const std::size_t nv = flow.verts.size();
    sgeom.assign(nv, {});
    std::vector<int> natOff(nv, 0);

    // The net shared by two consecutive elements of a series string.
    auto sharedNet = [&](std::uint32_t a, std::uint32_t b) -> std::int32_t {
        for (const ComponentPin& pa : d.components[a].pins) {
            if (pa.net < 0) continue;
            for (const ComponentPin& pb : d.components[b].pins) {
                if (pa.net == pb.net) return pa.net;
            }
        }
        return -1;
    };

    for (std::size_t vi = 0; vi < nv; ++vi) {
        const FlowVertex& v = flow.verts[vi];
        switch (v.kind) {
            case FlowVertex::Kind::Anchor:
            case FlowVertex::Kind::Loose: {
                int extL = 0, extR = 0, extT = 0, extB = 0;
                bodyExtents(v.comps[0], extL, extR, extT, extB);
                natOff[vi] = extL;
                break;
            }
            case FlowVertex::Kind::Vertical:
                natOff[vi] = roundUpP(verticalCellW(vTopNet[vi], vBotNet[vi]) / 2);
                break;
            case FlowVertex::Kind::Child: {
                const BlockInstance& b = d.blocks[v.comps[0]];
                int extL = kStubLen + 4;
                for (const BlockPort& p : b.ports) {
                    if (p.net >= 0) extL = std::max(extL, markExtent(pg, p.net, Side::Left));
                }
                natOff[vi] = roundUpP(extL);
                break;
            }
            case FlowVertex::Kind::Series: {
                SeriesGeom sg;
                const std::vector<std::uint32_t>& comps = v.comps;
                std::uint32_t entry = 0;
                if (comps.size() > 1) {
                    std::int32_t link = sharedNet(comps[0], comps[1]);
                    entry = d.components[comps[0]].pins[0].net == link ? 1u : 0u;
                }
                for (std::size_t i = 0; i < comps.size(); ++i) {
                    if (i > 0) {
                        std::int32_t link = sharedNet(comps[i - 1], comps[i]);
                        entry = d.components[comps[i]].pins[0].net == link ? 0u : 1u;
                    }
                    SeriesElem e;
                    e.comp = comps[i];
                    e.entryPin = entry;
                    const SymbolGeom& g = cache[comps[i]];
                    int gp = geomPinFor(g, entry);
                    if (gp < 0) gp = 0;
                    bool entryLeft = g.pins[static_cast<std::size_t>(gp)].side == Side::Left;
                    e.rot = entryLeft ? Rot::R0 : Rot::R180;
                    int lx = 0, ly = 0, rx = 0, ry = 0;
                    localPin(g, g.pins[static_cast<std::size_t>(gp)], lx, ly);
                    rotatePoint(g, e.rot, lx, ly, rx, ry);
                    sg.up = std::max(sg.up, ry + 16);
                    sg.dn = std::max(sg.dn, rotatedH(g, e.rot) - ry + 18);
                    sg.elems.push_back(e);
                }
                sg.startNet = d.components[comps[0]].pins[sg.elems[0].entryPin].net;
                std::uint32_t lastExit = sg.elems.back().entryPin == 0 ? 1u : 0u;
                sg.endNet = d.components[comps.back()].pins[lastExit].net;
                // A ground/rail terminal jogs a row down/up; make room for it.
                if (sg.endNet >= 0 && !isRoutable(sg.endNet)) {
                    switch (markKindFor(pg, sg.endNet)) {
                        case MarkKind::Ground: sg.dn = std::max(sg.dn, P + 16); break;
                        case MarkKind::RailFlag: sg.up = std::max(sg.up, P + 16); break;
                        default: break;
                    }
                }
                // Full mark extent even when the start will be a bare routed
                // stub: the space is what guarantees a route-failure label
                // fits without reaching into the previous column.
                natOff[vi] = roundUpP(markExtent(pg, sg.startNet, Side::Left));
                sgeom[vi] = std::move(sg);
                break;
            }
        }
    }

    // Alignment: vertices with identical (SymbolKind of comps[0], partName,
    // Kind, rank) share one offset -- the maximum of the group -- so
    // identical resistors and testpoints in one column stop staircasing.
    // Children key on the definition name instead of a SymbolKind. Cells
    // stack without overlap by construction, so the snap cannot collide.
    auto sameKey = [&](std::size_t a, std::size_t b) {
        const FlowVertex& va = flow.verts[a];
        const FlowVertex& vb = flow.verts[b];
        if (va.kind != vb.kind || va.rank != vb.rank) return false;
        if (va.kind == FlowVertex::Kind::Child) {
            return d.blocks[va.comps[0]].block == d.blocks[vb.comps[0]].block;
        }
        return m.kinds[va.comps[0]] == m.kinds[vb.comps[0]] &&
               d.components[va.comps[0]].partName == d.components[vb.comps[0]].partName;
    };
    effOff.assign(nv, 0);
    for (std::size_t a = 0; a < nv; ++a) {
        int best = natOff[a];
        for (std::size_t b = 0; b < nv; ++b) {
            if (a != b && sameKey(a, b)) best = std::max(best, natOff[b]);
        }
        effOff[a] = best;
    }
}

// ---------------------------------------------------------------------------
// Pin furniture. A pin of a net the router will attempt gets only the bare
// stub -- snapped outward to the P grid, its free end recorded -- so that
// whatever routing decides, the pin-conductor invariant already holds.
// Everything else keeps the classic stub, mark and solid strip.
// ---------------------------------------------------------------------------

void RoomPlacer::pinStub(const PlacedSymbol& s, std::size_t gp) {
    const SymPin& p = s.geom.pins[gp];
    int px = 0, py = 0;
    Side side = Side::Left;
    pinPos(s, gp, px, py, side);

    const bool tapToBar =
        !p.nc && p.net >= 0 && barOf[static_cast<std::size_t>(p.net)] >= 0;
    if (!p.nc && (isRoutable(p.net) || tapToBar)) {
        int sx = px, sy = py;
        switch (side) {
            case Side::Left: sx = roundDownP(px - kStubLen); break;
            case Side::Right: sx = roundUpP(px + kStubLen); break;
            case Side::Top: sy = roundDownP(py - kStubLen); break;
            case Side::Bottom: sy = roundUpP(py + kStubLen); break;
        }
        buf.wire({px, py, sx, sy}, p.net);
        buf.reserveWire(Rect{std::min(px, sx) - 2, std::min(py, sy) - 2, std::max(px, sx) + 2,
                             std::max(py, sy) + 2});
        if (tapToBar) {
            // A left/right/bottom pin of a bar rail: the bare stub's free end
            // is the tap point (top-side pins never reach here -- placeBody
            // intercepts them with a riser to the cell top).
            taps.push_back(Tap{sx, sy, p.net, side});
        } else {
            netPts[static_cast<std::size_t>(p.net)].push_back(StubPt{sx, sy, side});
        }
        return;
    }

    int e = markExtent(pg, p.net, side);
    int sx = px, sy = py;
    switch (side) {
        case Side::Left:
            sx -= kStubLen;
            buf.reserve(Rect{px - e, py - 6, px, py + 6});
            break;
        case Side::Right:
            sx += kStubLen;
            buf.reserve(Rect{px, py - 6, px + e, py + 6});
            break;
        case Side::Top:
            sy -= kStubLen;
            buf.reserve(Rect{px - 8, py - e, px + 8, py});
            break;
        case Side::Bottom:
            sy += kStubLen;
            buf.reserve(Rect{px - 8, py, px + 8, py + e});
            break;
    }
    buf.wire({px, py, sx, sy}, p.net);
    if (p.nc || p.net >= 0) buf.mark(markKindForPin(pg, p), sx, sy, side, p.net);
}

// ---------------------------------------------------------------------------
// Cells. Every cell is placed at a P-aligned (colX + off, y), reserves what
// it draws, and reports its outer size; the column stacks them without any
// collision search.
// ---------------------------------------------------------------------------

// Anchor and Loose vertices: the body with per-pin furniture. A top-side pin
// on a bar rail defers to the tap pass instead of drawing anything now.
void RoomPlacer::placeBody(std::uint32_t vi, int colX, int off, int y, int& cellW, int& cellH) {
    const std::uint32_t comp = flow.verts[vi].comps[0];
    const SymbolGeom& g = cache[comp];
    int extL = 0, extR = 0, extT = 0, extB = 0;
    bodyExtents(comp, extL, extR, extT, extB);

    PlacedSymbol placed;
    placed.component = comp;
    placed.geom = g;
    placed.x = colX + off;
    placed.y = y + extT;
    // One solid over the body, the top/bottom mark strips and the refdes and
    // part-name text rows; left/right strips are reserved per pin below.
    buf.reserve(Rect{placed.x - 2, y, placed.x + g.w + 2, placed.y + g.h + extB});

    for (std::size_t gp = 0; gp < g.pins.size(); ++gp) {
        const SymPin& p = g.pins[gp];
        int px = 0, py = 0;
        Side side = Side::Left;
        pinPos(placed, gp, px, py, side);
        if (side == Side::Top && !p.nc && p.net >= 0 &&
            barOf[static_cast<std::size_t>(p.net)] >= 0) {
            // Riser to the cell's top edge, through the pin's own mark strip;
            // the tap point is outside every solid, so the routed fallback
            // can leave it when the straight corridor is blocked.
            buf.wire({px, py, px, y}, p.net);
            buf.reserveWire(Rect{px - 2, y, px + 2, py});
            taps.push_back(Tap{px, y, p.net, Side::Top});
            continue;
        }
        pinStub(placed, gp);
    }
    buf.symbols.push_back(std::move(placed));

    cellW = off + g.w + extR;
    cellH = extT + g.h + extB;
    // The extents include the mark space of bare-stub pins too, so grow the
    // whole cell: a route-failure label must stay inside the room.
    buf.grow(Rect{colX, y, colX + cellW, y + cellH});
}

// A standing two-terminal cell: mark (or bare stub, or a bar tap's riser)
// above, body, mark (or bare stub) below. The attach rows sit on the P grid
// so a bare end is routable.
void RoomPlacer::placeVerticalCell(std::uint32_t vi, int colX, int off, int y, int& cellW,
                                   int& cellH) {
    const std::uint32_t comp = flow.verts[vi].comps[0];
    const std::int32_t topNet = vTopNet[vi], botNet = vBotNet[vi];
    const int cw = verticalCellW(topNet, botNet);
    const int cx = colX + off;

    PlacedSymbol s;
    s.component = comp;
    s.geom = cache[comp];
    s.rot = verticalRot(s.geom, vTopPin[vi]);
    int gp = geomPinFor(s.geom, vTopPin[vi]);
    int lx = 0, ly = 0, rx = 0, ry = 0;
    localPin(s.geom, s.geom.pins[static_cast<std::size_t>(gp < 0 ? 0 : gp)], lx, ly);
    rotatePoint(s.geom, s.rot, lx, ly, rx, ry);

    const int attachY = y + 2 * P;
    const int bodyTop = y + 3 * P;
    s.x = cx - rx;
    s.y = bodyTop;
    const int bodyBot = bodyTop + rotatedH(s.geom, s.rot);

    const bool bareTop = isRoutable(topNet);
    const bool tapTop = topNet >= 0 && barOf[static_cast<std::size_t>(topNet)] >= 0;
    const bool bareBot = isRoutable(botNet);
    if (tapTop) {
        // A pull-up on a bar rail: the attach wire runs to the cell's top
        // edge and becomes the tap's riser instead of taking a rail flag.
        buf.wire({cx, y, cx, bodyTop}, topNet);
        buf.reserveWire(Rect{cx - 2, y, cx + 2, bodyTop});
        taps.push_back(Tap{cx, y, topNet, Side::Top});
    } else {
        buf.wire({cx, attachY, cx, bodyTop}, topNet);
        if (bareTop) {
            buf.reserveWire(Rect{cx - 2, attachY - 2, cx + 2, bodyTop});
            netPts[static_cast<std::size_t>(topNet)].push_back(StubPt{cx, attachY, Side::Top});
        } else if (topNet >= 0) {
            buf.mark(markKindFor(pg, topNet), cx, attachY, Side::Top, topNet);
        }
    }
    buf.wire({cx, bodyBot, cx, bodyBot + P}, botNet);
    if (bareBot) {
        buf.reserveWire(Rect{cx - 2, bodyBot, cx + 2, bodyBot + P + 2});
        netPts[static_cast<std::size_t>(botNet)].push_back(
            StubPt{cx, bodyBot + P, Side::Bottom});
    } else if (botNet >= 0) {
        buf.mark(markKindFor(pg, botNet), cx, bodyBot + P, Side::Bottom, botNet);
    }
    buf.symbols.push_back(std::move(s));

    cellH = roundUpP(3 * P + (bodyBot - bodyTop) + P + 16);
    cellW = off + cw / 2 + 24;  // +24: the refdes/value text right of the body
    // The solid stops at a bare end's stub row, so the router can leave it;
    // a tapped top keeps its whole riser clear the same way.
    const int top = bareTop ? attachY : tapTop ? bodyTop : y;
    const int bot = bareBot ? bodyBot + P : y + cellH;
    buf.reserve(Rect{cx - cw / 2, top, cx + cw / 2 + 24, bot});
    buf.grow(Rect{colX, y, colX + cellW, y + cellH});
}

// A series string laid horizontally pin to pin: placeRun's element mechanics
// without the anchor lead-in. The two end nets are marked, or left as bare
// grid stubs for the router.
void RoomPlacer::placeSeries(std::uint32_t vi, int colX, int off, int y, int& cellW,
                             int& cellH) {
    const SeriesGeom& sg = sgeom[vi];
    const int up = roundUpP(sg.up);
    const int stripY = y + up;
    const int sx0 = colX + off;

    if (isRoutable(sg.startNet)) {
        netPts[static_cast<std::size_t>(sg.startNet)].push_back(
            StubPt{sx0, stripY, Side::Left});
    } else if (sg.startNet >= 0 && barOf[static_cast<std::size_t>(sg.startNet)] >= 0) {
        // The string starts on a bar rail: riser to the cell's top edge.
        buf.wire({sx0, stripY, sx0, y}, sg.startNet);
        taps.push_back(Tap{sx0, y, sg.startNet, Side::Top});
    } else if (sg.startNet >= 0) {
        buf.mark(markKindFor(pg, sg.startNet), sx0, stripY, Side::Left, sg.startNet);
    }

    int cursor = sx0;
    std::int32_t net = sg.startNet;
    for (const SeriesElem& e : sg.elems) {
        PlacedSymbol s;
        s.component = e.comp;
        s.geom = cache[e.comp];
        s.rot = e.rot;
        // The entry terminal is the one whose rotated side faces the source.
        int entryIdx = -1;
        for (std::size_t i = 0; i < s.geom.pins.size(); ++i) {
            if (rotatedSide(s.geom.pins[i].side, s.rot) == Side::Left) {
                entryIdx = static_cast<int>(i);
            }
        }
        if (entryIdx < 0) entryIdx = 0;
        int lx = 0, ly = 0, ex = 0, ey = 0;
        localPin(s.geom, s.geom.pins[static_cast<std::size_t>(entryIdx)], lx, ly);
        rotatePoint(s.geom, s.rot, lx, ly, ex, ey);
        const int entryX = cursor + P;
        s.x = entryX - ex;
        s.y = stripY - ey;
        buf.wire({cursor, stripY, entryX, stripY}, net);
        const int exitIdx = entryIdx == 0 ? 1 : 0;
        int ox = 0, oy = 0;
        localPin(s.geom, s.geom.pins[static_cast<std::size_t>(exitIdx)], lx, ly);
        rotatePoint(s.geom, s.rot, lx, ly, ox, oy);
        cursor = s.x + ox;
        net =
            d.components[e.comp].pins[s.geom.pins[static_cast<std::size_t>(exitIdx)].pin].net;
        buf.symbols.push_back(std::move(s));
    }

    // The end terminal (classic placeRun's switch, rightward only).
    const std::int32_t end = sg.endNet;
    int bandX1;
    if (end >= 0 && isRoutable(end)) {
        const int ex2 = roundUpP(cursor + P);
        buf.wire({cursor, stripY, ex2, stripY}, end);
        netPts[static_cast<std::size_t>(end)].push_back(StubPt{ex2, stripY, Side::Right});
        bandX1 = ex2;
    } else if (end < 0) {
        buf.wire({cursor, stripY, cursor + P, stripY}, -1);
        bandX1 = cursor + P;
    } else {
        switch (markKindFor(pg, end)) {
            case MarkKind::Ground:
                buf.wire({cursor, stripY, cursor + P, stripY, cursor + P, stripY + P}, end);
                buf.mark(MarkKind::Ground, cursor + P, stripY + P, Side::Bottom, end);
                break;
            case MarkKind::RailFlag:
                if (barOf[static_cast<std::size_t>(end)] >= 0) {
                    // The string ends on a bar rail: the jog keeps rising to
                    // the cell's top edge and taps the bar from there.
                    buf.wire({cursor, stripY, cursor + P, stripY, cursor + P, y}, end);
                    taps.push_back(Tap{cursor + P, y, end, Side::Top});
                } else {
                    buf.wire({cursor, stripY, cursor + P, stripY, cursor + P, stripY - P},
                             end);
                    buf.mark(MarkKind::RailFlag, cursor + P, stripY - P, Side::Top, end);
                }
                break;
            case MarkKind::Label:
            case MarkKind::NoConnect:
            case MarkKind::PortFlag:
                buf.wire({cursor, stripY, cursor + P, stripY}, end);
                buf.mark(markKindFor(pg, end), cursor + P, stripY, Side::Right, end);
                break;
        }
        bandX1 = cursor + markTail(pg, end);
    }

    // The band solid stops at a bare end so the router can leave the stub;
    // a marked end's text is covered by markTail above. The cell itself
    // always spans the full mark extents, so a route-failure label at either
    // bare end still lands in space nothing else claimed.
    const int bandX0 = isRoutable(sg.startNet) ? sx0 : colX;
    buf.reserve(Rect{bandX0, stripY - sg.up, bandX1, stripY + sg.dn});
    cellH = roundUpP(up + sg.dn);
    cellW = std::max(bandX1, end >= 0 ? cursor + markTail(pg, end) : bandX1) - colX + P;
    buf.grow(Rect{colX, y, colX + cellW, y + cellH});
}

// A child block's sheet symbol: the classic green box, ports down the left
// edge, stub and mark per port (port nets are never routed).
void RoomPlacer::placeChild(std::uint32_t vi, int colX, int off, int y, int& cellW,
                            int& cellH) {
    const std::uint32_t bi = flow.verts[vi].comps[0];
    const BlockInstance& b = d.blocks[bi];
    const int n = static_cast<int>(b.ports.size());
    int w = std::max(textW(b.block) + 16, 6 * P);
    for (const BlockPort& p : b.ports) w = std::max(w, 16 + textW(p.name) + 8);
    const int h = sheetSymBodyH(n);
    const int topPad = 14;  // the instance designator above the box

    SheetSymItem item;
    item.x = colX + off;
    item.y = y + topPad;
    item.w = w;
    item.h = h;
    item.block = bi;
    for (int i = 0; i < n; ++i) {
        const BlockPort& p = b.ports[static_cast<std::size_t>(i)];
        int py = item.y + sheetSymPortY(i);
        buf.wire({item.x, py, item.x - kStubLen, py}, p.net);
        if (p.net >= 0) {
            buf.mark(markKindFor(pg, p.net), item.x - kStubLen, py, Side::Left, p.net);
        }
    }
    buf.children.push_back(item);

    cellW = off + w + P;
    cellH = roundUpP(topPad + h + P);
    buf.reserve(Rect{colX, y, colX + cellW, y + cellH});
}

// ---------------------------------------------------------------------------
// Phase 2: the rank columns, left to right; within a column pull-ups first,
// then the rank's own order, then pull-downs, each rank's barycentre order
// preserved within its group.
// ---------------------------------------------------------------------------

void RoomPlacer::placeColumns(int yStart) {
    int cursorX = 0;
    bool prevHadAnchor = false;
    bool anyColumn = false;
    for (const std::vector<std::uint32_t>& layer : flow.ranks) {
        std::vector<std::uint32_t> ups, mid, downs;
        for (std::uint32_t vi : layer) {
            if (vertPlaced[vi]) continue;  // a ladder the bar phase consumed
            if (flow.verts[vi].kind == FlowVertex::Kind::Vertical &&
                vrole[vi] == VRole::PullUp) {
                ups.push_back(vi);
            } else if (flow.verts[vi].kind == FlowVertex::Kind::Vertical &&
                       vrole[vi] == VRole::PullDown) {
                downs.push_back(vi);
            } else {
                mid.push_back(vi);
            }
        }
        std::vector<std::uint32_t> ordered;
        ordered.insert(ordered.end(), ups.begin(), ups.end());
        ordered.insert(ordered.end(), mid.begin(), mid.end());
        ordered.insert(ordered.end(), downs.begin(), downs.end());
        if (ordered.empty()) continue;

        bool hasAnchor = false;
        for (std::uint32_t vi : ordered) {
            if (flow.verts[vi].kind == FlowVertex::Kind::Anchor) hasAnchor = true;
        }
        if (anyColumn) cursorX += (prevHadAnchor && hasAnchor) ? 4 * P : 2 * P;

        const int colX = cursorX;
        int y = yStart;
        int colW = 0;
        for (std::uint32_t vi : ordered) {
            int cw = 0, ch = 0;
            switch (flow.verts[vi].kind) {
                case FlowVertex::Kind::Anchor:
                case FlowVertex::Kind::Loose: placeBody(vi, colX, effOff[vi], y, cw, ch); break;
                case FlowVertex::Kind::Vertical:
                    placeVerticalCell(vi, colX, effOff[vi], y, cw, ch);
                    break;
                case FlowVertex::Kind::Series: placeSeries(vi, colX, effOff[vi], y, cw, ch); break;
                case FlowVertex::Kind::Child: placeChild(vi, colX, effOff[vi], y, cw, ch); break;
            }
            colW = std::max(colW, cw);
            y += ch + 2 * P;
        }
        cursorX = colX + roundUpP(colW);
        prevHadAnchor = hasAnchor;
        anyColumn = true;
    }
}

// ---------------------------------------------------------------------------
// Phase 3: stretch each bar to the room's content width and join its taps.
// Each tap tries the straight corridor drop first (the pretty case), then the
// routed fallback around whatever blocked it, and only when both refuse does
// it keep the classic per-pin rail flag -- so a rail with a bar shows flags
// only where no conductor can reach the bar at all.
// ---------------------------------------------------------------------------

void RoomPlacer::finishBars() {
    if (buf.bars.empty()) return;
    const int contentW = std::max(buf.maxX, 4 * P);
    for (RailBarItem& bar : buf.bars) {
        bar.x2 = std::max(bar.x2, contentW);
        buf.reserveWire(Rect{bar.x1, bar.y - 2, bar.x2, bar.y + 2});
    }

    // The straight drop must clear every solid AND every foreign vertical
    // wire astride its line -- an earlier tap's routed leg, a bare stub --
    // or two nets would read as one conductor. Same-net wires merge legally.
    auto corridorClear = [&](std::int32_t net, int px, int barY, int bottom) {
        if (bottom <= barY + 4) return false;
        if (buf.collides(Rect{px - 2, barY + 4, px + 2, bottom})) return false;
        for (const WireItem& w : buf.wires) {
            if (w.net == net) continue;
            for (std::size_t i = 0; i + 3 < w.pts.size(); i += 2) {
                const int ax = w.pts[i], ay = w.pts[i + 1];
                const int bx = w.pts[i + 2], by = w.pts[i + 3];
                if (ax != bx || ay == by) continue;  // vertical segments only
                if (ax <= px - 4 || ax >= px + 4) continue;
                if (std::max(barY + 4, std::min(ay, by)) <
                    std::min(bottom, std::max(ay, by))) {
                    return false;
                }
            }
        }
        return true;
    };
    auto dotOnce = [&](int x, int y, std::int32_t net) {
        for (const DotItem& e : buf.dots) {
            if (e.x == x && e.y == y) return;
        }
        buf.dots.push_back(DotItem{x, y, net});
    };

    // The routed fallback needs a grid start; a pin at an off-grid x (some
    // connector geometries) first jogs along its own cell-top edge to the
    // nearest grid point, nearer candidate tried first, lower x on the tie.
    auto tryRoute = [&](const Tap& t, const RailBarItem& bar) {
        if (t.px % P == 0) return routeRailTap(buf, t.px, t.py, bar);
        const int lo = roundDownP(t.px), hi = roundUpP(t.px);
        const int first = (t.px - lo <= hi - t.px) ? lo : hi;
        const int second = first == lo ? hi : lo;
        for (int gx : {first, second}) {
            if (gx < 0) continue;
            if (routeRailTap(buf, gx, t.py, bar)) {
                buf.wire({t.px, t.py, gx, t.py}, t.net);
                buf.reserveWire(Rect{std::min(t.px, gx) - 2, t.py - 2,
                                     std::max(t.px, gx) + 2, t.py + 2});
                return true;
            }
        }
        return false;
    };

    for (const Tap& t : taps) {
        const std::int32_t bi = barOf[static_cast<std::size_t>(t.net)];
        const RailBarItem& bar = buf.bars[static_cast<std::size_t>(bi)];
        if (corridorClear(t.net, t.px, bar.y, t.py)) {
            buf.wire({t.px, bar.y, t.px, t.py}, t.net);
            buf.reserveWire(Rect{t.px - 2, bar.y, t.px + 2, t.py});
            // The bar runs through, the tap ends: three conductors, one dot.
            dotOnce(t.px, bar.y, t.net);
        } else if (tryRoute(t, bar)) {
            // Routed around the blockage; the router dotted the bar joint.
        } else {
            buf.mark(MarkKind::RailFlag, t.px, t.py, t.fbSide, t.net);
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 4: routing, in net index order. Success needs no marks -- the wire
// says it, and junction dots are the router's job. Failure adds the label
// each pin would have had, so the pin-conductor invariant holds either way.
// ---------------------------------------------------------------------------

void RoomPlacer::routeAll() {
    for (std::size_t ni = 0; ni < d.nets.size(); ++ni) {
        if (!routable[ni]) continue;
        const std::vector<StubPt>& v = netPts[ni];
        if (v.empty()) continue;  // fully drawn inside a series string
        bool ok = v.size() == d.nets[ni].pins.size();
        if (ok) {
            RouteRequest req;
            req.net = static_cast<std::int32_t>(ni);
            for (const StubPt& sp : v) req.pins.push_back({sp.x, sp.y});
            ok = routeNet(buf, req, static_cast<std::int32_t>(ni));
        }
        if (!ok) {
            for (const StubPt& sp : v) {
                buf.mark(markKindFor(pg, static_cast<std::int32_t>(ni)), sp.x, sp.y, sp.side,
                         static_cast<std::int32_t>(ni));
            }
        }
    }
}

void RoomPlacer::run() {
    classifyVerticals();
    computeRoutable();
    const int yStart = placeBars();
    computeMetrics();
    placeColumns(yStart);
    finishBars();
    routeAll();
}

}  // namespace

SheetLayout layoutPageFlow(const RenderModel& model, const RenderPage& page) {
    const Design& d = *model.design;
    SheetLayout sheet;
    sheet.note = page.note;

    // 1. Flows and plans first, then the page's own symbol cache: sides face
    // their flow partners, and planned pins carry the page-local net names.
    std::vector<RoomFlow> flows;
    flows.reserve(page.rooms.size());
    for (const RenderRoom& room : page.rooms) flows.push_back(buildRoomFlow(page, room, model));

    std::vector<SidePlan> plans(d.components.size());
    for (std::size_t ri = 0; ri < page.rooms.size(); ++ri) {
        for (std::uint32_t idx : page.rooms[ri].components) {
            SymbolKind k = model.kinds[idx];
            if (k == SymbolKind::Generic || k == SymbolKind::Connector) {
                plans[idx] = planSides(idx, flows[ri], page, model);
            }
        }
    }
    const SymbolCache cache = buildSymbolCache(model, &plans, &page);

    // 2-3. Each room into its own buffer, room-local coordinates.
    std::vector<RoomBuf> bufs;
    std::vector<RoomExtent> extents;
    std::vector<int> stripH;
    bufs.reserve(page.rooms.size());
    for (std::size_t ri = 0; ri < page.rooms.size(); ++ri) {
        const RenderRoom& room = page.rooms[ri];
        RoomPlacer rp(model, page, cache, room, flows[ri]);
        rp.run();
        const int strip = room.framed && !room.title.empty() ? kTitleStrip : 0;
        extents.push_back(
            RoomExtent{rp.buf.maxX + 2 * kRoomPad, rp.buf.maxY + 2 * kRoomPad + strip});
        stripH.push_back(strip);
        bufs.push_back(std::move(rp.buf));
    }

    // 4. Tile the rooms. The placed rectangles partition their bounding box;
    // the frame is drawn at the stretched size while the content stays
    // anchored top-left. The width budget is a generous ceiling --
    // ceil(sqrt(3 * area)) admits shapes out to roughly 2:1 once column
    // stretch is paid for -- and the tiler's aspect-driven column count does
    // the real shaping, so sheets land near the landscape sqrt(2):1 of the
    // reference schematics instead of the portrait strips the old classic
    // 1600-clamped budget produced under column tiling.
    std::int64_t area = 0;
    int widest = 0;
    for (const RoomExtent& e : extents) {
        area += static_cast<std::int64_t>(e.w) * e.h;
        widest = std::max(widest, e.w);
    }
    int targetW = static_cast<int>(isqrtCeil(area * 3));
    targetW = std::max(targetW, widest);

    const std::vector<RoomPlace> places =
        tileRooms(extents, buildInterRoomFlow(page, model), targetW);

    int bboxW = 0, bboxH = 0;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const RoomPlace& pl = places[i];
        const int rx = kSheetMargin + pl.x;
        const int ry = kSheetMargin + pl.y;
        const RenderRoom& room = page.rooms[i];
        RoomItem ri;
        ri.x = rx;
        ri.y = ry;
        ri.w = pl.w;
        ri.h = pl.h;
        ri.title = upperCopy(room.title);
        ri.framed = room.framed;
        sheet.rooms.push_back(std::move(ri));

        translate(bufs[i], rx + kRoomPad, ry + stripH[i] + kRoomPad);
        RoomBuf& b = bufs[i];
        sheet.symbols.insert(sheet.symbols.end(), b.symbols.begin(), b.symbols.end());
        sheet.children.insert(sheet.children.end(), b.children.begin(), b.children.end());
        sheet.wires.insert(sheet.wires.end(), b.wires.begin(), b.wires.end());
        sheet.dots.insert(sheet.dots.end(), b.dots.begin(), b.dots.end());
        sheet.marks.insert(sheet.marks.end(), b.marks.begin(), b.marks.end());
        sheet.bars.insert(sheet.bars.end(), b.bars.begin(), b.bars.end());

        bboxW = std::max(bboxW, pl.x + pl.w);
        bboxH = std::max(bboxH, pl.y + pl.h);
    }

    sheet.w = std::max(bboxW + 2 * kSheetMargin, 680);
    sheet.h = bboxH + 2 * kSheetMargin + kTitleBlockH;
    return sheet;
}

}  // namespace manta::render
