// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Cluster planning: arteries walked from anchor pins, junctions with shunt
// strings, decap rows, satellites, and the net classification the router
// reads. Re-expresses the classic placer's chain/node mechanics as a pure
// plan -- claiming order is the law here, geometry is someone else's.
//
// Determinism (spec 15.8): every container is a vector iterated in insertion
// or index order, and every choice's tie is broken by a named key -- slot
// then pin index for seeding, net-pin order for junction heads, string
// length then head designator (naturalLess) then component index for the
// continuation, room-component order for every claiming sweep.
#include "render/cluster.h"

#include <algorithm>
#include <cassert>
#include <cstddef>

#include "render/symbols.h"

namespace manta::render {

namespace {

// A junction hangs at most this many strings; excess parts stay unclaimed,
// which leaves their pins uncovered and forces the net Named -- the same
// bound (and the same fallback) as the classic node's kMaxTaps.
constexpr std::size_t kMaxShunts = 6;

// The role gates, copied from flow.cpp's classification (flow does not
// export roles; a later package may deduplicate).
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

enum class Role : std::uint8_t { Anchor, Ladder, PullUp, PullDown, Chain, Loose };

// The whole plan's working state. Claiming is first-wins over one `claimed`
// array; `consumed` (the published subset) marks only the parts drawn INSIDE
// something -- an artery step, a shunt, a decap row, a satellite -- while
// looseVerts and freeRuns entries are claimed but keep their own cells.
struct Planner {
    const RenderPage& pg;
    const RenderRoom& room;
    const RoomFlow& flow;
    const std::vector<SidePlan>& plans;
    const RenderModel& m;
    const Design& d;

    RoomPlan plan;
    std::vector<Role> role;
    std::vector<std::int32_t> sigNet, railNet;
    std::vector<std::uint8_t> inRoom;
    std::vector<char> claimed;
    std::vector<std::uint32_t> flowAnchors;   // anchor components, (rank, order) order
    std::vector<std::int32_t> clusterOfVert;  // flow vertex -> cluster index, -1
    std::size_t freeCluster = 0;

    Planner(const RenderPage& page, const RenderRoom& r, const RoomFlow& f,
            const std::vector<SidePlan>& sp, const RenderModel& model)
        : pg(page), room(r), flow(f), plans(sp), m(model), d(*model.design) {}

    // ------------------------------------------------------------------
    // Net predicates, shared with flow.cpp's semantics.
    // ------------------------------------------------------------------
    [[nodiscard]] bool isGnd(std::int32_t net) const {
        return net >= 0 && pg.nets[static_cast<std::size_t>(net)].mark == NetMark::Ground;
    }
    [[nodiscard]] bool isRailN(std::int32_t net) const {
        return net >= 0 && pg.nets[static_cast<std::size_t>(net)].mark == NetMark::Rail;
    }
    // A private net can carry a drawn series link: exactly two pins, plain
    // label, not crossing, not a port -- classic isPrivate, verbatim.
    [[nodiscard]] bool isPrivate(std::int32_t net) const {
        if (net < 0) return false;
        const Net& n = d.nets[static_cast<std::size_t>(net)];
        const RenderNet& rn = pg.nets[static_cast<std::size_t>(net)];
        return n.pins.size() == 2 && rn.mark == NetMark::Label && !rn.crossing &&
               rn.direction == PortDir::None;
    }
    [[nodiscard]] PinRef otherEnd(std::int32_t net, std::uint32_t comp, std::uint32_t pin) const {
        for (const PinRef& pr : d.nets[static_cast<std::size_t>(net)].pins) {
            if (pr.component != comp || pr.pin != pin) return pr;
        }
        return PinRef{comp, pin};
    }

    void claim(std::uint32_t comp) { claimed[comp] = 1; }
    void consume(std::uint32_t comp) {
        claimed[comp] = 1;
        plan.consumed[comp] = 1;
    }

    // What an artery may walk through: the classic consumable gate.
    [[nodiscard]] bool consumable(std::uint32_t comp) const {
        if (!inRoom[comp] || claimed[comp]) return false;
        return role[comp] == Role::Chain || role[comp] == Role::PullDown;
    }
    // What may head a shunt string: any unclaimed two-terminal part. A
    // PullUp heads legitimately (a buck inductor onto its rail); a Ladder
    // never appears -- both its nets carry marks, not labels.
    [[nodiscard]] bool headEligible(std::uint32_t comp) const {
        if (!inRoom[comp] || claimed[comp]) return false;
        Role r = role[comp];
        return r == Role::Chain || r == Role::PullUp || r == Role::PullDown;
    }

    [[nodiscard]] bool compTouches(std::uint32_t comp, std::int32_t net) const {
        for (const ComponentPin& p : d.components[comp].pins) {
            if (p.net == net) return true;
        }
        return false;
    }
    [[nodiscard]] std::size_t clusterIndexOf(std::uint32_t comp) const {
        std::int32_t v = flow.vertexOf[comp];
        std::int32_t ci = v >= 0 ? clusterOfVert[static_cast<std::size_t>(v)] : -1;
        return ci >= 0 ? static_cast<std::size_t>(ci) : freeCluster;
    }
    [[nodiscard]] const std::string& keyOf(std::uint32_t comp) const {
        const Component& c = d.components[comp];
        return c.designator.empty() ? c.identity : c.designator;
    }

    void classify();
    void makeClusters();
    void anchorPinSides(std::uint32_t comp, std::vector<std::uint32_t>& left,
                        std::vector<std::uint32_t>& right) const;
    void walkArteries();
    void walkPin(std::uint32_t anchor, std::uint32_t pin, std::vector<Artery>& out);
    [[nodiscard]] bool junctionNet(std::int32_t net, std::uint32_t anchor) const;
    [[nodiscard]] bool bridgesAnchorNet(std::uint32_t comp, std::uint32_t farPin,
                                        std::uint32_t anchor) const;
    [[nodiscard]] bool buildJunction(Artery& art, std::int32_t net, std::uint32_t anchor,
                                     std::uint32_t fromComp, std::uint32_t fromPin);
    void walkString(Shunt& s, std::uint32_t comp, std::uint32_t exitPin);
    void claimDecaps();
    void claimPulls();
    void claimLoose();
    void claimFreeRuns();
};

// Classification in room component order, flow.cpp:110-143 semantics.
void Planner::classify() {
    const std::size_t nc = d.components.size();
    role.assign(nc, Role::Loose);
    sigNet.assign(nc, -1);
    railNet.assign(nc, -1);
    inRoom.assign(nc, 0);
    claimed.assign(nc, 0);
    plan.consumed.assign(nc, 0);
    plan.netState.assign(pg.nets.size(), NetState::Free);

    for (std::uint32_t idx : room.components) {
        inRoom[idx] = 1;
        const Component& c = d.components[idx];
        SymbolKind k = m.kinds[idx];
        if (anchorKind(k, c)) {
            role[idx] = Role::Anchor;
            continue;
        }
        if (!twoTerminalKind(k) || c.pins.size() != 2) {
            role[idx] = Role::Loose;
            continue;
        }
        std::int32_t a = c.pins[0].net;
        std::int32_t b = c.pins[1].net;
        bool railA = isRailN(a), railB = isRailN(b);
        bool gndA = isGnd(a), gndB = isGnd(b);
        if ((railA && gndB) || (railB && gndA)) {
            role[idx] = Role::Ladder;
            railNet[idx] = railA ? a : b;
        } else if (railA != railB && !gndA && !gndB) {
            role[idx] = Role::PullUp;
            railNet[idx] = railA ? a : b;
            sigNet[idx] = railA ? b : a;
        } else if (gndA != gndB && !railA && !railB) {
            role[idx] = Role::PullDown;
            sigNet[idx] = gndA ? b : a;
        } else {
            role[idx] = Role::Chain;
        }
    }
}

// One cluster per Anchor and Child vertex in (rank, order) packing order --
// the pair is unique per vertex, vertex index named as the formal tie -- then
// the free cluster, last, at one rank past the room.
void Planner::makeClusters() {
    std::vector<std::uint32_t> vs;
    for (std::uint32_t vi = 0; vi < flow.verts.size(); ++vi) {
        FlowVertex::Kind k = flow.verts[vi].kind;
        if (k == FlowVertex::Kind::Anchor || k == FlowVertex::Kind::Child) vs.push_back(vi);
    }
    std::sort(vs.begin(), vs.end(), [&](std::uint32_t x, std::uint32_t y) {
        const FlowVertex& a = flow.verts[x];
        const FlowVertex& b = flow.verts[y];
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.order != b.order) return a.order < b.order;
        return x < y;
    });
    clusterOfVert.assign(flow.verts.size(), -1);
    for (std::uint32_t vi : vs) {
        clusterOfVert[vi] = static_cast<std::int32_t>(plan.clusters.size());
        Cluster cl;
        cl.anchorVert = static_cast<std::int32_t>(vi);
        cl.rank = flow.verts[vi].rank;
        cl.order = flow.verts[vi].order;
        plan.clusters.push_back(std::move(cl));
    }
    Cluster spare;
    spare.rank = static_cast<int>(flow.ranks.size());
    freeCluster = plan.clusters.size();
    plan.clusters.push_back(std::move(spare));

    // Anchors in flow (rank, then order) order: the artery walk's outer loop
    // and every "first flow-order anchor" tie-break below read this list.
    for (const std::vector<std::uint32_t>& layer : flow.ranks) {
        for (std::uint32_t vi : layer) {
            if (flow.verts[vi].kind == FlowVertex::Kind::Anchor) {
                flowAnchors.push_back(flow.verts[vi].comps[0]);
            }
        }
    }
}

// Which pins seed arteries, and in what order: the component's SidePlan when
// the placer built one (Left column then Right, slot order top->bottom, pin
// index as the slot tie exactly like symbols.cpp slotSort), else the builtin
// geometry heuristic recomputed here -- a connector is one column on the side
// its '&EDGE' faces, a box sends Out pins right and everything undirected
// left, both in natural pin-number order. Top/Bottom/dead pins never seed: a
// rail or ground pin gets a mark, not an artery.
void Planner::anchorPinSides(std::uint32_t comp, std::vector<std::uint32_t>& left,
                             std::vector<std::uint32_t>& right) const {
    const Component& c = d.components[comp];
    const std::size_t n = c.pins.size();
    const SidePlan* sp = comp < plans.size() ? &plans[comp] : nullptr;
    if (sp != nullptr && !sp->byPin.empty() && sp->byPin.size() == n) {
        for (std::uint32_t i = 0; i < n; ++i) {
            if (sp->byPin[i].side == Side::Left) left.push_back(i);
            else if (sp->byPin[i].side == Side::Right) right.push_back(i);
        }
        auto slotLess = [&](std::uint32_t a, std::uint32_t b) {
            int sa = sp->byPin[a].slot;
            int sb = sp->byPin[b].slot;
            if (sa != sb) return sa < sb;
            return a < b;
        };
        std::sort(left.begin(), left.end(), slotLess);
        std::sort(right.begin(), right.end(), slotLess);
        return;
    }

    auto naturalPin = [&](std::uint32_t a, std::uint32_t b) {
        const std::string& pa = c.pins[a].physical;
        const std::string& pb = c.pins[b].physical;
        if (naturalLess(pa, pb)) return true;
        if (naturalLess(pb, pa)) return false;
        return a < b;
    };
    if (m.kinds[comp] == SymbolKind::Connector) {
        bool flip = c.edge == "RIGHT" || c.edge == "BOTTOM";
        std::vector<std::uint32_t>& col = flip ? left : right;
        for (std::uint32_t i = 0; i < n; ++i) col.push_back(i);
        std::sort(col.begin(), col.end(), naturalPin);
        return;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const ComponentPin& p = c.pins[i];
        if (p.type == PinType::NC || p.type == PinType::Power || p.type == PinType::Ground) {
            continue;  // dead block, top row, bottom row: none seeds
        }
        if (p.direction == PortDir::Out) right.push_back(i);
        else left.push_back(i);
    }
    std::sort(left.begin(), left.end(), naturalPin);
    std::sort(right.begin(), right.end(), naturalPin);
}

void Planner::walkArteries() {
    for (std::uint32_t a : flowAnchors) {
        std::vector<std::uint32_t> left, right;
        anchorPinSides(a, left, right);
        Cluster& cl = plan.clusters[clusterIndexOf(a)];
        for (std::uint32_t p : left) walkPin(a, p, cl.left);
        for (std::uint32_t p : right) walkPin(a, p, cl.right);
    }
}

// A junction may form on a room-local multi-pin label net: every pin in this
// room, no mark, no port flag, three or more pins, and no earlier artery has
// spoken for it (a second arrival is a plain stop -- the drawn junction is
// already there). A net touching a SECOND anchor never junctions (drawing
// gap, WP6): the junction geometry cannot reach across clusters, so claiming
// the heads would force the net Named and hang a label on a net the router
// can draw whole. Left Free, the router joins every pin wordlessly, with the
// stub+mark ladder below it as ever.
bool Planner::junctionNet(std::int32_t net, std::uint32_t anchor) const {
    if (net < 0) return false;
    const Net& n = d.nets[static_cast<std::size_t>(net)];
    const RenderNet& rn = pg.nets[static_cast<std::size_t>(net)];
    if (n.pins.size() < 3) return false;
    if (rn.mark != NetMark::Label || rn.crossing || rn.direction != PortDir::None) return false;
    if (plan.netState[static_cast<std::size_t>(net)] != NetState::Free) return false;
    for (const PinRef& pr : n.pins) {
        if (!inRoom[pr.component]) return false;
        if (role[pr.component] == Role::Anchor && pr.component != anchor) return false;
    }
    return true;
}

// A two-terminal part BRIDGES between two of the seeding anchor's own nets
// when its far pin lands on another junction-eligible net of that anchor --
// the crystal between OSC_IN and OSC_OUT, the bootstrap cap between BST and
// the switch node. Such a part may be consumed by NEITHER side: an artery
// walking through it would build the far net's junction from the wrong end
// (the anchor's own pin there would be uncovered and force the net Named),
// and a junction claiming it as a shunt head would leave the sibling net
// uncoverable in the same way. Both sides refuse, the part keeps its own
// idiom, and the nets stay Free -- routed or paired by name, never a label
// hung on a net the plan could have drawn whole from the right pin.
bool Planner::bridgesAnchorNet(std::uint32_t comp, std::uint32_t farPin,
                               std::uint32_t anchor) const {
    std::int32_t farNet = d.components[comp].pins[farPin].net;
    return junctionNet(farNet, anchor) && compTouches(anchor, farNet);
}

// One artery from one anchor pin. The walk re-enters the case split after
// every consumed part, so a chain may run through several inline parts and
// then land on a junction. An artery that consumed nothing is dropped: the
// pin is a plain stub and needs no plan.
void Planner::walkPin(std::uint32_t anchor, std::uint32_t pin, std::vector<Artery>& out) {
    Artery art;
    art.anchorPin = pin;
    art.startNet = d.components[anchor].pins[pin].net;

    std::int32_t net = art.startNet;
    std::uint32_t fromComp = anchor, fromPin = pin;
    while (true) {
        if (isPrivate(net)) {
            PinRef other = otherEnd(net, fromComp, fromPin);
            if (other.component != fromComp && consumable(other.component)) {
                // A part bridging to a junction net of this same anchor is
                // left alone: that net's own pin must seed its junction (the
                // buck's BST pin walking through the bootstrap cap would
                // otherwise reach the switch node first, whatever the side
                // plan's seeding order, and build its junction with the
                // anchor's SW pin uncovered). The artery hands off here.
                std::uint32_t farPin = other.pin == 0 ? 1u : 0u;
                if (bridgesAnchorNet(other.component, farPin, anchor)) {
                    art.endNet = net;
                    break;
                }
                consume(other.component);
                art.steps.push_back(ArteryStep{ArteryStep::Kind::Inline,
                                               ChainElem{other.component, other.pin}, -1, {}});
                // Both pins of the link now sit on drawn geometry.
                plan.netState[static_cast<std::size_t>(net)] = NetState::Drawn;
                fromComp = other.component;
                fromPin = other.pin == 0 ? 1u : 0u;
                net = d.components[fromComp].pins[fromPin].net;
                continue;
            }
            // The far end is an anchor, a claimed part, or an ineligible
            // role: a bare handoff. The net stays Free for the router.
            art.endNet = net;
            break;
        }
        if (junctionNet(net, anchor)) {
            if (!buildJunction(art, net, anchor, fromComp, fromPin)) art.endNet = net;
            break;  // a junction always ends the artery (rail continuation included)
        }
        // Rail, ground, no-connect, crossing, port, foreign label, or an
        // unwired pin: stop; the drawing package puts the mark or tap there.
        art.endNet = net;
        break;
    }
    if (!art.steps.empty()) out.push_back(std::move(art));
}

// The string past a shunt head, walked away from the junction through private
// nets: classic walkForward. Interior links become drawn conductors.
void Planner::walkString(Shunt& s, std::uint32_t comp, std::uint32_t exitPin) {
    std::int32_t link = d.components[comp].pins[exitPin].net;
    while (isPrivate(link)) {
        PinRef other = otherEnd(link, comp, exitPin);
        if (other.component == comp || !consumable(other.component)) break;
        consume(other.component);
        plan.netState[static_cast<std::size_t>(link)] = NetState::Drawn;
        s.elems.push_back(ChainElem{other.component, other.pin});
        comp = other.component;
        exitPin = other.pin == 0 ? 1u : 0u;
        link = d.components[comp].pins[exitPin].net;
        if (isGnd(link) || isRailN(link)) break;
    }
    s.endNet = link;
    s.up = isRailN(link);
}

bool Planner::buildJunction(Artery& art, std::int32_t net, std::uint32_t anchor,
                            std::uint32_t fromComp, std::uint32_t fromPin) {
    const Net& n = d.nets[static_cast<std::size_t>(net)];

    // A head that bridges to a sibling junction net of the same anchor (the
    // crystal between the two OSC pins) refuses the WHOLE junction, before
    // anything is consumed: claiming the bridge here would leave the sibling
    // net uncoverable, and claiming everything BUT the bridge would leave
    // this net Named over a pin the router can join wordlessly. Both nets
    // stay Free and route -- the honest outcome where the two-junction shape
    // has no connected drawing (WP note: routed, never labelled).
    for (const PinRef& pr : n.pins) {
        if (!headEligible(pr.component)) continue;
        const Component& bc = d.components[pr.component];
        std::uint32_t bFar = pr.pin == 0 ? 1u : 0u;
        if (bc.pins[bFar].net == net) continue;  // a short, not a bridge
        if (bridgesAnchorNet(pr.component, bFar, anchor)) return false;
    }

    // Shunt heads in net-pin order: unclaimed two-terminal parts with exactly
    // one pin on the net. Both pins on it is a short -- skipped, left for the
    // router and its labels. kMaxShunts bounds the row; overflow heads stay
    // unclaimed and their pins force the net Named below.
    std::vector<Shunt> shunts;
    std::vector<std::uint32_t> headComps;
    for (const PinRef& pr : n.pins) {
        if (shunts.size() == kMaxShunts) break;
        if (!headEligible(pr.component)) continue;
        const Component& cc = d.components[pr.component];
        std::uint32_t farPin = pr.pin == 0 ? 1u : 0u;
        if (cc.pins[farPin].net == net) continue;  // a short, not a tap
        consume(pr.component);
        headComps.push_back(pr.component);
        Shunt s;
        s.elems.push_back(ChainElem{pr.component, pr.pin});
        walkString(s, pr.component, farPin);
        shunts.push_back(std::move(s));
    }
    if (shunts.empty()) return false;  // nothing to join: no junction step

    // Coverage: a pin counts as drawn when it sits on the part the artery
    // arrived through, on the seeding pin ITSELF, or on a claimed head (the
    // continuation head included). Anything else -- a second pin of the
    // anchor (the artery lands on one pin only; drawing gap, WP6), a Loose
    // part, a short, an overflow head, a child sheet port -- keeps a stub
    // with the net's mark, so the net is Named rather than Drawn.
    bool covered = true;
    for (const PinRef& pr : n.pins) {
        if (pr.component == fromComp && (fromComp != anchor || pr.pin == fromPin)) continue;
        bool isHead = false;
        for (std::uint32_t h : headComps) {
            if (h == pr.component) isHead = true;
        }
        if (!isHead) covered = false;
    }
    for (std::uint32_t b : room.children) {
        for (const BlockPort& p : d.blocks[b].ports) {
            if (p.net == net) covered = false;
        }
    }
    plan.netState[static_cast<std::size_t>(net)] = covered ? NetState::Drawn : NetState::Named;

    // Continuation: the longest rail-ending string straightens onto the
    // artery, so the wire reads anchor -> junction -> rail. Most elements
    // wins; ties by head designator (naturalLess), then head component index.
    // Only a fully covered junction may straighten (drawing gap, WP6): a
    // partial one must end on its own trunk so the trunk can carry the net's
    // name -- with a rail continuation there would be no trunk end to name,
    // and the uncovered stubs' labels would pair with nothing visible.
    std::int32_t best = -1;
    for (std::size_t i = 0; covered && i < shunts.size(); ++i) {
        if (!shunts[i].up) continue;
        if (best < 0) {
            best = static_cast<std::int32_t>(i);
            continue;
        }
        const Shunt& cand = shunts[i];
        const Shunt& cur = shunts[static_cast<std::size_t>(best)];
        std::uint32_t candHead = cand.elems[0].comp;
        std::uint32_t curHead = cur.elems[0].comp;
        bool take = false;
        if (cand.elems.size() != cur.elems.size()) {
            take = cand.elems.size() > cur.elems.size();
        } else if (naturalLess(keyOf(candHead), keyOf(curHead))) {
            take = true;
        } else if (naturalLess(keyOf(curHead), keyOf(candHead))) {
            take = false;
        } else {
            take = candHead < curHead;
        }
        if (take) best = static_cast<std::int32_t>(i);
    }

    ArteryStep js;
    js.kind = ArteryStep::Kind::Junction;
    js.net = net;
    Shunt cont;
    if (best >= 0) {
        cont = std::move(shunts[static_cast<std::size_t>(best)]);
        shunts.erase(shunts.begin() + best);
    }
    js.shunts = std::move(shunts);
    art.steps.push_back(std::move(js));

    if (best >= 0) {
        for (const ChainElem& e : cont.elems) {
            art.steps.push_back(ArteryStep{ArteryStep::Kind::Inline, e, -1, {}});
        }
        art.endNet = cont.endNet;  // a rail: its mark is implied, never Named
        art.namedEnd = false;
    } else {
        art.endNet = net;
        art.namedEnd = !covered;  // the junction ends the artery; a partial one keeps its mark
    }
    return true;
}

// Rail segments: per room rail in roomRails (first-touch) order. The classic
// placer's pin-count rule, cluster-scoped: a cluster earns a rail segment
// when its CONSUMERS on the rail -- the anchor's own pins (an MCU drinking a
// rail through five supply pins wants the segment exactly as much as five
// separate parts do), arteries ending on the rail, claimed rail satellites,
// and the ladder caps it claims -- number two or more. The first consumer
// cluster in flow order claims the room's unclaimed ladder caps; later
// clusters may still earn pin-only segments from their own consumers. A
// single consumer never earns a segment, and everything on the rail outside
// a group keeps the classic per-part flag. Runs AFTER claimPulls so claimed
// satellites count (ladders and pulls never compete for parts: their role
// sets are disjoint).
void Planner::claimDecaps() {
    for (std::int32_t rail : flow.roomRails) {
        std::vector<std::uint32_t> parts;
        for (std::uint32_t idx : room.components) {
            if (!claimed[idx] && role[idx] == Role::Ladder && railNet[idx] == rail) {
                parts.push_back(idx);
            }
        }
        bool capsTaken = false;
        for (std::uint32_t a : flowAnchors) {
            const std::size_t ci = clusterIndexOf(a);
            Cluster& cl = plan.clusters[ci];
            std::size_t consumers = 0;
            for (const ComponentPin& p : d.components[a].pins) {
                if (p.net == rail) ++consumers;
            }
            for (const std::vector<Artery>* side : {&cl.left, &cl.right}) {
                for (const Artery& art : *side) {
                    if (art.endNet == rail) ++consumers;
                }
            }
            for (std::uint32_t v : cl.satUps) {
                if (railNet[flow.verts[v].comps[0]] == rail) ++consumers;
            }
            if (consumers == 0) continue;
            std::vector<std::uint32_t> caps;
            if (!capsTaken) caps = parts;
            if (consumers + caps.size() < 2) continue;
            for (std::uint32_t c : caps) consume(c);
            if (!caps.empty()) capsTaken = true;
            plan.clusters[ci].decaps.push_back(DecapGroup{rail, std::move(caps)});
        }
        if (capsTaken || parts.empty()) continue;
        // No consumer cluster took the ladder: it groups by count alone in
        // the free cluster, a lone cap standing as its own loose cell.
        if (parts.size() >= 2) {
            for (std::uint32_t c : parts) consume(c);
            plan.clusters[freeCluster].decaps.push_back(DecapGroup{rail, std::move(parts)});
        } else {
            claim(parts[0]);
            plan.clusters[freeCluster].looseVerts.push_back(
                static_cast<std::uint32_t>(flow.vertexOf[parts[0]]));
        }
    }
}

// Satellites: each unclaimed pull-up/down (room-component order) stands at
// the first flow-order anchor with a pin on its signal net; with no such
// anchor it falls to the free cluster as a loose vertical.
void Planner::claimPulls() {
    for (std::uint32_t idx : room.components) {
        if (claimed[idx]) continue;
        if (role[idx] != Role::PullUp && role[idx] != Role::PullDown) continue;
        std::int32_t target = -1;
        for (std::uint32_t a : flowAnchors) {
            if (compTouches(a, sigNet[idx])) {
                target = static_cast<std::int32_t>(clusterIndexOf(a));
                break;
            }
        }
        std::uint32_t v = static_cast<std::uint32_t>(flow.vertexOf[idx]);
        if (target >= 0) {
            consume(idx);
            Cluster& cl = plan.clusters[static_cast<std::size_t>(target)];
            if (role[idx] == Role::PullUp) cl.satUps.push_back(v);
            else cl.satDowns.push_back(v);
        } else {
            claim(idx);
            plan.clusters[freeCluster].looseVerts.push_back(v);
        }
    }
}

// Loose leftovers (room-component order): a part sharing a plain Label net
// with an anchor stands in that anchor's cluster -- anchors scanned in flow
// order, first hit wins -- else in the free cluster. Rails and grounds are
// ambient and bind a part to nobody.
void Planner::claimLoose() {
    for (std::uint32_t idx : room.components) {
        if (claimed[idx] || role[idx] != Role::Loose) continue;
        std::int32_t target = -1;
        for (std::uint32_t a : flowAnchors) {
            bool shared = false;
            for (const ComponentPin& p : d.components[idx].pins) {
                if (p.net < 0) continue;
                if (pg.nets[static_cast<std::size_t>(p.net)].mark != NetMark::Label) continue;
                if (compTouches(a, p.net)) shared = true;
            }
            if (shared) {
                target = static_cast<std::int32_t>(clusterIndexOf(a));
                break;
            }
        }
        claim(idx);
        std::uint32_t v = static_cast<std::uint32_t>(flow.vertexOf[idx]);
        plan.clusters[target >= 0 ? static_cast<std::size_t>(target) : freeCluster]
            .looseVerts.push_back(v);
    }
}

// Remaining Chain parts collapse into maximal runs for the free cluster:
// back to the string's head first (ring-safe), then forward consuming the
// whole string -- the flow.cpp series-walk pattern, keeping entry pins.
void Planner::claimFreeRuns() {
    auto chainFree = [&](std::uint32_t comp) {
        return inRoom[comp] != 0 && role[comp] == Role::Chain && claimed[comp] == 0;
    };
    for (std::uint32_t idx : room.components) {
        if (!chainFree(idx)) continue;
        std::uint32_t head = idx;
        std::uint32_t headEntry = 0;
        std::vector<std::uint32_t> visited{head};
        while (true) {
            std::int32_t back = d.components[head].pins[headEntry].net;
            if (!isPrivate(back)) break;
            PinRef prev = otherEnd(back, head, headEntry);
            if (!chainFree(prev.component)) break;
            bool seen = false;
            for (std::uint32_t v : visited) {
                if (v == prev.component) seen = true;
            }
            if (seen) break;  // a ring: break it where the scan found it
            visited.push_back(prev.component);
            head = prev.component;
            headEntry = prev.pin == 0 ? 1u : 0u;
        }
        std::vector<ChainElem> run;
        claim(head);
        run.push_back(ChainElem{head, headEntry});
        std::uint32_t cur = head;
        std::uint32_t exitPin = headEntry == 0 ? 1u : 0u;
        std::int32_t net = d.components[cur].pins[exitPin].net;
        if (!isGnd(net) && !isRailN(net)) {
            while (isPrivate(net)) {
                PinRef nx = otherEnd(net, cur, exitPin);
                if (!chainFree(nx.component)) break;
                claim(nx.component);
                run.push_back(ChainElem{nx.component, nx.pin});
                cur = nx.component;
                exitPin = nx.pin == 0 ? 1u : 0u;
                net = d.components[cur].pins[exitPin].net;
                if (isGnd(net) || isRailN(net)) break;
            }
        }
        plan.clusters[freeCluster].freeRuns.push_back(std::move(run));
    }
}

}  // namespace

RoomPlan buildRoomPlan(const RenderPage& page, const RenderRoom& room, const RoomFlow& flow,
                       const std::vector<SidePlan>& plans, const RenderModel& m) {
    Planner pl(page, room, flow, plans, m);
    pl.classify();
    pl.makeClusters();
    // Claiming order is fixed and first-wins: arteries, then satellites,
    // then rail segments (which count the claimed satellites and artery rail
    // ends as consumers -- pulls and ladders can never contend for a part,
    // so the swap changes no claim), then loose leftovers, then free runs.
    pl.walkArteries();
    pl.claimPulls();
    pl.claimDecaps();
    pl.claimLoose();
    pl.claimFreeRuns();

#ifndef NDEBUG
    // The netState invariant: a Drawn net's every pin sits on a consumed
    // part or on an anchor; Named leaves the rest a stub with the mark.
    const Design& d = *m.design;
    for (std::size_t ni = 0; ni < pl.plan.netState.size(); ++ni) {
        if (pl.plan.netState[ni] != NetState::Drawn) continue;
        for (const PinRef& pr : d.nets[ni].pins) {
            assert(pl.plan.consumed[pr.component] ||
                   (pl.inRoom[pr.component] && pl.role[pr.component] == Role::Anchor));
        }
    }
#endif
    return std::move(pl.plan);
}

}  // namespace manta::render
