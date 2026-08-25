// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WP6: cluster composition. Each room is drawn from its RoomPlan (cluster.cpp)
// the way a datasheet reference schematic is drawn: one cluster per anchor,
// each cluster a self-contained cell -- decap rows on top, pull-up satellites,
// the anchor body with its ARTERIES (one horizontal wire per seeded pin:
// series parts inline on the wire, shunt strings hanging from junction dots,
// grounds below, the rail flag or the router handoff at the end), pull-down
// satellites and loose cells below -- and the cells shelf-packed into the
// room at natural content size.
//
// Structural choices, documented per the work package:
//   - Rails are LOCAL. A cluster's rail segment (earned by the pin-count
//     rule over the cluster's consumers -- see cluster.cpp claimDecaps)
//     draws one short RailBarItem spanning just its ladder and the
//     cluster's own tap columns (finishBars widens it to the taps, never to
//     the room); a group may have no ladder at all when the anchor's own
//     supply pins earn it. Rail pins outside an owning cluster keep the
//     classic per-part flag. Bars still sit at y = 12 (mod P), off the
//     routing grid.
//   - Artery strips are allocated by measure, not search: side slots top to
//     bottom, stripY = max(anchor pin row, previous strip bottom + up-half
//     + P), and the cluster cell reserves the whole envelope, so cells can
//     never collide by construction.
//   - Junction taps use the classic node mechanics verbatim: pitch from the
//     far-end name widths, firstTap = max(2P, pitch/2 + P), only the last
//     gap of a string carries its end mark, and junction dots are COUNTED
//     over the drawn segments, never assumed.
//   - An artery ending on a rail taps the cluster's own rail segment: the
//     side's top strip is guaranteed at measure time, a lower strip takes
//     the tap opportunistically at draw time when its riser column is
//     actually clear (everything above it is already reserved), and the
//     classic flag stands wherever the riser cannot be drawn.
//   - Alignment is keyed on cluster STRUCTURE (Cluster::shapeKey), not on
//     rank: clusters with equal non-empty keys take the element-wise max of
//     every internal measure (metric union, unifyMetrics) so their cells
//     come out byte-identical inside, and they pack adjacently at the first
//     member's slot -- repeated identical channels draw identically, side
//     by side. Unequal cells still take natural metrics.
//   - Bare shunt ends are drawn as marks, not router handoffs: a tap column
//     sits at an element-width x that is rarely on the P grid, and a label
//     connects correctly where a refused route would only fall back to one.
//
// Determinism (spec 15.8): integer math, vectors in index/insertion order,
// explicit tie-breaks; nothing here reads an unordered container or a float.
#include "render/place.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "render/cluster.h"
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

// ---------------------------------------------------------------------------
// One room, drawn from its cluster plan.
// ---------------------------------------------------------------------------

struct RoomPlacer {
    const RenderModel& m;
    const RenderPage& pg;
    const Design& d;
    const SymbolCache& cache;
    const RenderRoom& room;
    const RoomFlow& flow;
    const std::vector<SidePlan>& plans;
    RoomBuf buf;
    RoomPlan plan;

    RoomPlacer(const RenderModel& model, const RenderPage& page, const SymbolCache& geoms,
               const RenderRoom& rm, const RoomFlow& fl, const std::vector<SidePlan>& sp)
        : m(model), pg(page), d(*model.design), cache(geoms), room(rm), flow(fl), plans(sp) {}

    void run();

private:
    struct StubPt {
        int x = 0, y = 0;
        Side side = Side::Right;
    };
    // One rail-bar consumer. The placer pre-draws the riser from the pin to
    // the tap point -- a grid point on the cluster zone's top edge (or a side
    // stub's free end) that no solid covers -- so finishBars can join it to
    // the cluster's own bar three ways: the straight corridor drop, the
    // routed fallback, or, when both fail, the classic per-pin rail flag
    // drawn at the tap point.
    struct Tap {
        int px = 0, py = 0;  // the tap point, room coords
        std::int32_t net = -1;
        Side fbSide = Side::Top;  // the failure mark's direction
        std::int32_t bar = -1;    // the OWN cluster's bar (several clusters may
                                  // bar the same rail, so net alone is not it)
    };

    // ------------------------------------------------------------------
    // Geometry plans, all measured before anything is drawn.
    // ------------------------------------------------------------------

    // One horizontal element of an artery or free run, orientation resolved
    // so the entry pin faces the source (classic placeRun's R0/R180 rule).
    struct ElemGeom {
        std::uint32_t comp = 0;
        int gp = 0;  // geometry pin index of the entry pin
        Rot rot = Rot::R0;
        int w = 0;    // rotated width
        int ery = 0;  // rotated entry-pin y offset
    };
    // One vertical element of a shunt string (classic placeNode's ElemGeom).
    struct VElem {
        std::uint32_t comp = 0;
        int gp = 0;  // geometry pin index of the entry pin
        Rot rot = Rot::R0;
        int h = 0;  // rotated height
    };
    struct ShuntGeom {
        std::vector<VElem> elems;
        std::int32_t endNet = -1;
        bool up = false;
        int reach = 0;  // trunk row to the far side of the end mark
    };
    struct JuncGeom {
        std::int32_t net = -1;
        std::vector<ShuntGeom> shunts;
        int pitch = 3 * P, firstTap = 2 * P, lastTap = 0;
    };
    // How a horizontal strip terminates.
    enum class StripEnd : std::uint8_t {
        Bare,      // endNet < 0: a short dead wire
        Handoff,   // Free + routable: bare grid stub for the router
        Ground,    // step, drop, ground mark
        RailFlag,  // step, rise, flag
        RailTap,   // step, rise to the zone top, Tap onto the cluster's bar
        Mark,      // step + label/port/nc mark
        JuncNamed, // ends at the junction, trunk carries the net's mark
        JuncBare,  // ends at the junction, fully drawn: no mark, no dot
    };
    struct ArtGeom {
        const Artery* art = nullptr;
        int gp = 0;  // anchor geometry pin index
        bool rightward = true;
        std::vector<ElemGeom> inls;  // one per Inline step, walk order
        bool hasJunc = false;
        JuncGeom junc;
        StripEnd end = StripEnd::Bare;
        int up = 16, dn = 14;  // strip half-heights
        int inner = 0;         // midX -> reserve end
        int pinYRel = 0;       // pin row, relative to the anchor body top
        int midOff = 0;        // pin -> jog column distance (ext + 6 + stagger)
        int stripRel = 0;      // strip row, relative to the anchor body top
    };
    struct RunGeom {
        std::vector<ElemGeom> elems;
        std::int32_t startNet = -1, endNet = -1;
        StripEnd end = StripEnd::Bare;
        int up = 16, dn = 14;
        int startExt = 0;  // room for the start mark, P-rounded
        int inner = 0;     // startX -> reserve end
    };

    // The per-cluster draw plan: every position is relative to the cell's
    // top-left corner, decided by measure alone.
    struct DecapDraw {
        std::size_t idx = 0;  // into Cluster::decaps
        int y = 0, ladderX = 0;
    };
    struct SatDraw {
        std::uint32_t vi = 0;
        int x = 0, y = 0;
    };
    struct LooseDraw {
        std::uint32_t vi = 0;
        int x = 0, y = 0;
        bool vert = false;
    };
    struct RunDraw {
        RunGeom g;
        int x = 0, y = 0;
    };
    struct CMetric {
        int w = 0, h = 0;
        int bodyOff = 0;  // cell top -> anchor body top, for row alignment
        bool hasBody = false;
        std::uint32_t anchorComp = 0;
        int bodyX = 0, bodyY = 0, zoneTop = 0;  // rel cell origin
        std::vector<ArtGeom> arts;              // left bucket then right bucket
        std::vector<DecapDraw> decaps;
        std::vector<SatDraw> ups, downs;
        std::vector<LooseDraw> loose;
        std::vector<RunDraw> runs;
        bool isChild = false;
        std::uint32_t childVert = 0;
        int childY = 0;
        int x = 0, y = 0;  // pack position, room coords
    };

    // Everything measured about one cluster BEFORE composition: the raw
    // inputs the y/x sequencing is a pure function of. Split out so that
    // equal-shapeKey clusters can take the element-wise max of every input
    // first (metric union) and then compose to byte-identical cells.
    struct MIn {
        int extL = 0, extR = 0, extT = 0, extB = 0;
        std::vector<ArtGeom> arts;  // left bucket then right bucket
        std::size_t nLeft = 0;
        std::vector<int> capH;                        // per decap group
        std::vector<std::pair<int, int>> upWH, downWH;  // per satellite cell
        std::vector<std::pair<int, int>> looseWH;       // per loose vertex
        std::vector<std::pair<int, int>> runWH;         // per free run
        std::vector<RunGeom> runs;                      // parallel to freeRuns
    };

    std::vector<char> inRoom;                 // component -> member of this room
    std::vector<char> routable;               // net -> the router may claim it
    std::vector<std::vector<StubPt>> netPts;  // net -> bare stub ends collected
    std::vector<std::int32_t> barOf;          // net -> the bar of the cluster being
                                              // DRAWN (refreshed per cluster), -1
    std::vector<Tap> taps;
    std::vector<CMetric> mets;
    std::size_t curCluster = 0;

    [[nodiscard]] bool isRoutable(std::int32_t net) const {
        return net >= 0 && routable[static_cast<std::size_t>(net)] != 0;
    }
    // A rail pin taps a bar only inside a cluster that owns a segment on that
    // rail; outside it the pin keeps its per-part flag (rails are local).
    // Ownership is the plan's decap list, so it is answerable before any bar
    // is drawn -- and several clusters may each own a segment on one rail.
    [[nodiscard]] bool clusterHasBar(std::size_t ci, std::int32_t net) const {
        if (net < 0) return false;
        for (const DecapGroup& g : plan.clusters[ci].decaps) {
            if (g.rail == net) return true;
        }
        return false;
    }
    [[nodiscard]] bool tapsHere(std::int32_t net) const {
        return clusterHasBar(curCluster, net);
    }
    // Record a tap against the drawing cluster's own bar. Decap rows are the
    // first thing a cluster draws, so barOf is current by construction.
    void addTap(int px, int py, std::int32_t net, Side fbSide) {
        const std::int32_t bi = barOf[static_cast<std::size_t>(net)];
        assert(bi >= 0 && "a tap recorded before its cluster's bar was drawn");
        taps.push_back(Tap{px, py, net, fbSide, bi});
    }

    void computeRoutable();

    // Metrics.
    [[nodiscard]] ElemGeom horizElem(std::uint32_t comp, std::uint32_t entryPin,
                                     bool rightward) const;
    [[nodiscard]] ShuntGeom planShunt(const Shunt& sh) const;
    [[nodiscard]] StripEnd classifyEnd(std::int32_t net, std::size_t ci, bool railTapOk) const;
    [[nodiscard]] int endExtent(StripEnd end, std::int32_t net, const JuncGeom& jg) const;
    void absorbEnd(StripEnd end, int& up, int& dn) const;
    [[nodiscard]] ArtGeom planArtery(const Artery& a, const SymbolGeom& g, std::size_t ci,
                                     bool topSlot) const;
    [[nodiscard]] RunGeom planRun(const std::vector<ChainElem>& run, std::size_t ci) const;
    void vertOrient(std::uint32_t comp, std::uint32_t& topPin, std::int32_t& topNet,
                    std::int32_t& botNet) const;
    void measureVertical(std::uint32_t vi, int& w, int& h) const;
    void measureLooseBody(std::uint32_t vi, int& w, int& h) const;
    void measureChild(std::uint32_t vi, int& off, int& w, int& h) const;
    void measureCluster(std::size_t ci, MIn& in) const;
    void unifyMetrics(std::vector<MIn>& ins) const;
    void composeCluster(std::size_t ci, MIn& in);
    void clusterMetrics();
    void packClusters();

    // Drawing.
    void drawClusters();
    void drawDecapRow(const DecapGroup& gr, const DecapDraw& dd, int cx, int cy);
    void drawAnchor(CMetric& cm, int cx, int cy);
    void drawArtery(const PlacedSymbol& anchor, const ArtGeom& ag, int zoneTopY);
    int drawStripEnd(StripEnd end, std::int32_t net, int cursor, int stripY, int dir,
                     int riserTopY);
    void drawRun(const RunDraw& rd, int cx, int cy);
    void pinStub(const PlacedSymbol& s, std::size_t gp);
    void placeBody(std::uint32_t vi, int colX, int off, int y);
    void placeVerticalCell(std::uint32_t vi, int colX, int y);
    void placeChild(std::uint32_t vi, int colX, int off, int y);

    void finishBars();
    void routeAll();

    [[nodiscard]] bool riserClear(std::int32_t net, int x, int yTop, int yBot) const;
    [[nodiscard]] int verticalCellW(std::int32_t topNet, std::int32_t botNet) const;
    void bodyExtents(std::uint32_t comp, int& extL, int& extR, int& extT, int& extB) const;
};

// A vertical conductor may be drawn at x over (yTop, yBot) when it clears
// every solid and no foreign vertical wire runs astride its column -- two
// nets would read as one conductor. Perpendicular crossings are ordinary
// schematic drawing and stay legal; same-net overlap merges legally.
bool RoomPlacer::riserClear(std::int32_t net, int x, int yTop, int yBot) const {
    if (yBot <= yTop) return false;
    if (buf.collides(Rect{x - 2, yTop, x + 2, yBot})) return false;
    for (const WireItem& w : buf.wires) {
        if (w.net == net) continue;
        for (std::size_t i = 0; i + 3 < w.pts.size(); i += 2) {
            const int ax = w.pts[i], ay = w.pts[i + 1];
            const int bx = w.pts[i + 2], by = w.pts[i + 3];
            if (ax != bx || ay == by) continue;  // vertical segments only
            if (ax <= x - 4 || ax >= x + 4) continue;
            if (std::max(yTop, std::min(ay, by)) < std::min(yBot, std::max(ay, by))) {
                return false;
            }
        }
    }
    return true;
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
        // '&RENDER=LABEL' (revision 1.6): the author said a name, so the
        // router never claims it.
        if (rn.force == ForceMode::Label) continue;
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
// Shared cell measures.
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

// A standing two-terminal part's orientation: the rail pin up when it has
// one, the signal pin up over a ground, pin 0 up otherwise. The plan carries
// the ROLE (satUp, satDown, decap member); this is only the geometry of it.
void RoomPlacer::vertOrient(std::uint32_t comp, std::uint32_t& topPin, std::int32_t& topNet,
                            std::int32_t& botNet) const {
    const Component& c = d.components[comp];
    std::int32_t a = c.pins[0].net, b = c.pins[1].net;
    bool railA = isRail(pg, a), railB = isRail(pg, b);
    bool gndA = isGround(pg, a), gndB = isGround(pg, b);
    if (railA) {
        topPin = 0;
    } else if (railB) {
        topPin = 1;
    } else if (gndA) {
        topPin = 1;  // the signal pin faces up
    } else {
        topPin = 0;
    }
    (void)gndB;
    topNet = topPin == 0 ? a : b;
    botNet = topPin == 0 ? b : a;
}

void RoomPlacer::measureVertical(std::uint32_t vi, int& w, int& h) const {
    const std::uint32_t comp = flow.verts[vi].comps[0];
    std::uint32_t topPin = 0;
    std::int32_t topNet = -1, botNet = -1;
    vertOrient(comp, topPin, topNet, botNet);
    const int cw = verticalCellW(topNet, botNet);
    const int off = roundUpP(cw / 2);
    const SymbolGeom& g = cache[comp];
    w = off + cw / 2 + 24;  // +24: the refdes/value text right of the body
    h = roundUpP(3 * P + rotatedH(g, verticalRot(g, topPin)) + P + 16);
}

void RoomPlacer::measureLooseBody(std::uint32_t vi, int& w, int& h) const {
    const std::uint32_t comp = flow.verts[vi].comps[0];
    int extL = 0, extR = 0, extT = 0, extB = 0;
    bodyExtents(comp, extL, extR, extT, extB);
    const SymbolGeom& g = cache[comp];
    w = extL + g.w + extR;
    h = extT + g.h + extB;
}

void RoomPlacer::measureChild(std::uint32_t vi, int& off, int& w, int& h) const {
    const BlockInstance& b = d.blocks[flow.verts[vi].comps[0]];
    int extL = kStubLen + 4;
    for (const BlockPort& p : b.ports) {
        if (p.net >= 0) extL = std::max(extL, markExtent(pg, p.net, Side::Left));
    }
    off = roundUpP(extL);
    int bw = std::max(textW(b.block) + 16, 6 * P);
    for (const BlockPort& p : b.ports) bw = std::max(bw, 16 + textW(p.name) + 8);
    w = off + bw + P;
    h = roundUpP(14 + sheetSymBodyH(static_cast<int>(b.ports.size())) + P);
}

// ---------------------------------------------------------------------------
// Strip geometry: the classic element and node measures, plan-driven.
// ---------------------------------------------------------------------------

RoomPlacer::ElemGeom RoomPlacer::horizElem(std::uint32_t comp, std::uint32_t entryPin,
                                           bool rightward) const {
    ElemGeom e;
    e.comp = comp;
    const SymbolGeom& g = cache[comp];
    int gp = geomPinFor(g, entryPin);
    if (gp < 0) gp = 0;
    e.gp = gp;
    bool entryLeft = g.pins[static_cast<std::size_t>(gp)].side == Side::Left;
    e.rot = (entryLeft == rightward) ? Rot::R0 : Rot::R180;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    localPin(g, g.pins[static_cast<std::size_t>(gp)], lx, ly);
    rotatePoint(g, e.rot, lx, ly, rx, ry);
    e.w = rotatedW(g, e.rot);
    e.ery = ry;
    return e;
}

RoomPlacer::ShuntGeom RoomPlacer::planShunt(const Shunt& sh) const {
    ShuntGeom sg;
    sg.endNet = sh.endNet;
    sg.up = sh.up;
    sg.reach = P + 16;  // the last gap and the end mark's text
    for (const ChainElem& ce : sh.elems) {
        VElem ve;
        ve.comp = ce.comp;
        const SymbolGeom& g = cache[ce.comp];
        int gp = geomPinFor(g, ce.entryPin);
        if (gp < 0) gp = 0;
        ve.gp = gp;
        // verticalRot puts the named pin on top: an element of a string
        // hanging below wants its own entry pin up, one standing above
        // wants its exit pin up so the entry still faces the trunk.
        std::uint32_t exitPin = ce.entryPin == 0 ? 1u : 0u;
        ve.rot = verticalRot(g, sh.up ? exitPin : ce.entryPin);
        ve.h = rotatedH(g, ve.rot);
        sg.reach += P + ve.h;
        sg.elems.push_back(ve);
    }
    return sg;
}

RoomPlacer::StripEnd RoomPlacer::classifyEnd(std::int32_t net, std::size_t ci,
                                             bool railTapOk) const {
    if (net < 0) return StripEnd::Bare;
    if (isRoutable(net)) return StripEnd::Handoff;
    switch (markKindFor(pg, net)) {
        case MarkKind::Ground: return StripEnd::Ground;
        case MarkKind::RailFlag:
            return railTapOk && clusterHasBar(ci, net) ? StripEnd::RailTap
                                                       : StripEnd::RailFlag;
        default: return StripEnd::Mark;
    }
}

int RoomPlacer::endExtent(StripEnd end, std::int32_t net, const JuncGeom& jg) const {
    switch (end) {
        case StripEnd::Bare: return P;
        case StripEnd::Handoff: return 2 * P;  // the grid-rounded step
        case StripEnd::Ground: return P + 10;
        case StripEnd::RailFlag:
            return P + (net >= 0 ? textW(netName(pg, net)) / 2 : 0) + 8;
        case StripEnd::RailTap: return P + 4;
        case StripEnd::Mark:
        case StripEnd::JuncNamed: return markTail(pg, net);
        case StripEnd::JuncBare: return jg.shunts.empty() ? P : jg.pitch / 2 + 4;
    }
    return P;
}

void RoomPlacer::absorbEnd(StripEnd end, int& up, int& dn) const {
    if (end == StripEnd::Ground) dn = std::max(dn, P + 16);
    if (end == StripEnd::RailFlag) up = std::max(up, P + 16);
}

RoomPlacer::ArtGeom RoomPlacer::planArtery(const Artery& a, const SymbolGeom& g,
                                           std::size_t ci, bool topSlot) const {
    ArtGeom ag;
    ag.art = &a;
    ag.gp = geomPinFor(g, a.anchorPin);
    assert(ag.gp >= 0 && "an artery seeded on a pin the symbol does not expose");
    if (ag.gp < 0) ag.gp = 0;
    const SymPin& sp = g.pins[static_cast<std::size_t>(ag.gp)];
    ag.rightward = sp.side != Side::Left;
    int lx = 0, ly = 0;
    localPin(g, sp, lx, ly);
    ag.pinYRel = ly;

    int len = 0;
    for (const ArteryStep& s : a.steps) {
        if (s.kind == ArteryStep::Kind::Inline) {
            ElemGeom e = horizElem(s.elem.comp, s.elem.entryPin, ag.rightward);
            ag.up = std::max(ag.up, e.ery + 16);
            ag.dn = std::max(ag.dn, rotatedH(cache[e.comp], e.rot) - e.ery + 18);
            len += P + e.w;
            ag.inls.push_back(std::move(e));
            continue;
        }
        ag.hasJunc = true;
        ag.junc.net = s.net;
        // Tap pitch from the far-end name widths (classic nodeMetrics): only
        // the mark at a string's far end sits under its column, so the widest
        // name is what sets the pitch.
        int pitch = 3 * P;
        for (const Shunt& sh : s.shunts) {
            int w = 3 * P;
            if (sh.endNet >= 0 && markKindFor(pg, sh.endNet) != MarkKind::Ground &&
                markKindFor(pg, sh.endNet) != MarkKind::NoConnect) {
                w = textW(netName(pg, sh.endNet));
            }
            pitch = std::max(pitch, w + P);
        }
        pitch = roundUpP(pitch);
        ag.junc.pitch = pitch;
        // The first tap clears the entry by half a label, so the leftmost
        // far-end name cannot reach back over the wire arriving.
        ag.junc.firstTap = std::max(2 * P, pitch / 2 + P);
        const int n = static_cast<int>(s.shunts.size());
        ag.junc.lastTap = n > 0 ? ag.junc.firstTap + (n - 1) * pitch : 0;
        for (const Shunt& sh : s.shunts) {
            ShuntGeom sg = planShunt(sh);
            if (sg.up) ag.up = std::max(ag.up, sg.reach);
            else ag.dn = std::max(ag.dn, sg.reach);
            ag.junc.shunts.push_back(std::move(sg));
        }
        len += ag.junc.lastTap;
    }

    // Terminal classification. An artery whose last step is the junction ends
    // on the trunk itself: with a mark when the plan named it, bare when the
    // junction is fully drawn (the trunk stops on the last tap, a corner).
    if (!a.steps.empty() && a.steps.back().kind == ArteryStep::Kind::Junction) {
        ag.end = a.namedEnd ? StripEnd::JuncNamed : StripEnd::JuncBare;
        if (a.namedEnd) len += P;  // the step the mark sits at
    } else {
        ag.end = classifyEnd(a.endNet, ci, topSlot);
        absorbEnd(ag.end, ag.up, ag.dn);
    }
    len += endExtent(ag.end, a.endNet, ag.junc);
    ag.inner = len;
    return ag;
}

RoomPlacer::RunGeom RoomPlacer::planRun(const std::vector<ChainElem>& run,
                                        std::size_t ci) const {
    RunGeom rg;
    const Component& head = d.components[run[0].comp];
    rg.startNet = head.pins[run[0].entryPin].net;
    std::uint32_t cur = run[0].comp, entry = run[0].entryPin;
    int len = 0;
    for (const ChainElem& ce : run) {
        ElemGeom e = horizElem(ce.comp, ce.entryPin, true);
        rg.up = std::max(rg.up, e.ery + 16);
        rg.dn = std::max(rg.dn, rotatedH(cache[e.comp], e.rot) - e.ery + 18);
        len += P + e.w;
        rg.elems.push_back(std::move(e));
        cur = ce.comp;
        entry = ce.entryPin;
    }
    rg.endNet = d.components[cur].pins[entry == 0 ? 1u : 0u].net;
    rg.end = classifyEnd(rg.endNet, ci, true);
    absorbEnd(rg.end, rg.up, rg.dn);
    len += endExtent(rg.end, rg.endNet, JuncGeom{});
    rg.inner = len;
    // Full mark extent even when the start will be a bare routed stub: the
    // space is what guarantees a route-failure label fits inside the cell.
    rg.startExt = roundUpP(markExtent(pg, rg.startNet, Side::Left));
    // A rail/ground start needs headroom exactly like an end would.
    if (rg.startNet >= 0 && !isRoutable(rg.startNet)) {
        switch (markKindFor(pg, rg.startNet)) {
            case MarkKind::Ground: rg.dn = std::max(rg.dn, P + 16); break;
            case MarkKind::RailFlag: rg.up = std::max(rg.up, P + 16); break;
            default: break;
        }
    }
    return rg;
}

// ---------------------------------------------------------------------------
// Cluster metrics: every cell is measured whole -- decap rows, satellite
// rows, the anchor zone with its artery strips, loose cells, free runs --
// before anything is drawn, so packing needs no collision search.
//
// Three phases. MEASURE collects the raw inputs (extents, artery geometry,
// cap and satellite cell sizes). UNIFY takes, over each group of clusters
// with the same non-empty shapeKey, the element-wise max of every input
// that bends internal geometry -- so two identical channels whose only
// difference is a net name's width measure the same. COMPOSE then lays each
// cell out; composition is a pure function of structure plus inputs, so
// unified inputs make the group's cells byte-identical inside: every member
// part's offset from its anchor matches across the group.
// ---------------------------------------------------------------------------

void RoomPlacer::measureCluster(std::size_t ci, MIn& in) const {
    const Cluster& cl = plan.clusters[ci];
    const FlowVertex* av =
        cl.anchorVert >= 0 ? &flow.verts[static_cast<std::size_t>(cl.anchorVert)] : nullptr;

    if (av != nullptr && av->kind == FlowVertex::Kind::Anchor) {
        const std::uint32_t anchorComp = av->comps[0];
        const SymbolGeom& g = cache[anchorComp];
        bodyExtents(anchorComp, in.extL, in.extR, in.extT, in.extB);

        // Bucket the arteries by the GEOMETRY side of their pin -- the
        // side plan and the cluster walk agree, but the drawn side is
        // what the strip mechanics need -- keeping list order per side.
        std::vector<const Artery*> lefts, rights;
        for (const std::vector<Artery>* side : {&cl.left, &cl.right}) {
            for (const Artery& a : *side) {
                int gp = geomPinFor(g, a.anchorPin);
                assert(gp >= 0 && "an artery seeded on a pin the symbol hides");
                if (gp < 0) continue;
                if (g.pins[static_cast<std::size_t>(gp)].side == Side::Left) {
                    lefts.push_back(&a);
                } else {
                    rights.push_back(&a);
                }
            }
        }
        for (std::size_t k = 0; k < lefts.size(); ++k) {
            in.arts.push_back(planArtery(*lefts[k], g, ci, k == 0));
        }
        in.nLeft = in.arts.size();
        for (std::size_t k = 0; k < rights.size(); ++k) {
            in.arts.push_back(planArtery(*rights[k], g, ci, k == 0));
        }
    }

    for (const DecapGroup& gr : cl.decaps) {
        int capH = 0;
        for (std::uint32_t comp : gr.comps) {
            const SymbolGeom& cg = cache[comp];
            std::uint32_t railPin = d.components[comp].pins[0].net == gr.rail ? 0u : 1u;
            capH = std::max(capH, rotatedH(cg, verticalRot(cg, railPin)));
        }
        in.capH.push_back(capH);
    }

    auto satWH = [&](const std::vector<std::uint32_t>& vis, std::vector<std::pair<int, int>>& out) {
        for (std::uint32_t vi : vis) {
            int w = 0, h = 0;
            measureVertical(vi, w, h);
            out.push_back({w, h});
        }
    };
    satWH(cl.satUps, in.upWH);
    satWH(cl.satDowns, in.downWH);

    for (std::uint32_t vi : cl.looseVerts) {
        int w = 0, h = 0;
        if (flow.verts[vi].kind == FlowVertex::Kind::Vertical) measureVertical(vi, w, h);
        else measureLooseBody(vi, w, h);
        in.looseWH.push_back({w, h});
    }
    for (const std::vector<ChainElem>& run : cl.freeRuns) {
        RunGeom rg = planRun(run, ci);
        in.runWH.push_back({rg.startExt + rg.inner + P,
                            roundUpP(roundUpP(rg.up) + rg.dn)});
        in.runs.push_back(std::move(rg));
    }
}

// The metric union. Only measures are touched -- strip half-heights, inner
// reaches, junction tap pitches, extents, cell widths and heights -- never
// structure: equal shapeKeys already guarantee the vectors run parallel
// (asserted here, and any mismatch leaves the group untouched rather than
// mixing metrics across different shapes).
void RoomPlacer::unifyMetrics(std::vector<MIn>& ins) const {
    std::vector<char> done(ins.size(), 0);
    for (std::size_t i = 0; i < ins.size(); ++i) {
        if (done[i]) continue;
        done[i] = 1;
        const std::string& key = plan.clusters[i].shapeKey;
        if (key.empty()) continue;
        std::vector<std::size_t> group{i};
        for (std::size_t j = i + 1; j < ins.size(); ++j) {
            if (done[j] || plan.clusters[j].shapeKey != key) continue;
            done[j] = 1;
            group.push_back(j);
        }
        if (group.size() < 2) continue;

        bool parallel = true;
        const MIn& lead = ins[i];
        for (std::size_t j : group) {
            const MIn& in = ins[j];
            parallel = parallel && in.arts.size() == lead.arts.size() &&
                       in.nLeft == lead.nLeft && in.capH.size() == lead.capH.size() &&
                       in.upWH.size() == lead.upWH.size() &&
                       in.downWH.size() == lead.downWH.size() &&
                       in.looseWH.size() == lead.looseWH.size() &&
                       in.runWH.size() == lead.runWH.size();
        }
        assert(parallel && "equal shapeKeys with different structure");
        if (!parallel) continue;

        MIn u;  // the group's maxima, collected then written back whole
        u = ins[i];
        auto maxi = [](int& a, int b) { a = std::max(a, b); };
        for (std::size_t gi = 1; gi < group.size(); ++gi) {
            const MIn& in = ins[group[gi]];
            maxi(u.extL, in.extL);
            maxi(u.extR, in.extR);
            maxi(u.extT, in.extT);
            maxi(u.extB, in.extB);
            for (std::size_t a = 0; a < u.arts.size(); ++a) {
                maxi(u.arts[a].up, in.arts[a].up);
                maxi(u.arts[a].dn, in.arts[a].dn);
                maxi(u.arts[a].inner, in.arts[a].inner);
                maxi(u.arts[a].junc.pitch, in.arts[a].junc.pitch);
                maxi(u.arts[a].junc.firstTap, in.arts[a].junc.firstTap);
            }
            for (std::size_t k = 0; k < u.capH.size(); ++k) maxi(u.capH[k], in.capH[k]);
            auto maxWH = [&](std::vector<std::pair<int, int>>& a,
                             const std::vector<std::pair<int, int>>& b) {
                for (std::size_t k = 0; k < a.size(); ++k) {
                    maxi(a[k].first, b[k].first);
                    maxi(a[k].second, b[k].second);
                }
            };
            maxWH(u.upWH, in.upWH);
            maxWH(u.downWH, in.downWH);
            maxWH(u.looseWH, in.looseWH);
            maxWH(u.runWH, in.runWH);
        }
        for (std::size_t j : group) {
            MIn& in = ins[j];
            in.extL = u.extL;
            in.extR = u.extR;
            in.extT = u.extT;
            in.extB = u.extB;
            for (std::size_t a = 0; a < in.arts.size(); ++a) {
                JuncGeom& jg = in.arts[a].junc;
                in.arts[a].up = u.arts[a].up;
                in.arts[a].dn = u.arts[a].dn;
                in.arts[a].inner = u.arts[a].inner;
                jg.pitch = u.arts[a].junc.pitch;
                jg.firstTap = u.arts[a].junc.firstTap;
                // lastTap is DERIVED, so re-derive it: independent maxima of
                // pitch and firstTap could otherwise leave a trunk shorter
                // than its own last tap column.
                const int n = static_cast<int>(jg.shunts.size());
                jg.lastTap = n > 0 ? jg.firstTap + (n - 1) * jg.pitch : 0;
            }
            in.capH = u.capH;
            in.upWH = u.upWH;
            in.downWH = u.downWH;
            in.looseWH = u.looseWH;
            in.runWH = u.runWH;
        }
    }
}

void RoomPlacer::composeCluster(std::size_t ci, MIn& in) {
    const Cluster& cl = plan.clusters[ci];
    CMetric& cm = mets[ci];
    const FlowVertex* av =
        cl.anchorVert >= 0 ? &flow.verts[static_cast<std::size_t>(cl.anchorVert)] : nullptr;

    const SymbolGeom* g = nullptr;
    int zoneUp = 0, zoneDn = 0, leftW = 0, rightW = 0;
    if (av != nullptr && av->kind == FlowVertex::Kind::Anchor) {
        cm.hasBody = true;
        cm.anchorComp = av->comps[0];
        g = &cache[cm.anchorComp];

        // Per side: slot k of nSide chains jogs (nSide-1-k)*2P beyond the
        // side's mark extent (the classic fan-out), and strip rows go by
        // measure: stripY = max(pin row, previous bottom + up-half + P).
        auto planSide = [&](std::size_t b, std::size_t e, bool rightSide) {
            const int n = static_cast<int>(e - b);
            const int ext = rightSide ? in.extR : in.extL;
            int prevBot = 0;
            bool first = true;
            for (int k = 0; k < n; ++k) {
                ArtGeom ag = std::move(in.arts[b + static_cast<std::size_t>(k)]);
                ag.midOff = ext + 6 + (n - 1 - k) * 2 * P;
                int y = ag.pinYRel;
                if (!first) y = std::max(y, roundUpP(prevBot + ag.up + P));
                ag.stripRel = y;
                prevBot = y + ag.dn;
                first = false;
                zoneUp = std::max(zoneUp, ag.up - ag.stripRel);
                zoneDn = std::max(zoneDn, ag.stripRel + ag.dn - g->h);
                int reach = ag.midOff + ag.inner;
                if (rightSide) rightW = std::max(rightW, reach);
                else leftW = std::max(leftW, reach);
                cm.arts.push_back(std::move(ag));
            }
        };
        planSide(0, in.nLeft, false);
        planSide(in.nLeft, in.arts.size(), true);

        zoneUp = roundUpP(std::max(zoneUp, in.extT));
        zoneDn = roundUpP(std::max(zoneDn, in.extB));
        leftW = roundUpP(std::max(leftW, in.extL));
        rightW = roundUpP(std::max(rightW, in.extR));
        cm.bodyX = leftW;
    }

    int yCur = 0;
    int wMax = 0;

    // Decap rows first, at the top of the cell: the ladder columns sit
    // to the right of the anchor's top-pin block, so the pins' riser
    // corridors drop straight onto their own bar.
    for (std::size_t gi = 0; gi < cl.decaps.size(); ++gi) {
        const DecapGroup& gr = cl.decaps[gi];
        const int n = static_cast<int>(gr.comps.size());
        // A ladder sits to the right of the anchor's top-pin block; a
        // pin-only segment seeds its bar mid-body instead, so finishBars
        // widens it straight over the tap columns it will serve.
        const int ladderX =
            cm.hasBody ? (n > 0 ? roundUpP(cm.bodyX + g->w) + 2 * P
                                : roundUpP(cm.bodyX + g->w / 2))
                       : 2 * P;
        const int capH = in.capH[gi];
        const int bottom = yCur + 12 + (n > 0 ? P + capH + P + 10 : 10);
        cm.decaps.push_back(DecapDraw{gi, yCur, ladderX});
        wMax = std::max(wMax,
                        n > 0 ? ladderX + (n - 1) * 3 * P + 2 * P + 30 : ladderX + 2 * P);
        yCur = roundUpP(bottom + 4) + P;
    }

    // Satellite rows: pull-ups above the body, pull-downs below, each a
    // standing cell at the classic vertical metrics.
    auto satRow = [&](const std::vector<std::uint32_t>& vis,
                      const std::vector<std::pair<int, int>>& whs, std::vector<SatDraw>& out) {
        if (vis.empty()) return;
        int x = cm.hasBody ? cm.bodyX : 0;
        int rowH = 0;
        for (std::size_t k = 0; k < vis.size(); ++k) {
            out.push_back(SatDraw{vis[k], x, yCur});
            x += roundUpP(whs[k].first) + P;
            rowH = std::max(rowH, whs[k].second);
        }
        wMax = std::max(wMax, x);
        yCur += roundUpP(rowH) + P;
    };
    satRow(cl.satUps, in.upWH, cm.ups);

    if (cm.hasBody) {
        cm.zoneTop = yCur;
        cm.bodyY = yCur + zoneUp;
        cm.bodyOff = cm.bodyY;
        yCur = roundUpP(cm.bodyY + g->h + zoneDn);
        wMax = std::max(wMax, cm.bodyX + g->w + rightW);
    } else if (av != nullptr && av->kind == FlowVertex::Kind::Child) {
        cm.isChild = true;
        cm.childVert = static_cast<std::uint32_t>(cl.anchorVert);
        cm.childY = yCur;
        int off = 0, w = 0, h = 0;
        measureChild(cm.childVert, off, w, h);
        wMax = std::max(wMax, w);
        yCur += h;
    }

    satRow(cl.satDowns, in.downWH, cm.downs);

    // Loose cells and free runs, wrapped rows under everything else. The
    // budget is per-cluster: enough for the widest item, aiming at a
    // roughly landscape block.
    {
        struct Item {
            bool isRun = false;
            std::uint32_t vi = 0;
            std::size_t run = 0;
            int w = 0, h = 0;
            bool vert = false;
        };
        std::vector<Item> items;
        for (std::size_t k = 0; k < cl.looseVerts.size(); ++k) {
            Item it;
            it.vi = cl.looseVerts[k];
            it.vert = flow.verts[it.vi].kind == FlowVertex::Kind::Vertical;
            it.w = in.looseWH[k].first;
            it.h = in.looseWH[k].second;
            items.push_back(it);
        }
        for (std::size_t riIdx = 0; riIdx < cl.freeRuns.size(); ++riIdx) {
            Item it;
            it.isRun = true;
            it.run = riIdx;
            it.w = in.runWH[riIdx].first;
            it.h = in.runWH[riIdx].second;
            items.push_back(it);
        }
        if (!items.empty()) {
            std::int64_t area = 0;
            int widest = wMax;
            for (const Item& it : items) {
                area += static_cast<std::int64_t>(it.w) * it.h;
                widest = std::max(widest, it.w);
            }
            const int budget =
                std::max(widest, static_cast<int>(isqrtCeil(area * 3 / 2)));
            int x = 0, rowH = 0;
            for (const Item& it : items) {
                if (x > 0 && x + it.w > budget) {
                    x = 0;
                    yCur += roundUpP(rowH) + P;
                    rowH = 0;
                }
                if (it.isRun) {
                    RunDraw rd;
                    rd.g = std::move(in.runs[it.run]);
                    rd.x = x;
                    rd.y = yCur;
                    cm.runs.push_back(std::move(rd));
                } else {
                    cm.loose.push_back(LooseDraw{it.vi, x, yCur, it.vert});
                }
                x += roundUpP(it.w) + 2 * P;
                rowH = std::max(rowH, it.h);
                wMax = std::max(wMax, x);
            }
            yCur += roundUpP(rowH) + P;
        }
    }

    cm.w = roundUpP(wMax);
    cm.h = roundUpP(yCur);
}

void RoomPlacer::clusterMetrics() {
    mets.assign(plan.clusters.size(), {});
    std::vector<MIn> ins(plan.clusters.size());
    for (std::size_t ci = 0; ci < plan.clusters.size(); ++ci) measureCluster(ci, ins[ci]);
    unifyMetrics(ins);
    for (std::size_t ci = 0; ci < plan.clusters.size(); ++ci) composeCluster(ci, ins[ci]);
}

// Shelf rows over the cluster cells: packing order is the plan's own (rank,
// order, free cluster last), except that equal-shapeKey clusters of one rank
// pack ADJACENTLY -- the group enters at its first member's (rank, order)
// slot with members following in their own order, so repeated identical
// channels stand side by side the way a reference sheet draws them. Gutters
// 4P between anchor-bearing cells, 2P otherwise; within a row the anchor
// body TOPS align (a cell may extend upward further than its neighbour
// because of pull-up or decap rows).
void RoomPlacer::packClusters() {
    std::vector<std::size_t> live;
    std::vector<char> taken(mets.size(), 0);
    for (std::size_t i = 0; i < mets.size(); ++i) {
        if (taken[i] || mets[i].w <= 0 || mets[i].h <= 0) continue;
        taken[i] = 1;
        live.push_back(i);
        const std::string& key = plan.clusters[i].shapeKey;
        if (key.empty()) continue;
        for (std::size_t j = i + 1; j < mets.size(); ++j) {
            if (taken[j] || mets[j].w <= 0 || mets[j].h <= 0) continue;
            if (plan.clusters[j].rank != plan.clusters[i].rank) continue;
            if (plan.clusters[j].shapeKey != key) continue;
            taken[j] = 1;
            live.push_back(j);
        }
    }
    std::int64_t area = 0;
    int widest = 0;
    for (std::size_t i : live) {
        area += static_cast<std::int64_t>(mets[i].w) * mets[i].h;
        widest = std::max(widest, mets[i].w);
    }
    const int budget = std::max(widest, static_cast<int>(isqrtCeil(area * 3 / 2)));

    int rowY = 0, x = 0;
    bool prevAnchor = false;
    std::vector<std::size_t> row;
    auto flushRow = [&]() {
        if (row.empty()) return;
        int maxOff = 0;
        for (std::size_t j : row) {
            if (mets[j].hasBody) maxOff = std::max(maxOff, mets[j].bodyOff);
        }
        int rowH = 0;
        for (std::size_t j : row) {
            const int drop = mets[j].hasBody ? maxOff - mets[j].bodyOff : 0;
            mets[j].y = rowY + drop;
            rowH = std::max(rowH, drop + mets[j].h);
        }
        rowY += roundUpP(rowH) + 2 * P;
        row.clear();
    };
    for (std::size_t i : live) {
        int gutter = row.empty() ? 0 : ((prevAnchor && mets[i].hasBody) ? 4 * P : 2 * P);
        if (!row.empty() && x + gutter + mets[i].w > budget) {
            flushRow();
            x = 0;
            gutter = 0;
        }
        mets[i].x = x + gutter;
        x = mets[i].x + mets[i].w;
        row.push_back(i);
        prevAnchor = mets[i].hasBody;
    }
    flushRow();
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

    const bool tapToBar = !p.nc && tapsHere(p.net);
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
            // A left/right/bottom pin of the cluster's own bar rail: the bare
            // stub's free end is the tap point (top-side pins never reach
            // here -- drawAnchor intercepts them with a riser).
            addTap(sx, sy, p.net, side);
        } else {
            netPts[static_cast<std::size_t>(p.net)].push_back(StubPt{sx, sy, side});
        }
        return;
    }

    // A Drawn net's every pin sits on plan geometry that WAS drawn, so no
    // pin of one may reach this fallback; the debug build proves it, and the
    // stub+mark below keeps even a hypothetical escapee connected by name.
    assert(p.nc || p.net < 0 ||
           plan.netState[static_cast<std::size_t>(p.net)] != NetState::Drawn);

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
// Cells the plan keeps as their own bodies: loose parts, satellites,
// children. Each is placed at a P-aligned position its cluster's metrics
// reserved, so no collision search is needed.
// ---------------------------------------------------------------------------

// A loose body: the symbol with per-pin furniture. A top-side pin on the
// cluster's own bar rail defers to the tap pass instead of drawing anything.
void RoomPlacer::placeBody(std::uint32_t vi, int colX, int off, int y) {
    const std::uint32_t comp = flow.verts[vi].comps[0];
    const SymbolGeom& g = cache[comp];
    int extL = 0, extR = 0, extT = 0, extB = 0;
    bodyExtents(comp, extL, extR, extT, extB);
    (void)off;

    PlacedSymbol placed;
    placed.component = comp;
    placed.geom = g;
    placed.x = colX + extL;
    placed.y = y + extT;
    buf.reserve(Rect{placed.x - 2, y, placed.x + g.w + 2, placed.y + g.h + extB});

    for (std::size_t gp = 0; gp < g.pins.size(); ++gp) {
        const SymPin& p = g.pins[gp];
        int px = 0, py = 0;
        Side side = Side::Left;
        pinPos(placed, gp, px, py, side);
        if (side == Side::Top && !p.nc && tapsHere(p.net)) {
            buf.wire({px, py, px, y}, p.net);
            buf.reserveWire(Rect{px - 2, y, px + 2, py});
            addTap(px, y, p.net, Side::Top);
            continue;
        }
        pinStub(placed, gp);
    }
    buf.symbols.push_back(std::move(placed));
    buf.grow(Rect{colX, y, colX + extL + g.w + extR, y + extT + g.h + extB});
}

// A standing two-terminal cell: mark (or bare stub, or a bar tap's riser)
// above, body, mark (or bare stub) below. The attach rows sit on the P grid
// so a bare end is routable.
void RoomPlacer::placeVerticalCell(std::uint32_t vi, int colX, int y) {
    const std::uint32_t comp = flow.verts[vi].comps[0];
    std::uint32_t topPin = 0;
    std::int32_t topNet = -1, botNet = -1;
    vertOrient(comp, topPin, topNet, botNet);
    const int cw = verticalCellW(topNet, botNet);
    const int cx = colX + roundUpP(cw / 2);

    PlacedSymbol s;
    s.component = comp;
    s.geom = cache[comp];
    s.rot = verticalRot(s.geom, topPin);
    int gp = geomPinFor(s.geom, topPin);
    int lx = 0, ly = 0, rx = 0, ry = 0;
    localPin(s.geom, s.geom.pins[static_cast<std::size_t>(gp < 0 ? 0 : gp)], lx, ly);
    rotatePoint(s.geom, s.rot, lx, ly, rx, ry);

    const int attachY = y + 2 * P;
    const int bodyTop = y + 3 * P;
    s.x = cx - rx;
    s.y = bodyTop;
    const int bodyBot = bodyTop + rotatedH(s.geom, s.rot);

    const bool bareTop = isRoutable(topNet);
    const bool tapTop = tapsHere(topNet);
    const bool bareBot = isRoutable(botNet);
    if (tapTop) {
        // A pull-up or lone cap on the cluster's own bar rail: the attach
        // wire runs to the cell's top edge and becomes the tap's riser
        // instead of taking a rail flag.
        buf.wire({cx, y, cx, bodyTop}, topNet);
        buf.reserveWire(Rect{cx - 2, y, cx + 2, bodyTop});
        addTap(cx, y, topNet, Side::Top);
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

    const int cellH = roundUpP(3 * P + (bodyBot - bodyTop) + P + 16);
    const int cellW = roundUpP(cw / 2) + cw / 2 + 24;
    // The solid stops at a bare end's stub row, so the router can leave it;
    // a tapped top keeps its whole riser clear the same way.
    const int top = bareTop ? attachY : tapTop ? bodyTop : y;
    const int bot = bareBot ? bodyBot + P : y + cellH;
    buf.reserve(Rect{cx - cw / 2, top, cx + cw / 2 + 24, bot});
    buf.grow(Rect{colX, y, colX + cellW, y + cellH});
}

// A child block's sheet symbol: the classic green box, ports down the left
// edge, stub and mark per port (port nets are never routed).
void RoomPlacer::placeChild(std::uint32_t vi, int colX, int off, int y) {
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

    buf.reserve(Rect{colX, y, colX + off + w + P, y + roundUpP(topPad + h + P)});
}

// ---------------------------------------------------------------------------
// The decap row: one short rail segment with its ladder hanging below --
// the classic placeBars interior, cluster-local. finishBars later widens the
// bar to the cluster's consumer tap columns, never to the room.
// ---------------------------------------------------------------------------

void RoomPlacer::drawDecapRow(const DecapGroup& gr, const DecapDraw& dd, int cx, int cy) {
    const int rowY = cy + dd.y;
    // 12 above a P-multiple: never on the routing grid, so no routed wire
    // can ever run collinearly along the bar.
    const int barY = rowY + 12;
    const int firstTapX = cx + dd.ladderX;
    barOf[static_cast<std::size_t>(gr.rail)] = static_cast<std::int32_t>(buf.bars.size());
    const int n = static_cast<int>(gr.comps.size());
    if (n == 0) {
        // A pin-only segment: the bar alone, seeded mid-body; finishBars
        // widens it to the consumer tap columns and joins them.
        buf.bars.push_back(RailBarItem{firstTapX - P, firstTapX + P, barY, gr.rail});
        return;
    }
    buf.bars.push_back(
        RailBarItem{firstTapX - P, firstTapX + (n - 1) * 3 * P + P, barY, gr.rail});

    int tapX = firstTapX;
    int bottom = barY;
    for (std::uint32_t comp : gr.comps) {
        buf.dots.push_back(DotItem{tapX, barY, gr.rail});
        buf.wire({tapX, barY, tapX, barY + P}, gr.rail);
        PlacedSymbol s;
        s.component = comp;
        s.geom = cache[comp];
        std::uint32_t railPin = d.components[comp].pins[0].net == gr.rail ? 0u : 1u;
        s.rot = verticalRot(s.geom, railPin);
        int gp = geomPinFor(s.geom, railPin);
        int lx = 0, ly = 0, rx = 0, ry = 0;
        localPin(s.geom, s.geom.pins[static_cast<std::size_t>(gp < 0 ? 0 : gp)], lx, ly);
        rotatePoint(s.geom, s.rot, lx, ly, rx, ry);
        s.x = tapX - rx;
        s.y = barY + P;
        const int bodyBot = s.y + rotatedH(s.geom, s.rot);
        const std::int32_t gnd = d.components[comp].pins[railPin == 0 ? 1u : 0u].net;
        buf.wire({tapX, bodyBot, tapX, bodyBot + P}, gnd);
        buf.mark(MarkKind::Ground, tapX, bodyBot + P, Side::Bottom, gnd);
        buf.symbols.push_back(std::move(s));
        bottom = std::max(bottom, bodyBot + P + 10);
        tapX += 3 * P;
    }
    // The block solid covers the ladder and the name at the bar's left end;
    // the bar LINE itself is reserved (widened) by finishBars.
    buf.reserve(Rect{firstTapX - 2 * P, rowY, (tapX - 3 * P) + 2 * P + 30, bottom + 4});
}

// ---------------------------------------------------------------------------
// The strip terminal, shared by arteries and free runs.
// ---------------------------------------------------------------------------

int RoomPlacer::drawStripEnd(StripEnd end, std::int32_t net, int cursor, int stripY, int dir,
                             int riserTopY) {
    const Side outSide = dir > 0 ? Side::Right : Side::Left;
    switch (end) {
        case StripEnd::Bare:
            buf.wire({cursor, stripY, cursor + dir * P, stripY}, net);
            return cursor;
        case StripEnd::Handoff: {
            const int ex2 =
                dir > 0 ? roundUpP(cursor + P) : roundDownP(cursor - P);
            buf.wire({cursor, stripY, ex2, stripY}, net);
            buf.reserveWire(Rect{std::min(cursor, ex2) - 2, stripY - 2,
                                 std::max(cursor, ex2) + 2, stripY + 2});
            netPts[static_cast<std::size_t>(net)].push_back(StubPt{ex2, stripY, outSide});
            return cursor;
        }
        case StripEnd::Ground:
            buf.wire({cursor, stripY, cursor + dir * P, stripY, cursor + dir * P, stripY + P},
                     net);
            buf.mark(MarkKind::Ground, cursor + dir * P, stripY + P, Side::Bottom, net);
            return cursor + dir * (P + 10);
        case StripEnd::RailFlag: {
            // A lower strip is classified a flag because its riser cannot be
            // GUARANTEED at measure time. At draw time everything above the
            // strip is already reserved, so when this cluster owns a segment
            // on the rail and the column is actually clear, take the tap
            // after all -- the strip joins the one named segment instead of
            // repeating the name. The measured flag headroom stays reserved,
            // which only leaves slack.
            const int tx = cursor + dir * P;
            if (tapsHere(net) && riserTopY < stripY - P &&
                riserClear(net, tx, riserTopY, stripY - 4)) {
                buf.wire({cursor, stripY, tx, stripY, tx, riserTopY}, net);
                buf.reserveWire(Rect{tx - 2, riserTopY, tx + 2, stripY});
                addTap(tx, riserTopY, net, Side::Top);
                return cursor + dir * (P + 4);
            }
            buf.wire({cursor, stripY, tx, stripY, tx, stripY - P}, net);
            buf.mark(MarkKind::RailFlag, tx, stripY - P, Side::Top, net);
            return cursor + dir * (P + textW(netName(pg, net)) / 2 + 8);
        }
        case StripEnd::RailTap: {
            const int tx = cursor + dir * P;
            buf.wire({cursor, stripY, tx, stripY, tx, riserTopY}, net);
            buf.reserveWire(Rect{tx - 2, riserTopY, tx + 2, stripY});
            addTap(tx, riserTopY, net, Side::Top);
            return cursor + dir * (P + 4);
        }
        case StripEnd::Mark:
            buf.wire({cursor, stripY, cursor + dir * P, stripY}, net);
            buf.mark(markKindFor(pg, net), cursor + dir * P, stripY, outSide, net);
            return cursor + dir * markTail(pg, net);
        case StripEnd::JuncNamed:
        case StripEnd::JuncBare: break;  // the artery handles its own trunk end
    }
    return cursor;
}

// ---------------------------------------------------------------------------
// The artery: lead-in at the pin's own row, one jog at midX, then the classic
// element stepping; at a junction the trunk runs on with shunt strings at
// their tap columns; the terminal is the classic switch. Junction dots are
// counted over the drawn segments.
// ---------------------------------------------------------------------------

void RoomPlacer::drawArtery(const PlacedSymbol& anchor, const ArtGeom& ag, int zoneTopY) {
    int px = 0, py = 0;
    Side side = Side::Left;
    pinPos(anchor, static_cast<std::size_t>(ag.gp), px, py, side);
    (void)side;  // the plan's bucket already fixed the direction
    const int dir = ag.rightward ? 1 : -1;
    const int midX = px + dir * ag.midOff;
    const int stripY = anchor.y + ag.stripRel;
    const std::int32_t startNet = ag.art->startNet;
    const std::int32_t jnet = ag.hasJunc ? ag.junc.net : -1;

    std::vector<Seg> jsegs;

    // The Manhattan lead-in: straight when the strip kept the pin's row,
    // else one jog at midX. The lead is a wire: it may cross other wires.
    if (stripY == py) {
        buf.wire({px, py, midX, py}, startNet);
    } else {
        buf.wire({px, py, midX, py, midX, stripY}, startNet);
    }
    buf.reserveWire(ag.rightward ? Rect{px + 4, py - 2, midX + 2, py + 2}
                                 : Rect{midX - 2, py - 2, px - 4, py + 2});
    if (stripY != py) {
        buf.reserveWire(
            Rect{midX - 2, std::min(py, stripY), midX + 2, std::max(py, stripY)});
    }
    if (startNet == jnet) {
        jsegs.push_back(Seg{px, py, midX, py});
        if (stripY != py) jsegs.push_back(Seg{midX, py, midX, stripY});
    }

    int cursor = midX;
    std::int32_t net = startNet;
    std::size_t ei = 0;
    for (const ArteryStep& step : ag.art->steps) {
        if (step.kind == ArteryStep::Kind::Inline) {
            const ElemGeom& e = ag.inls[ei++];
            const int entryX = cursor + dir * P;
            buf.wire({cursor, stripY, entryX, stripY}, net);
            if (net == jnet) jsegs.push_back(Seg{cursor, stripY, entryX, stripY});

            PlacedSymbol s;
            s.component = e.comp;
            s.geom = cache[e.comp];
            s.rot = e.rot;
            int lx = 0, ly = 0, ex = 0, ey = 0;
            localPin(s.geom, s.geom.pins[static_cast<std::size_t>(e.gp)], lx, ly);
            rotatePoint(s.geom, s.rot, lx, ly, ex, ey);
            s.x = entryX - ex;
            s.y = stripY - ey;
            const int exitIdx = e.gp == 0 ? 1 : 0;
            int ox = 0, oy = 0;
            localPin(s.geom, s.geom.pins[static_cast<std::size_t>(exitIdx)], lx, ly);
            rotatePoint(s.geom, s.rot, lx, ly, ox, oy);
            cursor = s.x + ox;
            net = d.components[e.comp]
                      .pins[s.geom.pins[static_cast<std::size_t>(exitIdx)].pin]
                      .net;
            buf.symbols.push_back(std::move(s));
            continue;
        }

        // The junction: the trunk runs to the last tap column; each shunt
        // string walks away from it one part at a time -- a gap wire, a
        // body, the next gap on whatever net that body exits onto -- and
        // ONLY the last gap carries the end mark.
        const JuncGeom& jg = ag.junc;
        const int n = static_cast<int>(jg.shunts.size());
        if (n > 0) {
            const int lastX = cursor + dir * jg.lastTap;
            buf.wire({cursor, stripY, lastX, stripY}, jg.net);
            jsegs.push_back(Seg{cursor, stripY, lastX, stripY});
            for (int k = 0; k < n; ++k) {
                const ShuntGeom& sh = jg.shunts[static_cast<std::size_t>(k)];
                const int tapX = cursor + dir * (jg.firstTap + k * jg.pitch);
                const int sgn = sh.up ? -1 : 1;
                int vc = stripY;
                std::int32_t link = jg.net;
                for (std::size_t i = 0; i < sh.elems.size(); ++i) {
                    const VElem& ve = sh.elems[i];
                    buf.wire({tapX, vc, tapX, vc + sgn * P}, link);
                    if (i == 0) jsegs.push_back(Seg{tapX, vc, tapX, vc + sgn * P});
                    vc += sgn * P;

                    PlacedSymbol s;
                    s.component = ve.comp;
                    s.geom = cache[ve.comp];
                    s.rot = ve.rot;
                    int lx = 0, ly = 0, rx = 0, ry = 0;
                    localPin(s.geom, s.geom.pins[static_cast<std::size_t>(ve.gp)], lx, ly);
                    rotatePoint(s.geom, s.rot, lx, ly, rx, ry);
                    // The entry pin lands on the cursor; ry is 0 hanging
                    // below and the body's height standing above.
                    s.x = tapX - rx;
                    s.y = vc - ry;
                    vc += sgn * ve.h;
                    const int exitIdx = ve.gp == 0 ? 1 : 0;
                    link = d.components[ve.comp]
                               .pins[s.geom.pins[static_cast<std::size_t>(exitIdx)].pin]
                               .net;
                    buf.symbols.push_back(std::move(s));
                }
                buf.wire({tapX, vc, tapX, vc + sgn * P}, sh.endNet);
                if (sh.endNet >= 0) {
                    // A routable end hands the router a bare grid stub, the
                    // vertical twin of drawStripEnd's Handoff -- this is what
                    // lets a bootstrap cap's far side reach its pin as copper
                    // instead of a label pair. Shunt columns sit off the P
                    // grid, so the free end jogs onto it: down a step, across
                    // to the nearest grid column, out to the grid row.
                    const int yj = vc + sgn * P;
                    const int gx = tapX % P <= P / 2 ? roundDownP(tapX) : roundUpP(tapX);
                    const int gy = sgn > 0 ? roundUpP(yj) : roundDownP(yj);
                    if (isRoutable(sh.endNet) && gx >= 0 && gy >= 0) {
                        if (gx != tapX) buf.wire({tapX, yj, gx, yj}, sh.endNet);
                        if (gy != yj) buf.wire({gx, yj, gx, gy}, sh.endNet);
                        buf.reserveWire(Rect{std::min(tapX, gx) - 2, std::min(yj, gy) - 2,
                                             std::max(tapX, gx) + 2, std::max(yj, gy) + 2});
                        netPts[static_cast<std::size_t>(sh.endNet)].push_back(
                            StubPt{gx, gy, sh.up ? Side::Top : Side::Bottom});
                    } else {
                        buf.mark(markKindFor(pg, sh.endNet), tapX, yj,
                                 sh.up ? Side::Top : Side::Bottom, sh.endNet);
                    }
                }
            }
            cursor = lastX;
        }
        net = jg.net;
    }

    // The terminal.
    int bandX1 = cursor;
    switch (ag.end) {
        case StripEnd::JuncNamed: {
            const int ex2 = cursor + dir * P;
            buf.wire({cursor, stripY, ex2, stripY}, net);
            jsegs.push_back(Seg{cursor, stripY, ex2, stripY});
            buf.mark(markKindFor(pg, net), ex2, stripY,
                     ag.rightward ? Side::Right : Side::Left, net);
            bandX1 = cursor + dir * markTail(pg, net);
            break;
        }
        case StripEnd::JuncBare:
            // Fully drawn: the trunk stops dead on the last tap, which is
            // then a corner and takes no dot.
            bandX1 = cursor + dir * (ag.junc.pitch / 2 + 4);
            break;
        default:
            bandX1 = drawStripEnd(ag.end, ag.art->endNet, cursor, stripY, dir, zoneTopY);
            break;
    }

    // The band solid: element bodies, tap strings and marks. It starts past
    // the jog column (a wire) and stops short of a bare handoff end so the
    // router can leave the stub.
    const int bandX0 = midX + dir * 4;
    buf.reserve(Rect{std::min(bandX0, bandX1), stripY - ag.up, std::max(bandX0, bandX1),
                     stripY + ag.dn});

    // Junction dots are counted, never assumed.
    if (jnet >= 0) addJunctionDots(buf.dots, jsegs, jnet);
}

// A free run: start mark (or bare stub, or riser), the elements, the shared
// terminal. Interior nets are drawn conductors, exactly like an artery's.
void RoomPlacer::drawRun(const RunDraw& rd, int cx, int cy) {
    const RunGeom& rg = rd.g;
    const int startX = cx + rd.x + rg.startExt;
    const int up = roundUpP(rg.up);
    const int stripY = cy + rd.y + up;
    const int rowTopY = cy + rd.y;

    if (rg.startNet >= 0 && isRoutable(rg.startNet)) {
        netPts[static_cast<std::size_t>(rg.startNet)].push_back(
            StubPt{startX, stripY, Side::Left});
    } else if (tapsHere(rg.startNet)) {
        buf.wire({startX, stripY, startX, rowTopY}, rg.startNet);
        buf.reserveWire(Rect{startX - 2, rowTopY, startX + 2, stripY});
        addTap(startX, rowTopY, rg.startNet, Side::Top);
    } else if (rg.startNet >= 0) {
        buf.mark(markKindFor(pg, rg.startNet), startX, stripY, Side::Left, rg.startNet);
    }

    int cursor = startX;
    std::int32_t net = rg.startNet;
    for (const ElemGeom& e : rg.elems) {
        const int entryX = cursor + P;
        buf.wire({cursor, stripY, entryX, stripY}, net);
        PlacedSymbol s;
        s.component = e.comp;
        s.geom = cache[e.comp];
        s.rot = e.rot;
        int lx = 0, ly = 0, ex = 0, ey = 0;
        localPin(s.geom, s.geom.pins[static_cast<std::size_t>(e.gp)], lx, ly);
        rotatePoint(s.geom, s.rot, lx, ly, ex, ey);
        s.x = entryX - ex;
        s.y = stripY - ey;
        const int exitIdx = e.gp == 0 ? 1 : 0;
        int ox = 0, oy = 0;
        localPin(s.geom, s.geom.pins[static_cast<std::size_t>(exitIdx)], lx, ly);
        rotatePoint(s.geom, s.rot, lx, ly, ox, oy);
        cursor = s.x + ox;
        net = d.components[e.comp]
                  .pins[s.geom.pins[static_cast<std::size_t>(exitIdx)].pin]
                  .net;
        buf.symbols.push_back(std::move(s));
    }

    const int bandX1 = drawStripEnd(rg.end, rg.endNet, cursor, stripY, 1, rowTopY);
    const bool bareStart = rg.startNet >= 0 && isRoutable(rg.startNet);
    const int bandX0 = bareStart ? startX : cx + rd.x;
    buf.reserve(Rect{bandX0, stripY - rg.up, std::max(bandX1, cursor), stripY + rg.dn});
    buf.grow(Rect{cx + rd.x, cy + rd.y, cx + rd.x + rg.startExt + rg.inner + P,
                  cy + rd.y + up + rg.dn});
}

// ---------------------------------------------------------------------------
// The anchor: body, risers for its own bar rails, arteries, and the classic
// stub+mark furniture on every pin nothing consumed.
// ---------------------------------------------------------------------------

void RoomPlacer::drawAnchor(CMetric& cm, int cx, int cy) {
    const std::uint32_t comp = cm.anchorComp;
    const SymbolGeom& g = cache[comp];
    int extL = 0, extR = 0, extT = 0, extB = 0;
    bodyExtents(comp, extL, extR, extT, extB);

    PlacedSymbol placed;
    placed.component = comp;
    placed.geom = g;
    placed.x = cx + cm.bodyX;
    placed.y = cy + cm.bodyY;
    const int zoneTopY = cy + cm.zoneTop;
    // One solid over the body, the riser column strip above and the mark
    // strip below; left/right strips are reserved per pin.
    buf.reserve(Rect{placed.x - 2, zoneTopY, placed.x + g.w + 2, placed.y + g.h + extB});

    std::vector<char> done(d.components[comp].pins.size(), 0);
    for (const ArtGeom& ag : cm.arts) {
        drawArtery(placed, ag, zoneTopY);
        done[ag.art->anchorPin] = 1;
    }

    for (std::size_t gp = 0; gp < g.pins.size(); ++gp) {
        const SymPin& p = g.pins[gp];
        if (done[p.pin]) continue;
        int px = 0, py = 0;
        Side side = Side::Left;
        pinPos(placed, gp, px, py, side);
        if (side == Side::Top && !p.nc && tapsHere(p.net)) {
            // Riser to the zone's top edge, through the pin's own mark
            // strip; the tap point is outside every solid, so the routed
            // fallback can leave it when the straight corridor is blocked.
            buf.wire({px, py, px, zoneTopY}, p.net);
            buf.reserveWire(Rect{px - 2, zoneTopY, px + 2, py});
            addTap(px, zoneTopY, p.net, Side::Top);
            continue;
        }
        pinStub(placed, gp);
    }
    buf.symbols.push_back(std::move(placed));
}

// ---------------------------------------------------------------------------
// The room: every cluster cell drawn at its packed position.
// ---------------------------------------------------------------------------

void RoomPlacer::drawClusters() {
    barOf.assign(pg.nets.size(), -1);
    for (std::size_t ci = 0; ci < plan.clusters.size(); ++ci) {
        curCluster = ci;
        const Cluster& cl = plan.clusters[ci];
        CMetric& cm = mets[ci];
        if (cm.w <= 0 || cm.h <= 0) continue;
        const int cx = cm.x, cy = cm.y;

        for (const DecapDraw& dd : cm.decaps) drawDecapRow(cl.decaps[dd.idx], dd, cx, cy);
        for (const SatDraw& s : cm.ups) placeVerticalCell(s.vi, cx + s.x, cy + s.y);
        if (cm.hasBody) drawAnchor(cm, cx, cy);
        if (cm.isChild) {
            int off = 0, w = 0, h = 0;
            measureChild(cm.childVert, off, w, h);
            placeChild(cm.childVert, cx, off, cy + cm.childY);
        }
        for (const SatDraw& s : cm.downs) placeVerticalCell(s.vi, cx + s.x, cy + s.y);
        for (const LooseDraw& l : cm.loose) {
            if (l.vert) placeVerticalCell(l.vi, cx + l.x, cy + l.y);
            else placeBody(l.vi, cx + l.x, 0, cy + l.y);
        }
        for (const RunDraw& rd : cm.runs) drawRun(rd, cx, cy);

        buf.grow(Rect{cx, cy, cx + cm.w, cy + cm.h});
    }
}

// ---------------------------------------------------------------------------
// Bars: widen each cluster's rail segment to its own tap columns (group
// width, never the room's), then join every tap -- straight corridor drop
// first, routed fallback second, and only when both refuse the classic
// per-pin rail flag.
// ---------------------------------------------------------------------------

void RoomPlacer::finishBars() {
    if (buf.bars.empty()) return;
    // Group width: the bar reaches exactly as far as its cluster's taps --
    // each tap widens the bar it was recorded against, never a namesake
    // segment another cluster owns on the same rail.
    for (const Tap& t : taps) {
        RailBarItem& bar = buf.bars[static_cast<std::size_t>(t.bar)];
        bar.x1 = std::min(bar.x1, t.px - P);
        bar.x2 = std::max(bar.x2, t.px + P);
    }
    for (RailBarItem& bar : buf.bars) {
        buf.reserveWire(Rect{bar.x1, bar.y - 2, bar.x2, bar.y + 2});
    }

    // The straight drop must clear every solid AND every foreign vertical
    // wire astride its line -- an earlier tap's routed leg, a bare stub --
    // or two nets would read as one conductor (riserClear's rule exactly).
    auto corridorClear = [&](std::int32_t net, int px, int barY, int bottom) {
        return bottom > barY + 4 && riserClear(net, px, barY + 4, bottom);
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

    std::vector<std::int32_t> dropped;  // nets with a corridor drop, tap order
    for (const Tap& t : taps) {
        const RailBarItem& bar = buf.bars[static_cast<std::size_t>(t.bar)];
        const bool inSpan = t.px >= bar.x1 && t.px <= bar.x2;
        if (inSpan && corridorClear(t.net, t.px, bar.y, t.py)) {
            buf.wire({t.px, bar.y, t.px, t.py}, t.net);
            buf.reserveWire(Rect{t.px - 2, bar.y, t.px + 2, t.py});
            // The bar runs through, the tap ends: three conductors, one dot.
            dotOnce(t.px, bar.y, t.net);
            bool seen = false;
            for (std::int32_t n : dropped) seen = seen || n == t.net;
            if (!seen) dropped.push_back(t.net);
        } else if (tryRoute(t, bar)) {
            // Routed around the blockage; the router dotted the bar joint.
        } else {
            buf.mark(MarkKind::RailFlag, t.px, t.py, t.fbSide, t.net);
        }
    }

    // Two stacked taps of one column (a connector's paired supply pins) put
    // the lower pin's corridor straight through the upper pin's stub end: a
    // T-join of the rail's own wires. Junction dots are counted, never
    // assumed, so recount over every wire the dropped nets now own; the
    // coordinate dedup keeps the dots dotOnce already placed single.
    for (std::int32_t net : dropped) {
        std::vector<Seg> segs;
        for (const WireItem& w : buf.wires) {
            if (w.net != net) continue;
            for (std::size_t i = 0; i + 3 < w.pts.size(); i += 2) {
                segs.push_back(Seg{w.pts[i], w.pts[i + 1], w.pts[i + 2], w.pts[i + 3]});
            }
        }
        std::vector<DotItem> fresh;
        addJunctionDots(fresh, segs, net);
        for (const DotItem& nd : fresh) {
            bool present = false;
            for (const DotItem& e : buf.dots) {
                if (e.x == nd.x && e.y == nd.y) present = true;
            }
            if (!present) buf.dots.push_back(nd);
        }
    }
}

// ---------------------------------------------------------------------------
// Routing, in net index order. Success needs no marks -- the wire says it,
// and junction dots are the router's job. Failure adds the label each pin
// would have had, so the pin-conductor invariant holds either way.
// ---------------------------------------------------------------------------

void RoomPlacer::routeAll() {
    // A handoff whose free end was buried by later furniture -- a cluster
    // band's solid reserved after the string drew -- pokes out along its own
    // direction to the first free grid cell, so the router can reach it.
    for (std::size_t ni = 0; ni < d.nets.size(); ++ni) {
        if (!routable[ni]) continue;
        for (StubPt& sp : netPts[ni]) {
            int guard = 0;
            int x0 = sp.x, y0 = sp.y;
            while (guard++ < 16 && sp.x >= 0 && sp.y >= 0 &&
                   buf.collides(Rect{sp.x, sp.y, sp.x, sp.y})) {
                switch (sp.side) {
                    case Side::Left: sp.x -= P; break;
                    case Side::Right: sp.x += P; break;
                    case Side::Top: sp.y -= P; break;
                    case Side::Bottom: sp.y += P; break;
                }
            }
            if (sp.x != x0 || sp.y != y0) {
                buf.wire({x0, y0, sp.x, sp.y}, static_cast<std::int32_t>(ni));
                buf.reserveWire(Rect{std::min(x0, sp.x) - 2, std::min(y0, sp.y) - 2,
                                     std::max(x0, sp.x) + 2, std::max(y0, sp.y) + 2});
            }
        }
    }

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
    plan = buildRoomPlan(pg, room, flow, plans, m);
    computeRoutable();
    // The plan outranks the router: a net it drew (or named) is spoken for.
    for (std::size_t ni = 0; ni < routable.size(); ++ni) {
        if (plan.netState[ni] != NetState::Free) routable[ni] = 0;
    }
    clusterMetrics();
    packClusters();
    drawClusters();
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
        RoomPlacer rp(model, page, cache, room, flows[ri], plans);
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
    // reference schematics instead of portrait strips.
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
