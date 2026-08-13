// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Cluster plans: artery walks, junctions with shunt strings and their rail
// continuation, the claiming order (arteries, decaps, satellites, loose,
// free runs), and the net classification. Designs are built directly and run
// through buildRenderModel so the page's net marks are the real ones; plans
// are checked structurally, and every fixture must satisfy the netState and
// component-totality invariants.
#include <algorithm>
#include <string>
#include <vector>

#include "harness.h"
#include "render/cluster.h"
#include "render/flow.h"
#include "render/model.h"

using namespace manta;
using namespace manta::render;

namespace {

// A small direct-construction builder: components, nets, and pin wiring in
// declaration order, exactly as the linker would have left them.
struct TB {
    Design d;

    std::uint32_t comp(std::string desig, std::string type, std::vector<std::string> pinNames,
                       std::string edge = "", PartType pt = PartType::BoardPart,
                       std::string section = "") {
        Component c;
        c.designator = std::move(desig);
        c.partName = "TEST";
        c.type = std::move(type);
        c.partType = pt;
        c.edge = std::move(edge);
        c.section = std::move(section);
        int n = 0;
        for (std::string& name : pinNames) {
            ComponentPin p;
            p.physical = std::to_string(++n);
            p.logical = std::move(name);
            c.pins.push_back(std::move(p));
        }
        d.components.push_back(std::move(c));
        return static_cast<std::uint32_t>(d.components.size() - 1);
    }

    std::int32_t net(std::string name, bool ground = false) {
        Net n;
        n.name = std::move(name);
        n.ground = ground;
        d.nets.push_back(std::move(n));
        return static_cast<std::int32_t>(d.nets.size() - 1);
    }

    void wire(std::uint32_t c, std::uint32_t pin, std::int32_t netIdx,
              PortDir dir = PortDir::None) {
        d.components[c].pins[pin].net = netIdx;
        d.components[c].pins[pin].direction = dir;
        d.nets[static_cast<std::size_t>(netIdx)].pins.push_back(PinRef{c, pin});
    }
};

// Flow and plan for the model's first page's first room, with all-empty
// SidePlans (the builtin side heuristic) unless a test hands its own.
struct Built {
    RoomFlow flow;
    RoomPlan plan;
};

Built build(const RenderModel& m, std::vector<SidePlan> plans = {}) {
    const RenderPage& pg = m.pages[0];
    const RenderRoom& room = pg.rooms[0];
    Built b;
    b.flow = buildRoomFlow(pg, room, m);
    if (plans.empty()) plans.assign(m.design->components.size(), SidePlan{});
    b.plan = buildRoomPlan(pg, room, b.flow, plans, m);
    return b;
}

// The component's own cluster; a static empty one (anchorVert -1, so the
// missing-cluster case still fails the caller's anchorVert check) when the
// plan has none, keeping every access null-free.
const Cluster& clusterOf(const Built& b, std::uint32_t comp) {
    std::int32_t v = b.flow.vertexOf[comp];
    for (const Cluster& c : b.plan.clusters) {
        if (c.anchorVert == v) return c;
    }
    static const Cluster none{};
    return none;
}

const Cluster& freeCluster(const Built& b) { return b.plan.clusters.back(); }

bool vertListHas(const Built& b, const std::vector<std::uint32_t>& verts, std::uint32_t comp) {
    for (std::uint32_t v : verts) {
        for (std::uint32_t c : b.flow.verts[v].comps) {
            if (c == comp) return true;
        }
    }
    return false;
}

// True when the component is drawn inside any artery of any cluster, as an
// inline step, a shunt element, or a continuation.
bool inAnyArtery(const RoomPlan& p, std::uint32_t comp) {
    for (const Cluster& cl : p.clusters) {
        for (const std::vector<Artery>* side : {&cl.left, &cl.right}) {
            for (const Artery& a : *side) {
                for (const ArteryStep& s : a.steps) {
                    if (s.kind == ArteryStep::Kind::Inline && s.elem.comp == comp) return true;
                    for (const Shunt& sh : s.shunts) {
                        for (const ChainElem& e : sh.elems) {
                            if (e.comp == comp) return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

// The netState invariant: a net never marked Drawn unless every pin sits on
// drawn plan geometry -- a consumed part or an anchor of this room. Named
// nets may leave pins for the stub+mark fallback; Free nets say nothing.
void checkNetInvariant(const Built& b, const Design& d, const RenderRoom& room) {
    std::vector<char> inRoom(d.components.size(), 0), anchor(d.components.size(), 0);
    for (std::uint32_t c : room.components) inRoom[c] = 1;
    for (const Cluster& cl : b.plan.clusters) {
        if (cl.anchorVert < 0) continue;
        const FlowVertex& v = b.flow.verts[static_cast<std::size_t>(cl.anchorVert)];
        if (v.kind == FlowVertex::Kind::Anchor) anchor[v.comps[0]] = 1;
    }
    CHECK_EQ(b.plan.netState.size(), d.nets.size());
    for (std::size_t ni = 0; ni < b.plan.netState.size(); ++ni) {
        if (b.plan.netState[ni] != NetState::Drawn) continue;
        for (const PinRef& pr : d.nets[ni].pins) {
            CHECK(inRoom[pr.component]);
            CHECK(b.plan.consumed[pr.component] || anchor[pr.component]);
        }
    }
}

// Component totality: every room component is exactly one of -- a cluster
// anchor, drawn inside an artery/decap/satellite (and then consumed), or
// listed once as a loose vertex or free-run element (and then not consumed).
void checkComponentTotality(const Built& b, const Design& d, const RenderRoom& room) {
    std::vector<int> seen(d.components.size(), 0);
    std::vector<int> inside(d.components.size(), 0);
    for (const Cluster& cl : b.plan.clusters) {
        for (const std::vector<Artery>* side : {&cl.left, &cl.right}) {
            for (const Artery& a : *side) {
                for (const ArteryStep& s : a.steps) {
                    if (s.kind == ArteryStep::Kind::Inline) {
                        ++seen[s.elem.comp];
                        inside[s.elem.comp] = 1;
                    }
                    for (const Shunt& sh : s.shunts) {
                        for (const ChainElem& e : sh.elems) {
                            ++seen[e.comp];
                            inside[e.comp] = 1;
                        }
                    }
                }
            }
        }
        for (const DecapGroup& g : cl.decaps) {
            for (std::uint32_t c : g.comps) {
                ++seen[c];
                inside[c] = 1;
            }
        }
        for (const std::vector<std::uint32_t>* sats : {&cl.satUps, &cl.satDowns}) {
            for (std::uint32_t v : *sats) {
                for (std::uint32_t c : b.flow.verts[v].comps) {
                    ++seen[c];
                    inside[c] = 1;
                }
            }
        }
        for (std::uint32_t v : cl.looseVerts) {
            for (std::uint32_t c : b.flow.verts[v].comps) ++seen[c];
        }
        for (const std::vector<ChainElem>& run : cl.freeRuns) {
            for (const ChainElem& e : run) ++seen[e.comp];
        }
    }
    std::vector<char> anchor(d.components.size(), 0);
    for (const Cluster& cl : b.plan.clusters) {
        if (cl.anchorVert < 0) continue;
        const FlowVertex& v = b.flow.verts[static_cast<std::size_t>(cl.anchorVert)];
        if (v.kind == FlowVertex::Kind::Anchor) anchor[v.comps[0]] = 1;
    }
    for (std::uint32_t c : room.components) {
        CHECK_EQ(seen[c], anchor[c] ? 0 : 1);
        CHECK_EQ(static_cast<int>(b.plan.consumed[c] != 0), inside[c]);
    }
}

void checkInvariants(const Built& b, const Design& d, const RenderRoom& room) {
    checkNetInvariant(b, d, room);
    checkComponentTotality(b, d, room);
}

// ---------------------------------------------------------------------------
// The buck fixture: anchor U2's SW pin feeds a four-pin switch node -- a
// catch diode down, a bootstrap cap sideways into a private net back to the
// anchor, and the inductor onto the output rail. `reversed` permutes the
// component declaration order (and with it every net's pin order) without
// changing the circuit.
// ---------------------------------------------------------------------------

struct Buck {
    TB b;
    std::uint32_t u2 = 0, d2 = 0, c3 = 0, l1 = 0;
    std::int32_t sw = 0, bst = 0, v5 = 0, gnd = 0;
};

Buck makeBuck(bool reversed) {
    Buck f;
    f.sw = f.b.net("SW");
    f.bst = f.b.net("BST");
    f.v5 = f.b.net("5V");
    f.gnd = f.b.net("GND", true);
    auto declare = [&](int which) {
        switch (which) {
            case 0:
                f.u2 = f.b.comp("U2", "", {"SW", "BST", "P3", "P4", "P5"});
                f.b.wire(f.u2, 0, f.sw);
                f.b.wire(f.u2, 1, f.bst);
                break;
            case 1:
                f.d2 = f.b.comp("D2", "led", {"A", "K"});
                f.b.wire(f.d2, 0, f.gnd);
                f.b.wire(f.d2, 1, f.sw);
                break;
            case 2:
                f.c3 = f.b.comp("C3", "capacitor", {"1", "2"});
                f.b.wire(f.c3, 0, f.sw);
                f.b.wire(f.c3, 1, f.bst);
                break;
            case 3:
                f.l1 = f.b.comp("L1", "inductor", {"1", "2"});
                f.b.wire(f.l1, 0, f.sw);
                f.b.wire(f.l1, 1, f.v5);
                break;
        }
    };
    if (reversed) {
        for (int i = 3; i >= 0; --i) declare(i);
    } else {
        for (int i = 0; i <= 3; ++i) declare(i);
    }
    return f;
}

}  // namespace

// ---------------------------------------------------------------------------
// The buck shape: junction, shunts, rail continuation.
// ---------------------------------------------------------------------------

TEST_CASE("buck: junction with catch and bootstrap shunts, inductor continues to the rail") {
    Buck f = makeBuck(false);
    RenderModel m = buildRenderModel(f.b.d);
    Built b = build(m);
    checkInvariants(b, f.b.d, m.pages[0].rooms[0]);

    const Cluster& cl = clusterOf(b, f.u2);
    CHECK(cl.anchorVert >= 0);
    // The SW pin is undirected, so the heuristic seeds it from the Left; it
    // is the only pin whose walk consumed anything.
    CHECK_EQ(cl.left.size(), static_cast<std::size_t>(1));
    CHECK_EQ(cl.right.size(), static_cast<std::size_t>(0));

    const Artery& a = cl.left[0];
    CHECK_EQ(a.anchorPin, 0u);
    CHECK_EQ(a.startNet, f.sw);
    CHECK_EQ(a.steps.size(), static_cast<std::size_t>(2));

    // One junction on SW: D2 hangs down to ground, C3 sideways to the
    // private bootstrap net; neither stands up.
    const ArteryStep& j = a.steps[0];
    CHECK(j.kind == ArteryStep::Kind::Junction);
    CHECK_EQ(j.net, f.sw);
    CHECK_EQ(j.shunts.size(), static_cast<std::size_t>(2));
    CHECK_EQ(j.shunts[0].elems.size(), static_cast<std::size_t>(1));
    CHECK_EQ(j.shunts[0].elems[0].comp, f.d2);
    CHECK_EQ(j.shunts[0].endNet, f.gnd);
    CHECK_FALSE(j.shunts[0].up);
    CHECK_EQ(j.shunts[1].elems[0].comp, f.c3);
    CHECK_EQ(j.shunts[1].endNet, f.bst);
    CHECK_FALSE(j.shunts[1].up);

    // The inductor's string ends on the rail, so it straightens onto the
    // artery instead of hanging.
    const ArteryStep& cont = a.steps[1];
    CHECK(cont.kind == ArteryStep::Kind::Inline);
    CHECK_EQ(cont.elem.comp, f.l1);
    CHECK_EQ(cont.elem.entryPin, 0u);
    CHECK_EQ(a.endNet, f.v5);
    CHECK_FALSE(a.namedEnd);

    // Every SW pin is drawn: the anchor's own, both shunt heads, the
    // continuation head. The bootstrap net keeps its labels.
    CHECK(b.plan.netState[static_cast<std::size_t>(f.sw)] == NetState::Drawn);
    CHECK(b.plan.netState[static_cast<std::size_t>(f.bst)] == NetState::Free);
    CHECK(b.plan.consumed[f.d2] != 0);
    CHECK(b.plan.consumed[f.c3] != 0);
    CHECK(b.plan.consumed[f.l1] != 0);
}

// ---------------------------------------------------------------------------
// Private chains and handoffs.
// ---------------------------------------------------------------------------

TEST_CASE("private chain: two inline steps, interior nets Drawn, the label ends it") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"IN", "P2", "P3", "P4", "P5"});
    std::uint32_t r1 = t.comp("R1", "resistor", {"1", "2"});
    std::uint32_t c1 = t.comp("C1", "capacitor", {"1", "2"});
    std::int32_t nA = t.net("A"), mid = t.net("MID"), out = t.net("OUT");
    t.wire(u1, 0, nA);
    t.wire(r1, 0, nA);
    t.wire(r1, 1, mid);
    t.wire(c1, 0, mid);
    t.wire(c1, 1, out);  // OUT has one pin: a label stub, not walkable

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    const Cluster& cl = clusterOf(b, u1);
    CHECK(cl.anchorVert >= 0);
    CHECK_EQ(cl.left.size(), static_cast<std::size_t>(1));
    const Artery& a = cl.left[0];
    CHECK_EQ(a.steps.size(), static_cast<std::size_t>(2));
    CHECK(a.steps[0].kind == ArteryStep::Kind::Inline);
    CHECK_EQ(a.steps[0].elem.comp, r1);
    CHECK_EQ(a.steps[0].elem.entryPin, 0u);
    CHECK(a.steps[1].kind == ArteryStep::Kind::Inline);
    CHECK_EQ(a.steps[1].elem.comp, c1);
    CHECK_EQ(a.steps[1].elem.entryPin, 0u);
    CHECK_EQ(a.endNet, out);
    CHECK_FALSE(a.namedEnd);

    CHECK(b.plan.netState[static_cast<std::size_t>(nA)] == NetState::Drawn);
    CHECK(b.plan.netState[static_cast<std::size_t>(mid)] == NetState::Drawn);
    CHECK(b.plan.netState[static_cast<std::size_t>(out)] == NetState::Free);
}

TEST_CASE("handoff: the first anchor takes the cap, the far net stays Free") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"OUT", "P2", "P3", "P4", "P5"});
    std::uint32_t u2 = t.comp("U2", "", {"IN", "P2", "P3", "P4", "P5"});
    std::uint32_t c1 = t.comp("C1", "capacitor", {"1", "2"});
    std::int32_t x = t.net("X"), y = t.net("Y");
    t.wire(u1, 0, x);
    t.wire(c1, 0, x);
    t.wire(c1, 1, y);
    t.wire(u2, 0, y);

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    // U1 walks first (flow order) and consumes the cap; its artery ends on
    // the far net as a bare handoff, which stays Free for the router.
    const Cluster& cu1 = clusterOf(b, u1);
    CHECK(cu1.anchorVert >= 0);
    CHECK_EQ(cu1.left.size(), static_cast<std::size_t>(1));
    const Artery& a = cu1.left[0];
    CHECK_EQ(a.steps.size(), static_cast<std::size_t>(1));
    CHECK_EQ(a.steps[0].elem.comp, c1);
    CHECK_EQ(a.endNet, y);
    CHECK_FALSE(a.namedEnd);
    CHECK(b.plan.netState[static_cast<std::size_t>(x)] == NetState::Drawn);
    CHECK(b.plan.netState[static_cast<std::size_t>(y)] == NetState::Free);

    // The second anchor finds the cap claimed and stops immediately: no
    // artery at all.
    const Cluster& cu2 = clusterOf(b, u2);
    CHECK(cu2.anchorVert >= 0);
    CHECK_EQ(cu2.left.size(), static_cast<std::size_t>(0));
    CHECK_EQ(cu2.right.size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Junction coverage and the Named fallback.
// ---------------------------------------------------------------------------

TEST_CASE("a junction with a Loose pin is Named; the loose part joins that cluster") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"P1", "P2", "P3", "P4", "P5"});
    std::uint32_t r1 = t.comp("R1", "resistor", {"1", "2"});
    std::uint32_t r2 = t.comp("R2", "resistor", {"1", "2"});
    std::uint32_t tp = t.comp("TP1", "testpoint", {"TP"});
    std::int32_t nn = t.net("NODE"), gnd = t.net("GND", true), rb = t.net("RB");
    t.wire(u1, 0, nn);
    t.wire(r1, 0, nn);
    t.wire(r1, 1, gnd);
    t.wire(r2, 0, nn);
    t.wire(r2, 1, rb);
    t.wire(tp, 0, nn);

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    const Cluster& cl = clusterOf(b, u1);
    CHECK(cl.anchorVert >= 0);
    CHECK_EQ(cl.left.size(), static_cast<std::size_t>(1));
    const Artery& a = cl.left[0];
    CHECK_EQ(a.steps.size(), static_cast<std::size_t>(1));
    const ArteryStep& j = a.steps[0];
    CHECK(j.kind == ArteryStep::Kind::Junction);
    CHECK_EQ(j.shunts.size(), static_cast<std::size_t>(2));  // R1 down, R2 out; no rail ender

    // The test point's pin is not drawn by the junction, so the net keeps
    // its mark at the artery's end and at every uncovered stub.
    CHECK(b.plan.netState[static_cast<std::size_t>(nn)] == NetState::Named);
    CHECK_EQ(a.endNet, nn);
    CHECK(a.namedEnd);

    // The loose test point shares NODE with the anchor: it stands in that
    // anchor's cluster, not the free one.
    CHECK(vertListHas(b, cl.looseVerts, tp));
    CHECK(b.plan.consumed[tp] == 0);
}

TEST_CASE("a part with both pins on the net is a short: skipped, left unclaimed") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"P1", "P2", "P3", "P4", "P5"});
    std::uint32_t rs = t.comp("RS", "resistor", {"1", "2"});
    std::uint32_t r1 = t.comp("R1", "resistor", {"1", "2"});
    std::uint32_t rs2 = t.comp("RS2", "resistor", {"1", "2"});
    std::int32_t ns = t.net("NS"), ns2 = t.net("NS2"), gnd = t.net("GND", true);
    t.wire(u1, 0, ns);
    t.wire(rs, 0, ns);
    t.wire(rs, 1, ns);
    t.wire(r1, 0, ns);
    t.wire(r1, 1, gnd);
    t.wire(u1, 1, ns2);
    t.wire(rs2, 0, ns2);
    t.wire(rs2, 1, ns2);

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    // NS builds a junction from R1 alone; the short's two pins stay
    // uncovered, so the net is Named.
    const Cluster& cl = clusterOf(b, u1);
    CHECK(cl.anchorVert >= 0);
    CHECK_EQ(cl.left.size(), static_cast<std::size_t>(1));
    const ArteryStep& j = cl.left[0].steps[0];
    CHECK(j.kind == ArteryStep::Kind::Junction);
    CHECK_EQ(j.net, ns);
    CHECK_EQ(j.shunts.size(), static_cast<std::size_t>(1));
    CHECK_EQ(j.shunts[0].elems[0].comp, r1);
    CHECK(b.plan.netState[static_cast<std::size_t>(ns)] == NetState::Named);
    CHECK(b.plan.consumed[rs] == 0);

    // NS2 holds only the anchor and a short: nothing to join, no junction,
    // and the net stays Free.
    CHECK(b.plan.netState[static_cast<std::size_t>(ns2)] == NetState::Free);
    CHECK(b.plan.consumed[rs2] == 0);

    // Both shorts fall through to the free cluster's runs.
    CHECK_EQ(freeCluster(b).freeRuns.size(), static_cast<std::size_t>(2));
}

TEST_CASE("kMaxShunts bounds a junction; overflow parts stay for later idioms") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"P1", "P2", "P3", "P4", "P5"});
    std::int32_t big = t.net("BIG"), gnd = t.net("GND", true);
    t.wire(u1, 0, big);
    std::vector<std::uint32_t> rs;
    for (int i = 1; i <= 8; ++i) {
        std::uint32_t r = t.comp("R" + std::to_string(i), "resistor", {"1", "2"});
        t.wire(r, 0, big);
        t.wire(r, 1, gnd);
        rs.push_back(r);
    }

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    const Cluster& cl = clusterOf(b, u1);
    CHECK(cl.anchorVert >= 0);
    const ArteryStep& j = cl.left[0].steps[0];
    CHECK(j.kind == ArteryStep::Kind::Junction);
    CHECK_EQ(j.shunts.size(), static_cast<std::size_t>(6));
    // First six in net-pin order are claimed; the excess stays out of the
    // junction and the net keeps its mark on their stubs.
    for (std::size_t i = 0; i < 6; ++i) CHECK(inAnyArtery(b.plan, rs[i]));
    CHECK_FALSE(inAnyArtery(b.plan, rs[6]));
    CHECK_FALSE(inAnyArtery(b.plan, rs[7]));
    CHECK(b.plan.netState[static_cast<std::size_t>(big)] == NetState::Named);
    CHECK(cl.left[0].namedEnd);

    // The two overflow pull-downs then claim their satellite place.
    CHECK_EQ(cl.satDowns.size(), static_cast<std::size_t>(2));
    CHECK(vertListHas(b, cl.satDowns, rs[6]));
    CHECK(vertListHas(b, cl.satDowns, rs[7]));
}

TEST_CASE("a net touching a second anchor never junctions: it stays Free to route") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"P1", "P2", "P3", "P4", "P5"});
    std::uint32_t u2 = t.comp("U2", "", {"P1", "P2", "P3", "P4", "P5"});
    std::uint32_t r1 = t.comp("R1", "resistor", {"1", "2"});
    std::int32_t nn = t.net("NODE"), gnd = t.net("GND", true);
    t.wire(u1, 0, nn);
    t.wire(u2, 0, nn);
    t.wire(r1, 0, nn);
    t.wire(r1, 1, gnd);

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    // The drawing cannot reach across clusters, and claiming R1 would force
    // NODE to Named -- a label on a net the router can draw whole. So no
    // junction forms anywhere, the net stays Free, and the pull-down claims
    // its satellite place instead.
    CHECK_FALSE(inAnyArtery(b.plan, r1));
    CHECK(b.plan.netState[static_cast<std::size_t>(nn)] == NetState::Free);
    for (const Cluster& cl : b.plan.clusters) {
        for (const std::vector<Artery>* side : {&cl.left, &cl.right}) {
            CHECK_EQ(side->size(), static_cast<std::size_t>(0));
        }
    }
    bool sat = false;
    for (const Cluster& cl : b.plan.clusters) sat = sat || vertListHas(b, cl.satDowns, r1);
    CHECK(sat);
}

TEST_CASE("a second pin of the seeding anchor is not covered: the junction is Named") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"P1", "P2", "P3", "P4", "P5"});
    std::uint32_t r1 = t.comp("R1", "resistor", {"1", "2"});
    std::int32_t nn = t.net("NODE"), gnd = t.net("GND", true);
    t.wire(u1, 0, nn);
    t.wire(u1, 1, nn);
    t.wire(r1, 0, nn);
    t.wire(r1, 1, gnd);

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    // The artery lands on ONE pin; the anchor's other pin keeps a stub with
    // the net's mark, so the net is Named and the trunk end carries it too.
    const Cluster& cl = clusterOf(b, u1);
    CHECK(cl.anchorVert >= 0);
    CHECK_EQ(cl.left.size(), static_cast<std::size_t>(1));
    const Artery& a = cl.left[0];
    CHECK_EQ(a.steps.size(), static_cast<std::size_t>(1));
    CHECK(a.steps[0].kind == ArteryStep::Kind::Junction);
    CHECK(b.plan.netState[static_cast<std::size_t>(nn)] == NetState::Named);
    CHECK_EQ(a.endNet, nn);
    CHECK(a.namedEnd);
}

TEST_CASE("an uncovered junction keeps its rail string standing so the trunk is named") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"P1", "P2", "P3", "P4", "P5"});
    std::uint32_t r1 = t.comp("R1", "resistor", {"1", "2"});
    std::uint32_t l1 = t.comp("L1", "inductor", {"1", "2"});
    std::uint32_t tp = t.comp("TP1", "testpoint", {"TP"});
    std::int32_t nn = t.net("NODE"), gnd = t.net("GND", true), rail = t.net("5V");
    t.wire(u1, 0, nn);
    t.wire(r1, 0, nn);
    t.wire(r1, 1, gnd);
    t.wire(l1, 0, nn);
    t.wire(l1, 1, rail);
    t.wire(tp, 0, nn);

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    // The test point's pin is uncovered, so the net is Named -- and the
    // inductor must NOT straighten onto the artery: with a rail continuation
    // there would be no trunk end to carry the name the uncovered stub's
    // label pairs with. Both strings hang; the artery ends on its trunk.
    const Cluster& cl = clusterOf(b, u1);
    CHECK(cl.anchorVert >= 0);
    CHECK_EQ(cl.left.size(), static_cast<std::size_t>(1));
    const Artery& a = cl.left[0];
    CHECK_EQ(a.steps.size(), static_cast<std::size_t>(1));
    const ArteryStep& j = a.steps[0];
    CHECK(j.kind == ArteryStep::Kind::Junction);
    CHECK_EQ(j.shunts.size(), static_cast<std::size_t>(2));
    bool upSeen = false;
    for (const Shunt& sh : j.shunts) upSeen = upSeen || sh.up;
    CHECK(upSeen);  // the inductor stands, endNet 5V
    CHECK(b.plan.netState[static_cast<std::size_t>(nn)] == NetState::Named);
    CHECK_EQ(a.endNet, nn);
    CHECK(a.namedEnd);
}

// ---------------------------------------------------------------------------
// Role exclusivity in the private-chain walk.
// ---------------------------------------------------------------------------

TEST_CASE("a pull-up is never inlined; a pull-down is; a ladder stays vertical") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"P1", "P2", "P3", "P4", "P5"});
    std::uint32_t rp = t.comp("RP", "resistor", {"1", "2"});
    std::uint32_t rd = t.comp("RD", "resistor", {"1", "2"});
    std::uint32_t cl1 = t.comp("CL", "capacitor", {"1", "2"});
    std::int32_t pa = t.net("PA"), pd = t.net("PD"), rail = t.net("3V3"),
                 gnd = t.net("GND", true);
    t.wire(u1, 0, pa);
    t.wire(rp, 0, pa);
    t.wire(rp, 1, rail);  // PullUp on a private net: the walk must not take it
    t.wire(u1, 1, pd);
    t.wire(rd, 0, pd);
    t.wire(rd, 1, gnd);  // PullDown on a private net: consumable
    t.wire(cl1, 0, rail);
    t.wire(cl1, 1, gnd);  // Ladder: no artery can ever reach it

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    CHECK_FALSE(inAnyArtery(b.plan, rp));
    CHECK_FALSE(inAnyArtery(b.plan, cl1));
    CHECK(inAnyArtery(b.plan, rd));

    const Cluster& cu = clusterOf(b, u1);
    CHECK(cu.anchorVert >= 0);
    // The pull-up stands as a satellite at the anchor on its signal net; the
    // pull-down was already drawn inline, so no satellite for it.
    CHECK(vertListHas(b, cu.satUps, rp));
    CHECK_EQ(cu.satDowns.size(), static_cast<std::size_t>(0));
    CHECK(b.plan.netState[static_cast<std::size_t>(pa)] == NetState::Free);
    CHECK(b.plan.netState[static_cast<std::size_t>(pd)] == NetState::Drawn);

    // The lone ladder's rail touches no anchor: it falls to the free
    // cluster as a loose vertical.
    CHECK(vertListHas(b, freeCluster(b).looseVerts, cl1));
}

// ---------------------------------------------------------------------------
// Decap grouping.
// ---------------------------------------------------------------------------

TEST_CASE("two caps on a rail group at the anchor; a single cap stands loose") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"VCC", "P2", "P3", "P4", "P5"});
    std::uint32_t c1 = t.comp("C1", "capacitor", {"1", "2"});
    std::uint32_t c2 = t.comp("C2", "capacitor", {"1", "2"});
    std::uint32_t c4 = t.comp("C4", "capacitor", {"1", "2"});
    std::int32_t r33 = t.net("3V3"), r5 = t.net("5V"), gnd = t.net("GND", true);
    t.wire(u1, 0, r33);
    t.wire(u1, 1, r5);
    t.wire(c1, 0, r33);
    t.wire(c1, 1, gnd);
    t.wire(c2, 0, r33);
    t.wire(c2, 1, gnd);
    t.wire(c4, 0, r5);
    t.wire(c4, 1, gnd);

    RenderModel m = buildRenderModel(t.d);
    Built b = build(m);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    const Cluster& cl = clusterOf(b, u1);
    CHECK(cl.anchorVert >= 0);
    CHECK_EQ(cl.decaps.size(), static_cast<std::size_t>(1));
    CHECK_EQ(cl.decaps[0].rail, r33);
    CHECK_EQ(cl.decaps[0].comps.size(), static_cast<std::size_t>(2));
    CHECK_EQ(cl.decaps[0].comps[0], c1);
    CHECK_EQ(cl.decaps[0].comps[1], c2);
    CHECK(b.plan.consumed[c1] != 0);
    CHECK(b.plan.consumed[c2] != 0);

    // The 5V rail holds one cap: it keeps its own cell at the same anchor.
    CHECK(vertListHas(b, cl.looseVerts, c4));
    CHECK(b.plan.consumed[c4] == 0);
    CHECK(cl.decaps.size() == 1);
}

// ---------------------------------------------------------------------------
// SidePlan seeding order.
// ---------------------------------------------------------------------------

TEST_CASE("a SidePlan orders the seeds: slots top to bottom, Left before Right") {
    TB t;
    std::uint32_t u1 = t.comp("U1", "", {"A", "B", "C", "D", "E"});
    std::uint32_t ra = t.comp("RA", "resistor", {"1", "2"});
    std::uint32_t rb = t.comp("RB", "resistor", {"1", "2"});
    std::uint32_t rc = t.comp("RC", "resistor", {"1", "2"});
    std::uint32_t rd = t.comp("RD", "resistor", {"1", "2"});
    std::int32_t nA = t.net("NA"), nB = t.net("NB"), nC = t.net("NC"), nD = t.net("ND");
    std::int32_t eA = t.net("EA"), eB = t.net("EB"), eC = t.net("EC"), eD = t.net("ED");
    t.wire(u1, 0, nA);
    t.wire(ra, 0, nA);
    t.wire(ra, 1, eA);
    t.wire(u1, 1, nB);
    t.wire(rb, 0, nB);
    t.wire(rb, 1, eB);
    t.wire(u1, 2, nC);
    t.wire(rc, 0, nC);
    t.wire(rc, 1, eC);
    t.wire(u1, 3, nD);
    t.wire(rd, 0, nD);
    t.wire(rd, 1, eD);

    RenderModel m = buildRenderModel(t.d);
    // Hand-built plan: B above A on the Right, C alone on the Left, D on
    // Top (a vertical pin never seeds), E dead.
    std::vector<SidePlan> plans(t.d.components.size());
    plans[u1].byPin = {PinPlan{Side::Right, 1, false}, PinPlan{Side::Right, 0, false},
                       PinPlan{Side::Left, 0, false}, PinPlan{Side::Top, 0, false},
                       PinPlan{Side::Right, 2, false}};
    Built b = build(m, plans);
    checkInvariants(b, t.d, m.pages[0].rooms[0]);

    const Cluster& cl = clusterOf(b, u1);
    CHECK(cl.anchorVert >= 0);
    CHECK_EQ(cl.left.size(), static_cast<std::size_t>(1));
    CHECK_EQ(cl.left[0].anchorPin, 2u);
    CHECK_EQ(cl.left[0].steps[0].elem.comp, rc);
    CHECK_EQ(cl.right.size(), static_cast<std::size_t>(2));
    CHECK_EQ(cl.right[0].anchorPin, 1u);  // slot 0 before slot 1
    CHECK_EQ(cl.right[0].steps[0].elem.comp, rb);
    CHECK_EQ(cl.right[1].anchorPin, 0u);
    CHECK_EQ(cl.right[1].steps[0].elem.comp, ra);

    // The Top pin seeded nothing: its partner stays for the free runs.
    CHECK(b.plan.consumed[rd] == 0);
    CHECK_EQ(freeCluster(b).freeRuns.size(), static_cast<std::size_t>(1));
    CHECK_EQ(freeCluster(b).freeRuns[0][0].comp, rd);
}

// ---------------------------------------------------------------------------
// Determinism: the same circuit, declared backwards.
// ---------------------------------------------------------------------------

namespace {

// A designator-keyed rendering of the whole plan. Collections whose order
// legitimately tracks room or net-pin order (shunts of one junction, decap
// members, loose lists, free runs) are canonicalised by sorting; everything
// the walk itself orders (clusters, arteries, steps) is kept exact.
std::vector<std::string> planKey(const Design& d, const Built& b) {
    auto desig = [&](std::uint32_t c) { return d.components[c].designator; };
    auto net = [&](std::int32_t n) {
        return n < 0 ? std::string("-") : d.nets[static_cast<std::size_t>(n)].name;
    };
    std::vector<std::string> out;
    for (const Cluster& cl : b.plan.clusters) {
        std::string head = "cluster ";
        if (cl.anchorVert < 0) {
            head += "FREE";
        } else {
            head += desig(b.flow.verts[static_cast<std::size_t>(cl.anchorVert)].comps[0]);
        }
        out.push_back(head);
        for (const std::vector<Artery>* side : {&cl.left, &cl.right}) {
            const char* tag = side == &cl.left ? "  L " : "  R ";
            for (const Artery& a : *side) {
                std::string line = tag + net(a.startNet) + " ->";
                for (const ArteryStep& s : a.steps) {
                    if (s.kind == ArteryStep::Kind::Inline) {
                        line += " " + desig(s.elem.comp);
                        continue;
                    }
                    line += " J(" + net(s.net);
                    std::vector<std::string> shs;
                    for (const Shunt& sh : s.shunts) {
                        std::string ts;
                        for (const ChainElem& e : sh.elems) ts += desig(e.comp) + ".";
                        ts += net(sh.endNet) + (sh.up ? "^" : "v");
                        shs.push_back(ts);
                    }
                    std::sort(shs.begin(), shs.end());
                    for (const std::string& ts : shs) line += " " + ts;
                    line += ")";
                }
                line += " => " + net(a.endNet) + (a.namedEnd ? " named" : "");
                out.push_back(line);
            }
        }
        std::vector<std::string> rest;
        for (const DecapGroup& g : cl.decaps) {
            std::string line = "  decap " + net(g.rail);
            std::vector<std::string> cs;
            for (std::uint32_t c : g.comps) cs.push_back(desig(c));
            std::sort(cs.begin(), cs.end());
            for (const std::string& s : cs) line += " " + s;
            rest.push_back(line);
        }
        auto verts = [&](const char* tag, const std::vector<std::uint32_t>& vs) {
            for (std::uint32_t v : vs) {
                rest.push_back(std::string("  ") + tag + " " +
                               desig(b.flow.verts[v].comps[0]));
            }
        };
        verts("up", cl.satUps);
        verts("down", cl.satDowns);
        verts("loose", cl.looseVerts);
        for (const std::vector<ChainElem>& run : cl.freeRuns) {
            std::string line = "  run";
            for (const ChainElem& e : run) line += " " + desig(e.comp);
            rest.push_back(line);
        }
        std::sort(rest.begin(), rest.end());
        out.insert(out.end(), rest.begin(), rest.end());
    }
    for (std::size_t ni = 0; ni < b.plan.netState.size(); ++ni) {
        const char* st = b.plan.netState[ni] == NetState::Free
                             ? "free"
                             : b.plan.netState[ni] == NetState::Drawn ? "drawn" : "named";
        out.push_back("net " + d.nets[ni].name + " " + st);
    }
    return out;
}

}  // namespace

TEST_CASE("permuting declaration order yields the same plan keyed by designator") {
    Buck fwd = makeBuck(false);
    Buck rev = makeBuck(true);
    RenderModel mf = buildRenderModel(fwd.b.d);
    RenderModel mr = buildRenderModel(rev.b.d);
    Built bf = build(mf);
    Built br = build(mr);
    checkInvariants(bf, fwd.b.d, mf.pages[0].rooms[0]);
    checkInvariants(br, rev.b.d, mr.pages[0].rooms[0]);

    std::vector<std::string> kf = planKey(fwd.b.d, bf);
    std::vector<std::string> kr = planKey(rev.b.d, br);
    CHECK_EQ(kf.size(), kr.size());
    for (std::size_t i = 0; i < kf.size() && i < kr.size(); ++i) {
        CHECK_EQ(kf[i], kr[i]);
    }
}

TEST_MAIN()
