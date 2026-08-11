// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classic engine: rooms, then idioms inside each room, then shelf-packed
// rooms on the sheet. There is deliberately NO general-purpose router here.
// Collision handling is one axis per element kind: chain and node strips move
// DOWN (y += P), pull-up/pull-down/ladder columns move RIGHT (x += P). A wire
// that still cannot be placed is not drawn at all -- the connection keeps its
// labels, which connect by name.
//
// Moved whole from layout.cpp; the geometry primitives it shares with the
// Flow pipeline live in geometry.h.
#include "render/place_classic.h"

#include <algorithm>

namespace manta::render {

namespace {

constexpr int kSheetMargin = 40;
constexpr int kTitleStrip = 14;   // the room's blue title band
constexpr int kTitleBlockH = 70;
constexpr int kVertLen = 88;       // top mark + stub + body + stub + bottom mark

// ---------------------------------------------------------------------------
// Per-room working set.
// ---------------------------------------------------------------------------

enum class Role : std::uint8_t { Ladder, PullUp, PullDown, Chain, Leftover };

struct Cand {
    std::uint32_t comp = 0;
    Role role = Role::Leftover;
    std::int32_t railNet = -1;  // Ladder, PullUp
    std::int32_t sigNet = -1;   // PullUp, PullDown
    std::uint32_t railPin = 0;  // component pin on the rail (PullDown: on ground)
    std::uint32_t sigPin = 0;
    bool consumed = false;
};

struct RunElem {
    std::size_t cand = 0;        // index into the room's candidate list
    std::uint32_t entryPin = 0;  // component pin facing the source
};

struct Run {
    bool fromAnchor = false;
    std::uint32_t anchor = 0;       // component index
    std::uint32_t anchorPin = 0;    // component pin index the run starts at
    std::int32_t startNet = -1;
    std::vector<RunElem> elems;
    std::int32_t endNet = -1;
    bool downgraded = false;  // could not be drawn at its anchor: floats instead
};

// One two-terminal part hanging off a node's trunk by the pin on the node's
// net; the far pin carries the mark for whatever net it lands on.
// One string of two-terminal parts hanging off a node's trunk, walked the way
// a chain run is: `elems[0]` sits on the node's net, each part after it
// continues through a private net, and `endNet` is where the string lands.
// A tap is a whole run and not a single part because the string a trunk
// interrupts -- a divider, an LED and its resistor -- is one drawn path in
// every reference sheet, and cutting it at the first part would put a label
// back in the middle of a conductor that was already drawn.
struct NodeTap {
    std::vector<RunElem> elems;  // trunk-end first
    std::int32_t endNet = -1;
    bool up = false;  // a string landing on a rail stands above the trunk
};

// A net joining three or more pins, drawn as one conductor: legs out of an
// anchor's pins to a spine, a trunk along the spine, and a tap per claimed
// two-terminal part. `terminal` is set when some pin of the net is left
// outside this drawing -- another anchor, another room, another page -- and
// the trunk must carry a mark so the two halves still join by name.
struct Node {
    std::int32_t net = -1;
    std::uint32_t anchor = 0;             // component index the legs leave from
    std::vector<std::uint32_t> rootPins;  // that anchor's pins on the net
    std::vector<NodeTap> taps;
    bool terminal = true;
};

bool twoTerminalKind(SymbolKind k) {
    switch (k) {
        case SymbolKind::Resistor:
        case SymbolKind::Capacitor:
        case SymbolKind::CapacitorPolarised:
        case SymbolKind::Inductor:
        case SymbolKind::Ferrite:
        case SymbolKind::Diode:
        case SymbolKind::Zener:
        case SymbolKind::Tvs:
        case SymbolKind::Led:
        case SymbolKind::Crystal:
        case SymbolKind::Switch:
        case SymbolKind::Fuse: return true;
        default: return false;
    }
}

bool anchorKind(SymbolKind k, const Component& c) {
    switch (k) {
        case SymbolKind::Generic:
        case SymbolKind::Connector:
        case SymbolKind::OpAmp:
        case SymbolKind::Nmos:
        case SymbolKind::Pmos:
        case SymbolKind::Mosfet:
        case SymbolKind::Npn:
        case SymbolKind::Pnp: return true;
        default: return c.pins.size() > 4;
    }
}

// One room's layout state, built then placed.
struct RoomLayout {
    const RenderModel& m;
    const RenderPage& pg;
    const Design& d;
    const SymbolCache& cache;  // geometry by component index, prebuilt
    RoomBuf buf;

    std::vector<std::uint32_t> anchors;   // component indices, designator order
    std::vector<Cand> cands;
    std::vector<std::int32_t> candOf;     // component index -> cands index, -1
    std::vector<std::uint32_t> leftovers; // wires, crimps, test points...
    std::vector<std::uint32_t> children;  // Design::blocks indices, room order
    std::vector<Run> runs;                // anchor runs then floating runs
    std::vector<Node> nodes;              // multi-way nodes, net declaration order
    std::vector<std::uint32_t> orphans;   // taps of a node that could not be drawn

    RoomLayout(const RenderModel& model, const RenderPage& page, const SymbolCache& geoms)
        : m(model), pg(page), d(*model.design), cache(geoms),
          candOf(model.design->components.size(), -1) {}

    // A private net can carry a drawn chain wire: exactly two pins, no class,
    // not crossing, not a port.
    [[nodiscard]] bool isPrivate(std::int32_t net) const {
        if (net < 0) return false;
        const Net& n = d.nets[static_cast<std::size_t>(net)];
        const RenderNet& rn = pg.nets[static_cast<std::size_t>(net)];
        return n.pins.size() == 2 && rn.mark == NetMark::Label && !rn.crossing &&
               rn.direction == PortDir::None;
    }

    // The other endpoint of a two-pin net.
    [[nodiscard]] PinRef otherEnd(std::int32_t net, std::uint32_t comp, std::uint32_t pin) const {
        const Net& n = d.nets[static_cast<std::size_t>(net)];
        for (const PinRef& pr : n.pins) {
            if (pr.component != comp || pr.pin != pin) return pr;
        }
        return PinRef{comp, pin};
    }

    [[nodiscard]] Cand* consumable(const PinRef& pr) {
        std::int32_t ci = candOf[pr.component];
        if (ci < 0) return nullptr;
        Cand& c = cands[static_cast<std::size_t>(ci)];
        if (c.consumed) return nullptr;
        if (c.role != Role::Chain && c.role != Role::PullDown) return nullptr;
        return &c;
    }

    void classify(const RenderRoom& room);
    void claimNodes();
    void walkChains();
    void placeAll();

private:
    // Trunk metrics, shared by the width an anchor must reserve and by the
    // placement itself so the two can never disagree.
    struct NodeMetrics {
        int pitch = 0;     // centre-to-centre spacing of the taps
        int firstTap = 0;  // spine to the first tap
        int lastTap = 0;   // spine to the last tap
        int reserve = 0;   // spine to the far edge of what the trunk occupies
        int trunk = 0;     // spine to the drawn end of the trunk
    };
    [[nodiscard]] NodeMetrics nodeMetrics(const Node& node) const;
    [[nodiscard]] bool placeNode(const Node& node, const PlacedSymbol& anchor, int ext,
                                 int stagger);
    void walkForward(Run& run, std::int32_t net, std::uint32_t fromComp, std::uint32_t fromPin);
    int placeBand0(int y);
    int placeBand1(int y);
    int placeChildren(int y);
    void placeBand2(int y);
    void placeVertical(std::uint32_t comp, int cx, int topY, std::uint32_t topPin,
                       std::int32_t topNet, std::int32_t botNet);
    int verticalCellW(std::uint32_t comp, std::int32_t topNet, std::int32_t botNet) const;
    [[nodiscard]] bool placeRun(const Run& run, int px, int py, bool rightward, int midX);
    int innerLength(const Run& run) const;
    void placeLeftoverCell(std::uint32_t comp, int& x, int& y, int& rowH);
};

void RoomLayout::classify(const RenderRoom& room) {
    children = room.children;
    for (std::uint32_t idx : room.components) {
        const Component& c = d.components[idx];
        SymbolKind k = m.kinds[idx];
        if (anchorKind(k, c)) {
            anchors.push_back(idx);
            continue;
        }
        if (!twoTerminalKind(k) || c.pins.size() != 2) {
            leftovers.push_back(idx);
            continue;
        }
        Cand cand;
        cand.comp = idx;
        std::int32_t a = c.pins[0].net;
        std::int32_t b = c.pins[1].net;
        bool railA = isRail(pg, a), railB = isRail(pg, b);
        bool gndA = isGround(pg, a), gndB = isGround(pg, b);
        if ((railA && gndB) || (railB && gndA)) {
            cand.role = Role::Ladder;
            cand.railNet = railA ? a : b;
            cand.railPin = railA ? 0 : 1;
        } else if (railA != railB && !gndA && !gndB) {
            cand.role = Role::PullUp;
            cand.railNet = railA ? a : b;
            cand.railPin = railA ? 0 : 1;
            cand.sigNet = railA ? b : a;
            cand.sigPin = railA ? 1 : 0;
        } else if (gndA != gndB && !railA && !railB) {
            cand.role = Role::PullDown;
            cand.railPin = gndA ? 0 : 1;  // the ground pin
            cand.sigNet = gndA ? b : a;
            cand.sigPin = gndA ? 1 : 0;
        } else {
            cand.role = Role::Chain;
        }
        candOf[idx] = static_cast<std::int32_t>(cands.size());
        cands.push_back(cand);
    }
    std::sort(anchors.begin(), anchors.end(), [&](std::uint32_t a, std::uint32_t b) {
        const std::string& da = d.components[a].designator;
        const std::string& db = d.components[b].designator;
        if (naturalLess(da, db)) return true;
        if (naturalLess(db, da)) return false;
        return a < b;
    });
}

// ---------------------------------------------------------------------------
// Multi-way nodes. A net on three or more pins has no class of its own, so
// before this idiom every one of its pins degraded to the same repeated label
// and the node could not be read off the sheet at all.
//
// The node claims its participants BEFORE the chain walk, so a switch node
// wins its bypass capacitor from the two-pin chain that would otherwise take
// it: a drawn junction is worth more than one more series string, and the
// label count is unchanged either way -- whichever idiom loses the part, its
// own net picks up the marks the other net sheds.
// ---------------------------------------------------------------------------

void RoomLayout::claimNodes() {
    constexpr std::size_t kMaxTaps = 6;  // bounds the trunk's width on a big net

    std::vector<SymbolGeom> geoms;
    geoms.reserve(anchors.size());
    for (std::uint32_t a : anchors) geoms.push_back(cache[a]);

    for (std::size_t ni = 0; ni < d.nets.size(); ++ni) {
        std::int32_t net = static_cast<std::int32_t>(ni);
        const Net& n = d.nets[ni];
        const RenderNet& rn = pg.nets[ni];
        if (n.pins.size() < 3) continue;         // a pair is the chain idiom's
        if (rn.mark != NetMark::Label) continue;  // ground and rail have marks

        // The root is the anchor with the most pins on the net on ONE side, so
        // every leg runs straight out of the body without crossing it. Ties go
        // to the earlier anchor, which is designator order.
        std::size_t bestA = 0;
        std::vector<std::uint32_t> bestPins;
        for (std::size_t ai = 0; ai < anchors.size(); ++ai) {
            for (Side s : {Side::Left, Side::Right}) {
                std::vector<std::uint32_t> pins;
                for (const SymPin& p : geoms[ai].pins) {
                    if (p.side == s && p.net == net) pins.push_back(p.pin);
                }
                if (pins.size() > bestPins.size()) {
                    bestPins = std::move(pins);
                    bestA = ai;
                }
            }
        }
        if (bestPins.empty()) continue;

        // Taps: the room's unclaimed two-terminal parts with exactly one pin
        // on the net. A part with both pins on it is a short, not a tap. The
        // first part is claimed here; the string past it is walked below, so
        // nothing is consumed until the tap is certain to exist.
        std::vector<std::size_t> heads;
        for (std::size_t ci = 0; ci < cands.size() && heads.size() < kMaxTaps; ++ci) {
            const Cand& c = cands[ci];
            if (c.consumed) continue;
            const Component& cc = d.components[c.comp];
            if (cc.pins.size() != 2) continue;
            bool on0 = cc.pins[0].net == net;
            bool on1 = cc.pins[1].net == net;
            if (on0 == on1) continue;
            heads.push_back(ci);
        }
        if (bestPins.size() + heads.size() < 2) continue;  // nothing to join

        Node node;
        node.net = net;
        node.anchor = anchors[bestA];
        node.rootPins = std::move(bestPins);
        for (std::size_t ci : heads) {
            const Component& cc = d.components[cands[ci].comp];
            std::uint32_t nearPin = cc.pins[0].net == net ? 0u : 1u;
            NodeTap t;
            cands[ci].consumed = true;
            t.elems.push_back(RunElem{ci, nearPin});
            // The string continues exactly as a chain run does: on through
            // each private net into the next unclaimed part, stopping at a
            // ground, a rail, or anything another idiom already holds.
            std::uint32_t comp = cands[ci].comp;
            std::uint32_t exit = nearPin == 0 ? 1u : 0u;
            std::int32_t link = d.components[comp].pins[exit].net;
            while (!isGround(pg, link) && !isRail(pg, link) && isPrivate(link)) {
                PinRef other = otherEnd(link, comp, exit);
                Cand* nx = consumable(other);
                if (!nx) break;
                nx->consumed = true;
                t.elems.push_back(
                    RunElem{static_cast<std::size_t>(candOf[other.component]), other.pin});
                comp = other.component;
                exit = other.pin == 0 ? 1u : 0u;
                link = d.components[comp].pins[exit].net;
            }
            t.endNet = link;
            t.up = link >= 0 && markKindFor(pg, link) == MarkKind::RailFlag;
            node.taps.push_back(std::move(t));
        }

        // The trunk may drop its mark only when the drawing already holds
        // every pin of the net: a pin left with a label and a trunk with none
        // would show no connection at all.
        std::size_t drawn = node.rootPins.size() + node.taps.size();
        bool childPort = false;
        for (std::uint32_t bi : children) {
            for (const BlockPort& p : d.blocks[bi].ports) {
                if (p.net == net) childPort = true;
            }
        }
        node.terminal = drawn != n.pins.size() || rn.crossing || childPort ||
                        rn.direction != PortDir::None;
        nodes.push_back(std::move(node));
    }
}

RoomLayout::NodeMetrics RoomLayout::nodeMetrics(const Node& node) const {
    NodeMetrics mt;
    mt.pitch = 3 * P;
    for (const NodeTap& t : node.taps) {
        // Only the mark at the string's far end sits under the tap column; the
        // bodies above it are narrow, so the widest name is what sets the pitch.
        int w = 3 * P;
        if (t.endNet >= 0 && markKindFor(pg, t.endNet) != MarkKind::Ground &&
            markKindFor(pg, t.endNet) != MarkKind::NoConnect) {
            w = textW(netName(pg, t.endNet));
        }
        mt.pitch = std::max(mt.pitch, w + P);
    }
    mt.pitch = (mt.pitch + P - 1) / P * P;
    // The first tap clears the spine by half a label, so the leftmost far-end
    // name cannot reach back over the legs.
    mt.firstTap = std::max(2 * P, mt.pitch / 2 + P);
    int n = static_cast<int>(node.taps.size());
    mt.lastTap = n > 0 ? mt.firstTap + (n - 1) * mt.pitch : 2 * P;
    if (node.terminal) {
        mt.trunk = mt.lastTap + markTail(pg, node.net);
        mt.reserve = mt.trunk;
    } else {
        // No mark: the trunk stops dead on the last tap, which is then a
        // corner and takes no dot.
        mt.trunk = n > 0 ? mt.lastTap : 0;
        mt.reserve = mt.trunk + (n > 0 ? mt.pitch / 2 + 4 : 0);
    }
    return mt;
}

bool RoomLayout::placeNode(const Node& node, const PlacedSymbol& anchor, int ext, int stagger) {
    const SymbolGeom& g = anchor.geom;
    int px = 0;
    Side side = Side::Left;
    std::vector<int> ys;
    for (std::uint32_t cp : node.rootPins) {
        int gp = geomPinFor(g, cp);
        if (gp < 0) return false;
        int x = 0, y = 0;
        pinPos(anchor, static_cast<std::size_t>(gp), x, y, side);
        px = x;
        ys.push_back(y);
    }
    std::sort(ys.begin(), ys.end());
    const bool rightward = side != Side::Left;
    const int dir = rightward ? 1 : -1;
    const int spineX = px + dir * (ext + 6 + stagger);
    const int minY = ys.front(), maxY = ys.back();

    // Tap bodies, measured before a trunk row is chosen: the strip's height
    // needs the tallest string on each side of it. Each element of a string
    // walks one step further from the trunk, so its own entry pin faces back
    // towards it.
    struct ElemGeom {
        SymbolGeom geom;
        Rot rot = Rot::R0;
        int h = 0;
        std::uint32_t entryPin = 0;  // the pin facing the trunk
    };
    struct TapGeom {
        std::vector<ElemGeom> elems;
        std::int32_t endNet = -1;
        bool up = false;
        int reach = 0;  // trunk row to the far side of the end mark
    };
    std::vector<TapGeom> tg;
    int upH = 0, dnH = 0;
    for (const NodeTap& t : node.taps) {
        TapGeom e;
        e.up = t.up;
        e.endNet = t.endNet;
        e.reach = P + 16;  // the last stub and the end mark's text
        for (const RunElem& re : t.elems) {
            const Cand& c = cands[re.cand];
            ElemGeom eg;
            eg.entryPin = re.entryPin;
            eg.geom = cache[c.comp];
            // Every element is measured before anything is drawn, so a
            // geometry that does not expose the pin fails the node here --
            // where the fallback can still hand every claimed part back whole.
            if (geomPinFor(eg.geom, eg.entryPin) < 0) return false;
            // verticalRot puts the named pin on top: an element of a string
            // hanging below wants its own entry pin up, one standing above
            // wants its exit pin up so the entry still faces the trunk.
            std::uint32_t exitPin = eg.entryPin == 0 ? 1u : 0u;
            eg.rot = verticalRot(eg.geom, e.up ? exitPin : eg.entryPin);
            eg.h = rotatedH(eg.geom, eg.rot);
            e.reach += P + eg.h;  // the gap wire above it, then the body
            e.elems.push_back(std::move(eg));
        }
        if (e.up) upH = std::max(upH, e.reach);
        else dnH = std::max(dnH, e.reach);
        tg.push_back(std::move(e));
    }

    const NodeMetrics mt = nodeMetrics(node);
    const int nTaps = static_cast<int>(tg.size());

    // A trunk wider than the band every other idiom wraps at would push its
    // room past every neighbour and wreck the shelf packing. Long far-end net
    // names are what get one there; the node gives up and its pins keep the
    // labels they would have had.
    if (mt.reserve > kBandWrap) return false;

    // The legs and the spine are wires: they may cross other wires but never a
    // solid. Each leg is tested on its own pin's row -- the rows between two
    // root pins belong to whatever pins live there, and the spine clears their
    // marks by construction, standing further out than any mark on this side.
    // The trunk band holds bodies and marks, so it must clear both classes.
    std::vector<Rect> legs;
    for (int y : ys) {
        legs.push_back(rightward ? Rect{px + 4, y - 2, spineX, y + 2}
                                 : Rect{spineX, y - 2, px - 4, y + 2});
    }
    for (const Rect& r : legs) {
        if (buf.collides(r)) return false;
    }

    int stripY = maxY;
    bool placed = false;
    Rect band;
    for (int tries = 0; tries < 40; ++tries) {
        int a = spineX, b = spineX + dir * mt.reserve;
        band.x0 = std::min(a, b);
        band.x1 = std::max(a, b);
        band.y0 = upH > 0 ? stripY - upH - 4 : stripY - 6;
        band.y1 = dnH > 0 ? stripY + dnH + 4 : stripY + 6;
        Rect spine{spineX - 2, minY, spineX + 2, stripY};
        if (band.y0 >= 0 && !buf.collidesAny(band) && !buf.collides(spine)) {
            buf.reserve(band);
            buf.reserveWire(spine);
            for (const Rect& r : legs) buf.reserveWire(r);
            placed = true;
            break;
        }
        stripY += P;  // the node strip moves DOWN, never sideways
    }
    if (!placed) return false;

    std::vector<Seg> segs;
    for (int y : ys) {
        buf.wire({px, y, spineX, y}, node.net);
        segs.push_back(Seg{px, y, spineX, y});
    }
    if (minY != stripY) {
        buf.wire({spineX, minY, spineX, stripY}, node.net);
        segs.push_back(Seg{spineX, minY, spineX, stripY});
    }
    if (mt.trunk > 0) {
        int endX = spineX + dir * mt.trunk;
        buf.wire({spineX, stripY, endX, stripY}, node.net);
        segs.push_back(Seg{spineX, stripY, endX, stripY});
    }

    // Each tap walks away from the trunk one part at a time: a gap wire, a
    // body, then the next gap on whatever net that body exits onto, so a
    // string's inner nets are drawn conductors and never labels. Only the last
    // gap carries a mark.
    for (int k = 0; k < nTaps; ++k) {
        const TapGeom& e = tg[static_cast<std::size_t>(k)];
        const NodeTap& t = node.taps[static_cast<std::size_t>(k)];
        int tapX = spineX + dir * (mt.firstTap + k * mt.pitch);
        int sgn = e.up ? -1 : 1;

        int cursor = stripY;
        std::int32_t link = node.net;
        for (std::size_t i = 0; i < e.elems.size(); ++i) {
            const ElemGeom& eg = e.elems[i];
            buf.wire({tapX, cursor, tapX, cursor + sgn * P}, link);
            if (i == 0) segs.push_back(Seg{tapX, cursor, tapX, cursor + sgn * P});
            cursor += sgn * P;

            PlacedSymbol s;
            s.component = cands[t.elems[i].cand].comp;
            s.geom = eg.geom;
            s.rot = eg.rot;
            int gp = geomPinFor(s.geom, eg.entryPin);
            int lx = 0, ly = 0, rx = 0, ry = 0;
            localPin(s.geom, s.geom.pins[static_cast<std::size_t>(gp)], lx, ly);
            rotatePoint(s.geom, s.rot, lx, ly, rx, ry);
            // The entry pin lands on the cursor; ry is 0 hanging below and the
            // body's height standing above, which is the same expression.
            s.x = tapX - rx;
            s.y = cursor - ry;
            cursor += sgn * eg.h;
            std::uint32_t exitPin = eg.entryPin == 0 ? 1u : 0u;
            link = d.components[s.component].pins[exitPin].net;
            buf.symbols.push_back(std::move(s));
        }

        buf.wire({tapX, cursor, tapX, cursor + sgn * P}, e.endNet);
        if (e.endNet >= 0) {
            buf.mark(markKindFor(pg, e.endNet), tapX, cursor + sgn * P,
                     e.up ? Side::Top : Side::Bottom, e.endNet);
        }
    }

    if (node.terminal) {
        buf.mark(markKindFor(pg, node.net), spineX + dir * mt.trunk, stripY,
                 rightward ? Side::Right : Side::Left, node.net);
    }
    addJunctionDots(buf.dots, segs, node.net);
    return true;
}

void RoomLayout::walkForward(Run& run, std::int32_t net, std::uint32_t fromComp,
                             std::uint32_t fromPin) {
    while (true) {
        if (!isPrivate(net)) break;
        PinRef other = otherEnd(net, fromComp, fromPin);
        Cand* c = consumable(other);
        if (!c) break;
        c->consumed = true;
        run.elems.push_back(RunElem{static_cast<std::size_t>(candOf[other.component]),
                                    other.pin});
        std::uint32_t exit = other.pin == 0 ? 1 : 0;
        fromComp = other.component;
        fromPin = exit;
        net = d.components[other.component].pins[exit].net;
        if (isGround(pg, net) || isRail(pg, net)) break;
    }
    run.endNet = net;
}

void RoomLayout::walkChains() {
    // From anchor pins first: anchors in designator order, pins in declaration
    // order. Only Left/Right pins seed a run -- a chain extends horizontally.
    for (std::uint32_t a : anchors) {
        const Component& c = d.components[a];
        for (std::uint32_t p = 0; p < c.pins.size(); ++p) {
            std::int32_t net = c.pins[p].net;
            if (!isPrivate(net)) continue;
            PinRef other = otherEnd(net, a, p);
            if (!consumable(other)) continue;
            Run run;
            run.fromAnchor = true;
            run.anchor = a;
            run.anchorPin = p;
            run.startNet = net;
            walkForward(run, net, a, p);
            if (!run.elems.empty()) runs.push_back(std::move(run));
        }
    }

    // Then maximal paths from whatever chain candidates remain, walking back
    // to the string's head first so a mid-string start still yields one run.
    for (std::size_t ci = 0; ci < cands.size(); ++ci) {
        if (cands[ci].consumed || cands[ci].role != Role::Chain) continue;
        std::uint32_t head = cands[ci].comp;
        std::uint32_t headEntry = 0;
        std::vector<std::uint32_t> visited{head};
        while (true) {
            std::int32_t back = d.components[head].pins[headEntry].net;
            if (!isPrivate(back)) break;
            PinRef prev = otherEnd(back, head, headEntry);
            Cand* pc = consumable(prev);
            if (!pc || pc->role != Role::Chain) break;
            bool seen = false;
            for (std::uint32_t v : visited) {
                if (v == prev.component) seen = true;
            }
            if (seen) break;  // a ring: break it where the scan found it
            visited.push_back(prev.component);
            head = prev.component;
            headEntry = prev.pin == 0 ? 1 : 0;
        }
        Run run;
        run.startNet = d.components[head].pins[headEntry].net;
        Cand& hc = cands[static_cast<std::size_t>(candOf[head])];
        hc.consumed = true;
        run.elems.push_back(RunElem{static_cast<std::size_t>(candOf[head]), headEntry});
        std::uint32_t exit = headEntry == 0 ? 1 : 0;
        std::int32_t net = d.components[head].pins[exit].net;
        if (!isGround(pg, net) && !isRail(pg, net)) walkForward(run, net, head, exit);
        else run.endNet = net;
        runs.push_back(std::move(run));
    }
}

// ---------------------------------------------------------------------------
// Vertical cells: ladder caps, pull-ups, pull-downs, loose verticals.
// topNet gets a rail flag or label above; botNet a ground symbol or label
// below; -2 means no mark at that end (a ladder cap's bar tap).
// ---------------------------------------------------------------------------

int RoomLayout::verticalCellW(std::uint32_t comp, std::int32_t topNet,
                              std::int32_t botNet) const {
    int w = 3 * P;
    if (topNet >= 0) w = std::max(w, textW(netName(pg, topNet)) + 4);
    if (botNet >= 0) w = std::max(w, textW(netName(pg, botNet)) + 4);
    (void)comp;
    return w;
}

void RoomLayout::placeVertical(std::uint32_t comp, int cx, int topY, std::uint32_t topPin,
                               std::int32_t topNet, std::int32_t botNet) {
    PlacedSymbol s;
    s.component = comp;
    s.geom = cache[comp];
    s.rot = verticalRot(s.geom, topPin);
    int gp = geomPinFor(s.geom, topPin);
    int lx = 0, ly = 0, rx = 0, ry = 0;
    localPin(s.geom, s.geom.pins[static_cast<std::size_t>(gp)], lx, ly);
    rotatePoint(s.geom, s.rot, lx, ly, rx, ry);

    int attachY = topY + 16;             // the top mark's text/flag height
    int bodyTop = attachY + P;
    s.x = cx - rx;
    s.y = bodyTop;
    int bodyBot = bodyTop + rotatedH(s.geom, s.rot);

    if (topNet >= 0) {
        buf.wire({cx, attachY, cx, bodyTop}, topNet);
        buf.mark(markKindFor(pg, topNet), cx, attachY, Side::Top, topNet);
    }
    if (botNet >= 0) {
        buf.wire({cx, bodyBot, cx, bodyBot + P}, botNet);
        buf.mark(markKindFor(pg, botNet), cx, bodyBot + P, Side::Bottom, botNet);
    }
    buf.symbols.push_back(std::move(s));
}

// ---------------------------------------------------------------------------
// Chain runs. `px, py` is the point the run leaves from (an anchor pin, or a
// virtual entry beside the start mark); the strip walks `rightward` or not.
// `lead` is the straight lead-in from that point to the strip's jog column --
// at least a stub, or the anchor's whole mark extent so element bodies clear
// the labels of neighbouring pins.
// ---------------------------------------------------------------------------

// Length from the jog column to the end of the terminal mark.
int RoomLayout::innerLength(const Run& run) const {
    int len = P;
    for (const RunElem& e : run.elems) {
        const Cand& c = cands[e.cand];
        const SymbolGeom& g = cache[c.comp];
        len += g.w + P;
    }
    len -= P;  // the last gap is replaced by the terminal below
    return len + markTail(pg, run.endNet);
}

bool RoomLayout::placeRun(const Run& run, int px, int py, bool rightward, int midX) {
    const int dir = rightward ? 1 : -1;

    // Collect element geometry first: strip height needs the tallest body.
    // A diode's orientation follows the walk: the entry pin (anode toward the
    // source) faces the source, via R180 when the geometry has it wrong.
    std::vector<PlacedSymbol> elems;
    int up = 16, dn = 14;
    for (const RunElem& e : run.elems) {
        const Cand& c = cands[e.cand];
        PlacedSymbol s;
        s.component = c.comp;
        s.geom = cache[c.comp];
        int gp = geomPinFor(s.geom, e.entryPin);
        bool entryLeft = s.geom.pins[static_cast<std::size_t>(gp)].side == Side::Left;
        s.rot = (entryLeft == rightward) ? Rot::R0 : Rot::R180;
        int lx = 0, ly = 0, rx = 0, ry = 0;
        localPin(s.geom, s.geom.pins[static_cast<std::size_t>(gp)], lx, ly);
        rotatePoint(s.geom, s.rot, lx, ly, rx, ry);
        up = std::max(up, ry + 16);
        dn = std::max(dn, rotatedH(s.geom, s.rot) - ry + 18);
        elems.push_back(std::move(s));
    }

    int inner = innerLength(run);

    // The lead is a wire: it may cross other wires but never a solid. Its
    // rect starts just off the pin so the anchor's own body margin is not a
    // false collision.
    Rect lead = rightward ? Rect{px + 4, py - 2, midX, py + 2}
                          : Rect{midX, py - 2, px - 4, py + 2};
    if (buf.collides(lead)) return false;

    // The strip band holds the element bodies and the terminal: solid, so it
    // must clear both solids and wires. The jog column is a wire. The nominal
    // row is the pin's own; collisions push the strip DOWN, never sideways.
    int stripY = py;
    bool placed = false;
    for (int tries = 0; tries < 40; ++tries) {
        Rect band;
        int a = midX + dir * P, b = midX + dir * inner;
        band.x0 = std::min(a, b);
        band.x1 = std::max(a, b);
        band.y0 = stripY - up;
        band.y1 = stripY + dn;
        Rect jog{midX - 2, std::min(py, stripY), midX + 2, std::max(py, stripY)};
        if (!buf.collidesAny(band) && !buf.collides(jog)) {
            buf.reserve(band);
            buf.reserveWire(jog);
            buf.reserveWire(lead);
            placed = true;
            break;
        }
        stripY += P;
    }
    if (!placed) return false;

    // The Manhattan lead-in: straight when aligned, else one jog at midX.
    std::int32_t leadNet = run.startNet;
    if (stripY == py) buf.wire({px, py, midX, py}, leadNet);
    else buf.wire({px, py, midX, py, midX, stripY}, leadNet);

    int cursor = midX;
    std::int32_t net = run.startNet;
    for (PlacedSymbol& s : elems) {
        // The entry terminal is the one whose rotated side faces the source;
        // place the body so it lands P past the cursor.
        int entryIdx = -1;
        for (std::size_t i = 0; i < s.geom.pins.size(); ++i) {
            Side rs = rotatedSide(s.geom.pins[i].side, s.rot);
            if ((rightward && rs == Side::Left) || (!rightward && rs == Side::Right)) {
                entryIdx = static_cast<int>(i);
            }
        }
        int lx = 0, ly = 0, ex = 0, ey = 0;
        localPin(s.geom, s.geom.pins[static_cast<std::size_t>(entryIdx)], lx, ly);
        rotatePoint(s.geom, s.rot, lx, ly, ex, ey);
        int entryX = cursor + dir * P;
        s.x = entryX - ex;
        s.y = stripY - ey;
        buf.wire({cursor, stripY, entryX, stripY}, net);
        int exitIdx = entryIdx == 0 ? 1 : 0;
        int ox = 0, oy = 0;
        localPin(s.geom, s.geom.pins[static_cast<std::size_t>(exitIdx)], lx, ly);
        rotatePoint(s.geom, s.rot, lx, ly, ox, oy);
        cursor = s.x + ox;
        net = d.components[s.component].pins[s.geom.pins[static_cast<std::size_t>(exitIdx)].pin]
                  .net;
        buf.symbols.push_back(std::move(s));
    }

    // The end terminal.
    std::int32_t end = run.endNet;
    if (end < 0) {
        buf.wire({cursor, stripY, cursor + dir * P, stripY}, -1);
        return true;
    }
    switch (markKindFor(pg, end)) {
        case MarkKind::Ground:
            buf.wire({cursor, stripY, cursor + dir * P, stripY, cursor + dir * P, stripY + P},
                     end);
            buf.mark(MarkKind::Ground, cursor + dir * P, stripY + P, Side::Bottom, end);
            break;
        case MarkKind::RailFlag:
            buf.wire({cursor, stripY, cursor + dir * P, stripY, cursor + dir * P, stripY - P},
                     end);
            buf.mark(MarkKind::RailFlag, cursor + dir * P, stripY - P, Side::Top, end);
            break;
        case MarkKind::Label:
        case MarkKind::NoConnect:
        case MarkKind::PortFlag:
            buf.wire({cursor, stripY, cursor + dir * P, stripY}, end);
            buf.mark(markKindFor(pg, end), cursor + dir * P, stripY,
                     rightward ? Side::Right : Side::Left, end);
            break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Band 0: rail bars with decoupling ladders, then loose verticals.
// ---------------------------------------------------------------------------

int RoomLayout::placeBand0(int y) {
    // Ladder caps grouped by rail, in room candidate order.
    std::vector<std::int32_t> railOrder;
    for (const Cand& c : cands) {
        if (c.role != Role::Ladder || c.consumed) continue;
        bool seen = false;
        for (std::int32_t r : railOrder) {
            if (r == c.railNet) seen = true;
        }
        if (!seen) railOrder.push_back(c.railNet);
    }

    for (std::int32_t rail : railOrder) {
        std::vector<Cand*> caps;
        for (Cand& c : cands) {
            if (c.role == Role::Ladder && !c.consumed && c.railNet == rail) caps.push_back(&c);
        }
        if (caps.size() < 2) continue;  // a single cap is a loose pull-up below

        int barY = y + 12;  // room for the rail name above the bar's left end
        int firstTap = 2 * P;
        int n = static_cast<int>(caps.size());
        int barLen = firstTap + (n - 1) * 3 * P + 2 * P;
        RailBarItem bar;
        bar.x1 = 0;
        bar.x2 = barLen;
        bar.y = barY;
        bar.net = rail;
        buf.bars.push_back(bar);

        int bottom = barY;
        for (int i = 0; i < n; ++i) {
            Cand& c = *caps[static_cast<std::size_t>(i)];
            c.consumed = true;
            int tapX = firstTap + i * 3 * P;
            buf.dots.push_back(DotItem{tapX, barY, rail});
            buf.wire({tapX, barY, tapX, barY + P}, rail);
            // body hangs from the tap; the far side drops to ground
            std::int32_t gnd = d.components[c.comp].pins[c.railPin == 0 ? 1 : 0].net;
            PlacedSymbol s;
            s.component = c.comp;
            s.geom = cache[c.comp];
            s.rot = verticalRot(s.geom, c.railPin);
            int gp = geomPinFor(s.geom, c.railPin);
            int lx = 0, ly = 0, rx = 0, ry = 0;
            localPin(s.geom, s.geom.pins[static_cast<std::size_t>(gp)], lx, ly);
            rotatePoint(s.geom, s.rot, lx, ly, rx, ry);
            s.x = tapX - rx;
            s.y = barY + P;
            int bodyBot = s.y + rotatedH(s.geom, s.rot);
            buf.wire({tapX, bodyBot, tapX, bodyBot + P}, gnd);
            buf.mark(MarkKind::Ground, tapX, bodyBot + P, Side::Bottom, gnd);
            buf.symbols.push_back(std::move(s));
            bottom = std::max(bottom, bodyBot + P + 10);
        }
        buf.reserve(Rect{0, y, barLen + 30, bottom + 4});  // +30: refdes text room
        y = bottom + 2 * P;
    }

    // Loose verticals: single-cap ladders (rail on top, ground below) and any
    // pull-up or pull-down no anchor claims. Placed in one row; collisions
    // move them right.
    struct Loose {
        Cand* c;
        std::int32_t topNet, botNet;
        std::uint32_t topPin;
    };
    std::vector<Loose> loose;
    for (Cand& c : cands) {
        if (c.consumed) continue;
        if (c.role == Role::Ladder) {
            std::int32_t gnd = d.components[c.comp].pins[c.railPin == 0 ? 1 : 0].net;
            loose.push_back(Loose{&c, c.railNet, gnd, c.railPin});
        } else if (c.role == Role::PullUp || c.role == Role::PullDown) {
            // claimed later by an anchor; only truly unclaimed ones land here
            bool claimed = false;
            for (std::uint32_t a : anchors) {
                for (const ComponentPin& p : d.components[a].pins) {
                    if (p.net == c.sigNet && c.sigNet >= 0) claimed = true;
                }
            }
            if (claimed) continue;
            if (c.role == Role::PullUp) {
                loose.push_back(Loose{&c, c.railNet, c.sigNet, c.railPin});
            } else {
                std::int32_t gnd = d.components[c.comp].pins[c.railPin].net;
                loose.push_back(Loose{&c, c.sigNet, gnd, c.sigPin});
            }
        }
    }
    if (!loose.empty()) {
        int x = 2 * P;
        for (Loose& l : loose) {
            l.c->consumed = true;
            int cw = verticalCellW(l.c->comp, l.topNet, l.botNet);
            int cx = x + cw / 2;
            Rect cell{cx - cw / 2, y, cx + cw / 2 + 24, y + kVertLen + 8};
            while (buf.collidesAny(cell)) {
                cx += P;
                cell.x0 += P;
                cell.x1 += P;
            }
            buf.reserve(cell);
            placeVertical(l.c->comp, cx, y, l.topPin, l.topNet, l.botNet);
            x = cell.x1 + P;
        }
        y += kVertLen + 2 * P;
    }
    return y;
}

// ---------------------------------------------------------------------------
// Band 1: anchors with their pull-ups, pull-downs and chains.
// ---------------------------------------------------------------------------

int RoomLayout::placeBand1(int y) {
    if (anchors.empty()) return y;
    int rowTop = y;
    int cursorX = 0;

    for (std::uint32_t a : anchors) {
        const Component& c = d.components[a];
        const SymbolGeom& g = cache[a];

        // Idioms attached to this anchor: the first anchor whose pins touch a
        // pull's signal net claims it.
        std::vector<Cand*> ups, downs;
        for (Cand& cd : cands) {
            if (cd.consumed || cd.sigNet < 0) continue;
            if (cd.role != Role::PullUp && cd.role != Role::PullDown) continue;
            bool mine = false;
            for (const ComponentPin& p : c.pins) {
                if (p.net == cd.sigNet) mine = true;
            }
            if (!mine) continue;
            (cd.role == Role::PullUp ? ups : downs).push_back(&cd);
            cd.consumed = true;
        }
        std::vector<Run*> myRuns;
        std::vector<bool> pinConsumed(c.pins.size(), false);
        for (Run& r : runs) {
            if (r.fromAnchor && r.anchor == a) {
                myRuns.push_back(&r);
                pinConsumed[r.anchorPin] = true;
            }
        }
        std::vector<Node*> myNodes;
        for (Node& nd : nodes) {
            if (nd.anchor != a) continue;
            myNodes.push_back(&nd);
            for (std::uint32_t rp : nd.rootPins) pinConsumed[rp] = true;
        }

        // Extents of the unconsumed pins' stubs and marks, and of the chains.
        int extL = 0, extR = 0, extT = 14, extB = 14;
        for (const SymPin& p : g.pins) {
            if (pinConsumed[p.pin]) continue;
            int e = markExtent(pg, p.net, p.side);
            switch (p.side) {
                case Side::Left: extL = std::max(extL, e); break;
                case Side::Right: extR = std::max(extR, e); break;
                case Side::Top: extT = std::max(extT, e); break;
                case Side::Bottom: extB = std::max(extB, e); break;
            }
        }
        extL = std::max(extL, kStubLen + 4);
        extR = std::max(extR, kStubLen + 4);

        // Chains and node trunks fan out: the structure on the topmost pin
        // jogs FURTHEST from the body, later ones jog sooner, so a lower
        // structure's drop column crosses only wires -- never an earlier one's
        // element bodies. Runs are staggered before nodes on either side, and
        // the placement loop below walks the two in the same order so the
        // stagger it uses is the one this reserved.
        int nLeft = 0, nRight = 0;
        for (const Run* r : myRuns) {
            int gp = geomPinFor(g, r->anchorPin);
            if (gp < 0) continue;
            (g.pins[static_cast<std::size_t>(gp)].side == Side::Left ? nLeft : nRight) += 1;
        }
        for (const Node* nd : myNodes) {
            int gp = geomPinFor(g, nd->rootPins[0]);
            if (gp < 0) continue;
            (g.pins[static_cast<std::size_t>(gp)].side == Side::Left ? nLeft : nRight) += 1;
        }
        int chainL = 0, chainR = 0;
        {
            int kL = 0, kR = 0;
            for (const Run* r : myRuns) {
                int gp = geomPinFor(g, r->anchorPin);
                if (gp < 0) continue;
                if (g.pins[static_cast<std::size_t>(gp)].side == Side::Left) {
                    int stagger = (nLeft - 1 - kL++) * 2 * P;
                    chainL = std::max(chainL, extL + 6 + stagger + innerLength(*r));
                } else {
                    int stagger = (nRight - 1 - kR++) * 2 * P;
                    chainR = std::max(chainR, extR + 6 + stagger + innerLength(*r));
                }
            }
            for (const Node* nd : myNodes) {
                int gp = geomPinFor(g, nd->rootPins[0]);
                if (gp < 0) continue;
                int reserve = nodeMetrics(*nd).reserve;
                if (g.pins[static_cast<std::size_t>(gp)].side == Side::Left) {
                    int stagger = (nLeft - 1 - kL++) * 2 * P;
                    chainL = std::max(chainL, extL + 6 + stagger + reserve);
                } else {
                    int stagger = (nRight - 1 - kR++) * 2 * P;
                    chainR = std::max(chainR, extR + 6 + stagger + reserve);
                }
            }
        }

        int pullZone = ups.empty() ? 0 : kVertLen + P;
        // Wrap to a second anchor row before overflowing the band.
        if (cursorX > 0 &&
            cursorX + std::max(extL, chainL) + g.w + std::max(extR, chainR) > kBandWrap) {
            rowTop = buf.maxY + 3 * P;
            cursorX = 0;
        }

        int bodyX = cursorX + std::max(extL, chainL);
        int bodyY = rowTop + pullZone + extT;
        Rect body{bodyX - 2, bodyY - extT, bodyX + g.w + 2, bodyY + g.h + extB};
        while (buf.collidesAny(body)) {
            bodyY += P;
            body.y0 += P;
            body.y1 += P;
        }
        buf.reserve(body);

        PlacedSymbol placed;
        placed.component = a;
        placed.geom = g;
        placed.x = bodyX;
        placed.y = bodyY;

        // Stubs and marks for every pin no idiom consumed, each reserving its
        // own strip so a chain can thread between them but never over them.
        for (std::size_t gp = 0; gp < g.pins.size(); ++gp) {
            const SymPin& p = g.pins[gp];
            if (pinConsumed[p.pin]) continue;
            int px = 0, py = 0;
            Side side = Side::Left;
            pinPos(placed, gp, px, py, side);
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

        // Pull-ups stand over the anchor, rail flag up, signal joining BY
        // LABEL below -- never a wire across the body.
        int px2 = bodyX;
        for (Cand* u : ups) {
            int cw = verticalCellW(u->comp, u->railNet, u->sigNet);
            int cx = px2 + cw / 2;
            Rect cell{cx - cw / 2, rowTop, cx + cw / 2 + 24, rowTop + kVertLen};
            while (buf.collidesAny(cell)) {
                cx += P;
                cell.x0 += P;
                cell.x1 += P;
            }
            buf.reserve(cell);
            placeVertical(u->comp, cx, rowTop, u->railPin, u->railNet, u->sigNet);
            px2 = cell.x1 + P;
        }

        // Pull-downs hang below, ground symbol under, signal labelled above.
        int dx2 = bodyX;
        int downTop = bodyY + g.h + extB + P;
        for (Cand* dn : downs) {
            std::int32_t gnd = d.components[dn->comp].pins[dn->railPin].net;
            int cw = verticalCellW(dn->comp, dn->sigNet, gnd);
            int cx = dx2 + cw / 2;
            Rect cell{cx - cw / 2, downTop, cx + cw / 2 + 24, downTop + kVertLen};
            while (buf.collidesAny(cell)) {
                cx += P;
                cell.x0 += P;
                cell.x1 += P;
            }
            buf.reserve(cell);
            placeVertical(dn->comp, cx, downTop, dn->sigPin, dn->sigNet, gnd);
            dx2 = cell.x1 + P;
        }

        // Chains extend horizontally from the pin they start at, leading past
        // the anchor's own mark zone before the first element. A run that
        // cannot be drawn downgrades: the pin keeps a plain mark and the run
        // floats to the free grid, where its labels connect it by name.
        {
            int kL = 0, kR = 0;
            for (Run* r : myRuns) {
                int gp = geomPinFor(g, r->anchorPin);
                if (gp < 0) continue;
                int px = 0, py = 0;
                Side side = Side::Left;
                pinPos(placed, static_cast<std::size_t>(gp), px, py, side);
                bool rightward = side != Side::Left;
                int stagger = rightward ? (nRight - 1 - kR++) * 2 * P
                                        : (nLeft - 1 - kL++) * 2 * P;
                // +6: the jog column must clear the mark zone it drops past.
                int ext = rightward ? extR : extL;
                int midX = px + (rightward ? 1 : -1) * (ext + 6 + stagger);
                if (!placeRun(*r, px, py, rightward, midX)) {
                    r->downgraded = true;
                    int sx = px + (rightward ? kStubLen : -kStubLen);
                    int e = markExtent(pg, r->startNet, side);
                    buf.wire({px, py, sx, py}, r->startNet);
                    if (r->startNet >= 0) {
                        buf.mark(markKindFor(pg, r->startNet), sx, py, side, r->startNet);
                    }
                    buf.reserve(rightward ? Rect{px, py - 6, px + e, py + 6}
                                          : Rect{px - e, py - 6, px, py + 6});
                }
            }

            // Node trunks leave the same way, one row of taps per net. A node
            // that will not fit gives every part back: the root pins take a
            // plain mark and the taps fall to the free grid as single cells,
            // where their labels connect the net by name exactly as before.
            for (Node* nd : myNodes) {
                int gp = geomPinFor(g, nd->rootPins[0]);
                if (gp < 0) continue;
                bool rightward = g.pins[static_cast<std::size_t>(gp)].side != Side::Left;
                int stagger = rightward ? (nRight - 1 - kR++) * 2 * P
                                        : (nLeft - 1 - kL++) * 2 * P;
                if (placeNode(*nd, placed, rightward ? extR : extL, stagger)) continue;
                for (std::uint32_t rp : nd->rootPins) {
                    int rgp = geomPinFor(g, rp);
                    if (rgp < 0) continue;
                    int px = 0, py = 0;
                    Side side = Side::Left;
                    pinPos(placed, static_cast<std::size_t>(rgp), px, py, side);
                    int sx = px + (side == Side::Left ? -kStubLen : kStubLen);
                    int e = markExtent(pg, nd->net, side);
                    buf.wire({px, py, sx, py}, nd->net);
                    buf.mark(markKindFor(pg, nd->net), sx, py, side, nd->net);
                    buf.reserve(side == Side::Left ? Rect{px - e, py - 6, px, py + 6}
                                                  : Rect{px, py - 6, px + e, py + 6});
                }
                // Every part of every string goes back, not just the one that
                // touched the trunk: a string the node walked is claimed whole.
                for (const NodeTap& t : nd->taps) {
                    for (const RunElem& re : t.elems) orphans.push_back(cands[re.cand].comp);
                }
            }
        }

        buf.symbols.push_back(std::move(placed));
        cursorX = std::max({bodyX + g.w + std::max(extR, chainR), px2, dx2}) + 4 * P;
    }
    return buf.maxY + 2 * P;
}

// ---------------------------------------------------------------------------
// Child sheet symbols: one green box per direct child block instance, its
// ports pins on the LEFT edge in declaration order, each with the ordinary
// stub and mark. The box is an anchor-class solid; the marks are what wire it
// into the page, connecting by name like any other stub.
// ---------------------------------------------------------------------------

int RoomLayout::placeChildren(int y) {
    if (children.empty()) return y;
    int x = 0, rowH = 0;
    for (std::uint32_t bi : children) {
        const BlockInstance& b = d.blocks[bi];
        int n = static_cast<int>(b.ports.size());
        int w = std::max(textW(b.block) + 16, 6 * P);
        for (const BlockPort& p : b.ports) w = std::max(w, 16 + textW(p.name) + 8);
        int h = sheetSymBodyH(n);

        int extL = kStubLen + 4;
        for (const BlockPort& p : b.ports) {
            if (p.net >= 0) extL = std::max(extL, markExtent(pg, p.net, Side::Left));
        }
        const int topPad = 14;  // the instance designator above the box
        int cellW = extL + w + 2 * P;
        int cellH = topPad + h + P;
        if (x > 0 && x + cellW > kBandWrap) {
            x = 0;
            y = buf.maxY + P;
            rowH = 0;
        }
        Rect cell{x, y, x + cellW, y + cellH};
        while (buf.collidesAny(cell)) {
            cell.y0 += P;
            cell.y1 += P;
        }
        buf.reserve(cell);

        SheetSymItem item;
        item.x = x + extL;
        item.y = cell.y0 + topPad;
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
        x += cellW + 2 * P;
        rowH = std::max(rowH, cellH);
    }
    return buf.maxY + 2 * P;
}

// ---------------------------------------------------------------------------
// Band 2: floating runs, then the leftover free grid.
// ---------------------------------------------------------------------------

// One free-grid cell: the symbol with a stub and a mark on every pin.
void RoomLayout::placeLeftoverCell(std::uint32_t comp, int& x, int& y, int& rowH) {
    PlacedSymbol s;
    s.component = comp;
    s.geom = cache[comp];

    int extL = 0, extR = 0, extT = 14, extB = 16;
    for (const SymPin& p : s.geom.pins) {
        int e = markExtent(pg, p.net, p.side);
        switch (p.side) {
            case Side::Left: extL = std::max(extL, e); break;
            case Side::Right: extR = std::max(extR, e); break;
            case Side::Top: extT = std::max(extT, e); break;
            case Side::Bottom: extB = std::max(extB, e); break;
        }
    }
    int cellW = extL + s.geom.w + extR;
    int cellH = extT + s.geom.h + extB;
    if (x > 0 && x + cellW > kBandWrap) {
        x = 0;
        y += rowH + P;
        rowH = 0;
    }
    Rect cell{x, y, x + cellW, y + cellH};
    while (buf.collidesAny(cell)) {
        cell.y0 += P;
        cell.y1 += P;
    }
    buf.reserve(cell);
    s.x = x + extL;
    s.y = cell.y0 + extT;
    buf.symbols.push_back(std::move(s));
    const PlacedSymbol& ps = buf.symbols.back();
    for (std::size_t gp = 0; gp < ps.geom.pins.size(); ++gp) {
        const SymPin& p = ps.geom.pins[gp];
        int px = 0, py = 0;
        Side side = Side::Left;
        pinPos(ps, gp, px, py, side);
        int sx = px, sy = py;
        switch (side) {
            case Side::Left: sx -= kStubLen; break;
            case Side::Right: sx += kStubLen; break;
            case Side::Top: sy -= kStubLen; break;
            case Side::Bottom: sy += kStubLen; break;
        }
        buf.wire({px, py, sx, sy}, p.net);
        if (p.nc || p.net >= 0) buf.mark(markKindForPin(pg, p), sx, sy, side, p.net);
    }
    x += cellW + 2 * P;
    rowH = std::max(rowH, cellH);
}

void RoomLayout::placeBand2(int y) {
    int x = 0;
    int rowH = 0;

    // Floating runs first: entry mark, elements, end mark, one strip each.
    // Runs an anchor had to give up on land here too, connected by name.
    for (const Run& r : runs) {
        if (r.fromAnchor && !r.downgraded) continue;
        int entryExt = r.startNet >= 0 ? markExtent(pg, r.startNet, Side::Left) : P;
        int cellW = entryExt + P + innerLength(r) + P;
        int cellH = 6 * P;
        if (x > 0 && x + cellW > kBandWrap) {
            x = 0;
            y += rowH + P;
            rowH = 0;
        }
        int py = y + 3 * P;
        int px = x + entryExt;
        if (placeRun(r, px, py, true, px + P)) {
            if (r.startNet >= 0) {
                buf.mark(markKindFor(pg, r.startNet), px, py, Side::Left, r.startNet);
            }
            x += cellW + 2 * P;
            rowH = std::max(rowH, cellH);
        } else {
            // Even the free grid was crowded here: the elements fall back to
            // single cells whose labels still connect everything by name.
            for (const RunElem& e : r.elems) {
                placeLeftoverCell(cands[e.cand].comp, x, y, rowH);
            }
        }
    }
    if (rowH > 0) {
        y = buf.maxY + P;
        x = 0;
        rowH = 0;
    }

    // The free grid: whatever no idiom placed, labelled on every pin. The taps
    // of a node that could not be drawn land here, so a claimed part is never
    // left off the sheet.
    for (std::uint32_t idx : orphans) placeLeftoverCell(idx, x, y, rowH);
    for (std::uint32_t idx : leftovers) placeLeftoverCell(idx, x, y, rowH);
}

void RoomLayout::placeAll() {
    int y = placeBand0(0);
    y = placeBand1(y);
    y = placeChildren(y);
    placeBand2(y);
}

// ---------------------------------------------------------------------------
// Sheet assembly.
// ---------------------------------------------------------------------------

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

}  // namespace

SheetLayout layoutPageClassic(const RenderModel& model, const RenderPage& page,
                              const SymbolCache& cache) {
    SheetLayout sheet;
    sheet.note = page.note;

    // Lay every room out in its own coordinates first.
    std::vector<RoomBuf> bufs;
    std::vector<int> roomW, roomH, stripH;
    bufs.reserve(page.rooms.size());
    for (const RenderRoom& room : page.rooms) {
        RoomLayout rl(model, page, cache);
        rl.classify(room);
        rl.claimNodes();
        rl.walkChains();
        rl.placeAll();
        int strip = room.framed && !room.title.empty() ? kTitleStrip : 0;
        roomW.push_back(rl.buf.maxX + 2 * kRoomPad);
        roomH.push_back(rl.buf.maxY + 2 * kRoomPad + strip);
        stripH.push_back(strip);
        bufs.push_back(std::move(rl.buf));
    }

    // Shelf-pack the rooms: targetW = ceil(sqrt(1.45 * total area)), clamped
    // to [widest room, 1600].
    std::int64_t area = 0;
    int widest = 0;
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        area += static_cast<std::int64_t>(roomW[i]) * roomH[i];
        widest = std::max(widest, roomW[i]);
    }
    int targetW = static_cast<int>(isqrtCeil(area * 145 / 100));
    targetW = std::clamp(targetW, widest, std::max(widest, 1600));

    int cx = 0, cy = 0, shelfH = 0, packedW = 0;
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        if (cx > 0 && cx + roomW[i] > targetW) {
            cx = 0;
            cy += shelfH;
            shelfH = 0;
        }
        int rx = kSheetMargin + cx;
        int ry = kSheetMargin + cy;
        const RenderRoom& room = page.rooms[i];
        RoomItem ri;
        ri.x = rx;
        ri.y = ry;
        ri.w = roomW[i];
        ri.h = roomH[i];
        ri.title = upperCopy(room.title);
        ri.framed = room.framed;
        sheet.rooms.push_back(std::move(ri));

        translate(bufs[i], rx + kRoomPad, ry + stripH[i] + kRoomPad);
        auto& b = bufs[i];
        sheet.symbols.insert(sheet.symbols.end(), b.symbols.begin(), b.symbols.end());
        sheet.children.insert(sheet.children.end(), b.children.begin(), b.children.end());
        sheet.wires.insert(sheet.wires.end(), b.wires.begin(), b.wires.end());
        sheet.dots.insert(sheet.dots.end(), b.dots.begin(), b.dots.end());
        sheet.marks.insert(sheet.marks.end(), b.marks.begin(), b.marks.end());
        sheet.bars.insert(sheet.bars.end(), b.bars.begin(), b.bars.end());

        cx += roomW[i];
        shelfH = std::max(shelfH, roomH[i]);
        packedW = std::max(packedW, cx);
    }
    int packedH = cy + shelfH;

    sheet.w = std::max(packedW + 2 * kSheetMargin, 680);
    sheet.h = packedH + 2 * kSheetMargin + kTitleBlockH;

    return sheet;
}

}  // namespace manta::render
