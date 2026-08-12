// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The flow graph (WP3): vertex formation, series collapse, ranking from
// sources, the '&EDGE' overrides, in-rank determinism, and the inter-room
// traffic counts. Designs are built directly -- the netlist structs are the
// contract -- and run through buildRenderModel so the page's net marks and
// room partition are the real ones.
#include <string>
#include <vector>

#include "harness.h"
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

int rankOf(const RoomFlow& f, std::uint32_t c) {
    return f.verts[static_cast<std::size_t>(f.vertexOf[c])].rank;
}

FlowVertex::Kind kindOf(const RoomFlow& f, std::uint32_t c) {
    return f.verts[static_cast<std::size_t>(f.vertexOf[c])].kind;
}

// The header's totality guarantee: every room component in exactly one
// vertex, every vertexOf entry valid, ranks a partition of the vertices with
// rank/order fields agreeing with the lists.
void checkTotality(const RoomFlow& f, const Design& d, const RenderRoom& room) {
    std::vector<int> seen(d.components.size(), 0);
    for (const FlowVertex& v : f.verts) {
        CHECK(!v.comps.empty());
        if (v.kind == FlowVertex::Kind::Child) continue;
        for (std::uint32_t c : v.comps) ++seen[c];
    }
    std::vector<int> inRoom(d.components.size(), 0);
    for (std::uint32_t c : room.components) inRoom[c] = 1;
    for (std::size_t c = 0; c < d.components.size(); ++c) {
        CHECK_EQ(seen[c], inRoom[c]);
        if (inRoom[c]) {
            std::int32_t v = f.vertexOf[c];
            CHECK(v >= 0 && static_cast<std::size_t>(v) < f.verts.size());
            bool contains = false;
            for (std::uint32_t vc : f.verts[static_cast<std::size_t>(v)].comps) {
                if (vc == c) contains = true;
            }
            CHECK(contains);
        } else {
            CHECK_EQ(f.vertexOf[c], -1);
        }
    }
    std::size_t total = 0;
    for (std::size_t r = 0; r < f.ranks.size(); ++r) {
        for (std::size_t i = 0; i < f.ranks[r].size(); ++i) {
            std::uint32_t v = f.ranks[r][i];
            CHECK(static_cast<std::size_t>(f.verts[v].rank) == r);
            CHECK(static_cast<std::size_t>(f.verts[v].order) == i);
        }
        total += f.ranks[r].size();
    }
    CHECK_EQ(total, f.verts.size());
}

// The reference room: connector -> series R -> IC hub -> LED string to
// ground, with a pull-up, a decoupling ladder, and a pull-down. `reversed`
// permutes the component declaration order (and with it every net's pin
// order) without changing the circuit.
struct Ladder {
    TB b;
    std::uint32_t j1 = 0, r1 = 0, u1 = 0, r3 = 0, c1 = 0, d1 = 0, r2 = 0;
    std::int32_t nA = 0, nB = 0, nC = 0, nD = 0, rail = 0, gnd = 0;
};

Ladder makeLadder(bool reversed) {
    Ladder l;
    l.nA = l.b.net("A");
    l.nB = l.b.net("B");
    l.nC = l.b.net("C");
    l.nD = l.b.net("D");
    l.rail = l.b.net("3V3");
    l.gnd = l.b.net("GND", true);
    auto declare = [&](int which) {
        switch (which) {
            case 0:
                l.j1 = l.b.comp("J1", "", {"1", "2"}, "", PartType::BoardConnector);
                l.b.wire(l.j1, 0, l.nA);
                break;
            case 1:
                l.r1 = l.b.comp("R1", "resistor", {"1", "2"});
                l.b.wire(l.r1, 0, l.nA);
                l.b.wire(l.r1, 1, l.nB);
                break;
            case 2:
                l.u1 = l.b.comp("U1", "", {"IN", "OUT", "VCC", "P4", "P5"});
                l.b.wire(l.u1, 0, l.nB, PortDir::In);
                l.b.wire(l.u1, 1, l.nC, PortDir::Out);
                l.b.wire(l.u1, 2, l.rail);
                break;
            case 3:
                l.r3 = l.b.comp("R3", "resistor", {"1", "2"});
                l.b.wire(l.r3, 0, l.rail);
                l.b.wire(l.r3, 1, l.nB);
                break;
            case 4:
                l.c1 = l.b.comp("C1", "capacitor", {"1", "2"});
                l.b.wire(l.c1, 0, l.rail);
                l.b.wire(l.c1, 1, l.gnd);
                break;
            case 5:
                l.d1 = l.b.comp("D1", "led", {"A", "K"});
                l.b.wire(l.d1, 0, l.nC, PortDir::In);
                l.b.wire(l.d1, 1, l.nD);
                break;
            case 6:
                l.r2 = l.b.comp("R2", "resistor", {"1", "2"});
                l.b.wire(l.r2, 0, l.nD);
                l.b.wire(l.r2, 1, l.gnd);
                break;
        }
    };
    if (reversed) {
        for (int i = 6; i >= 0; --i) declare(i);
    } else {
        for (int i = 0; i <= 6; ++i) declare(i);
    }
    return l;
}

std::string joinKeys(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}

// One line per rank: the vertices in order, each named by its first
// component's designator (comparable across declaration permutations).
std::vector<std::string> ranksByDesignator(const RoomFlow& f, const Design& d) {
    std::vector<std::string> out;
    for (const std::vector<std::uint32_t>& layer : f.ranks) {
        std::vector<std::string> keys;
        for (std::uint32_t v : layer) {
            keys.push_back(d.components[f.verts[v].comps[0]].designator);
        }
        out.push_back(joinKeys(keys));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Vertex formation and ranking on the reference room.
// ---------------------------------------------------------------------------

TEST_CASE("source to hub to sink: ranks increase along the signal path") {
    Ladder l = makeLadder(false);
    RenderModel m = buildRenderModel(l.b.d);
    CHECK_EQ(m.pages.size(), static_cast<std::size_t>(1));
    const RenderRoom& room = m.pages[0].rooms[0];
    RoomFlow f = buildRoomFlow(m.pages[0], room, m);

    checkTotality(f, l.b.d, room);
    CHECK_EQ(f.verts.size(), static_cast<std::size_t>(7));

    CHECK(kindOf(f, l.j1) == FlowVertex::Kind::Anchor);
    CHECK(kindOf(f, l.r1) == FlowVertex::Kind::Series);
    CHECK(kindOf(f, l.u1) == FlowVertex::Kind::Anchor);
    CHECK(kindOf(f, l.r3) == FlowVertex::Kind::Vertical);
    CHECK(kindOf(f, l.c1) == FlowVertex::Kind::Vertical);
    CHECK(kindOf(f, l.d1) == FlowVertex::Kind::Series);
    CHECK(kindOf(f, l.r2) == FlowVertex::Kind::Vertical);

    // The signal path ranks strictly increase, connector first.
    CHECK_EQ(rankOf(f, l.j1), 0);
    CHECK(rankOf(f, l.j1) < rankOf(f, l.r1));
    CHECK(rankOf(f, l.r1) < rankOf(f, l.u1));
    CHECK(rankOf(f, l.u1) < rankOf(f, l.d1));

    // Verticals rank beside what they serve: the pull-up and the ladder with
    // the hub, the pull-down with the LED string it terminates.
    CHECK_EQ(rankOf(f, l.r3), rankOf(f, l.u1));
    CHECK_EQ(rankOf(f, l.c1), rankOf(f, l.u1));
    CHECK_EQ(rankOf(f, l.r2), rankOf(f, l.d1));

    // Rails and grounds are ambient, listed apart, in first-touch order.
    CHECK_EQ(f.roomRails.size(), static_cast<std::size_t>(1));
    CHECK_EQ(f.roomRails[0], l.rail);
    CHECK_EQ(f.roomGrounds.size(), static_cast<std::size_t>(1));
    CHECK_EQ(f.roomGrounds[0], l.gnd);
}

// ---------------------------------------------------------------------------
// Series collapse.
// ---------------------------------------------------------------------------

TEST_CASE("a private net collapses a two-terminal string into one Series vertex") {
    TB b;
    // Declared C1 before R1: the walk must still find the string's head.
    std::uint32_t c1 = b.comp("C1", "capacitor", {"1", "2"});
    std::uint32_t r1 = b.comp("R1", "resistor", {"1", "2"});
    std::int32_t n1 = b.net("N1"), mid = b.net("MID"), n2 = b.net("N2");
    b.wire(r1, 0, n1);
    b.wire(r1, 1, mid);
    b.wire(c1, 0, mid);
    b.wire(c1, 1, n2);

    RenderModel m = buildRenderModel(b.d);
    const RenderRoom& room = m.pages[0].rooms[0];
    RoomFlow f = buildRoomFlow(m.pages[0], room, m);

    checkTotality(f, b.d, room);
    CHECK_EQ(f.verts.size(), static_cast<std::size_t>(1));
    CHECK(f.verts[0].kind == FlowVertex::Kind::Series);
    // comps in walk order, head first: R1 then C1, whatever the declaration.
    CHECK_EQ(f.verts[0].comps.size(), static_cast<std::size_t>(2));
    CHECK_EQ(f.verts[0].comps[0], r1);
    CHECK_EQ(f.verts[0].comps[1], c1);
    CHECK_EQ(f.vertexOf[r1], f.vertexOf[c1]);
}

TEST_CASE("a net with three pins does not link a series") {
    TB b;
    std::uint32_t c1 = b.comp("C1", "capacitor", {"1", "2"});
    std::uint32_t r1 = b.comp("R1", "resistor", {"1", "2"});
    std::uint32_t r4 = b.comp("R4", "resistor", {"1", "2"});
    std::int32_t n1 = b.net("N1"), mid = b.net("MID"), n2 = b.net("N2"), n3 = b.net("N3");
    b.wire(r1, 0, n1);
    b.wire(r1, 1, mid);
    b.wire(c1, 0, mid);
    b.wire(c1, 1, n2);
    b.wire(r4, 0, mid);  // the third pin: MID is no longer private
    b.wire(r4, 1, n3);

    RenderModel m = buildRenderModel(b.d);
    const RenderRoom& room = m.pages[0].rooms[0];
    RoomFlow f = buildRoomFlow(m.pages[0], room, m);

    checkTotality(f, b.d, room);
    CHECK_EQ(f.verts.size(), static_cast<std::size_t>(3));
    for (const FlowVertex& v : f.verts) {
        CHECK(v.kind == FlowVertex::Kind::Series);
        CHECK_EQ(v.comps.size(), static_cast<std::size_t>(1));
    }
    CHECK(f.vertexOf[r1] != f.vertexOf[c1]);
    CHECK(f.vertexOf[r1] != f.vertexOf[r4]);
}

// ---------------------------------------------------------------------------
// '&EDGE' overrides.
// ---------------------------------------------------------------------------

namespace {

// J1 -> U1 -> J2; `j2edge` is the '&EDGE' directive on the far connector.
RoomFlow edgeFlow(TB& b, const std::string& j2edge, std::uint32_t& j1, std::uint32_t& u1,
                  std::uint32_t& j2, RenderModel& m) {
    j1 = b.comp("J1", "", {"1"}, "", PartType::BoardConnector);
    u1 = b.comp("U1", "", {"IN", "OUT", "P3", "P4", "P5"});
    j2 = b.comp("J2", "", {"1"}, j2edge, PartType::BoardConnector);
    std::int32_t nA = b.net("A"), nB = b.net("B");
    b.wire(j1, 0, nA);
    b.wire(u1, 0, nA, PortDir::In);
    b.wire(u1, 1, nB, PortDir::Out);
    b.wire(j2, 0, nB, PortDir::In);
    m = buildRenderModel(b.d);
    return buildRoomFlow(m.pages[0], m.pages[0].rooms[0], m);
}

}  // namespace

TEST_CASE("a connector is a source at rank 0 by default") {
    TB b;
    std::uint32_t j1 = 0, u1 = 0, j2 = 0;
    RenderModel m;
    RoomFlow f = edgeFlow(b, "", j1, u1, j2, m);
    checkTotality(f, b.d, m.pages[0].rooms[0]);
    CHECK_EQ(rankOf(f, j1), 0);
    CHECK_EQ(rankOf(f, j2), 0);  // both connectors face in
    CHECK_EQ(rankOf(f, u1), 1);
}

TEST_CASE("&EDGE=RIGHT flips a connector from rank 0 to max rank") {
    TB b;
    std::uint32_t j1 = 0, u1 = 0, j2 = 0;
    RenderModel m;
    RoomFlow f = edgeFlow(b, "RIGHT", j1, u1, j2, m);
    checkTotality(f, b.d, m.pages[0].rooms[0]);
    CHECK_EQ(rankOf(f, j1), 0);
    CHECK_EQ(rankOf(f, u1), 1);
    CHECK_EQ(rankOf(f, j2), 2);
    CHECK_EQ(f.ranks.size(), static_cast<std::size_t>(3));
    CHECK_EQ(static_cast<std::size_t>(rankOf(f, j2)), f.ranks.size() - 1);
}

// ---------------------------------------------------------------------------
// Determinism: the same circuit, declared backwards.
// ---------------------------------------------------------------------------

TEST_CASE("permuting declaration order changes neither ranks nor in-rank order") {
    Ladder fwd = makeLadder(false);
    Ladder rev = makeLadder(true);
    RenderModel mf = buildRenderModel(fwd.b.d);
    RenderModel mr = buildRenderModel(rev.b.d);
    RoomFlow ff = buildRoomFlow(mf.pages[0], mf.pages[0].rooms[0], mf);
    RoomFlow fr = buildRoomFlow(mr.pages[0], mr.pages[0].rooms[0], mr);

    // Identical designator -> rank map.
    auto rk = [](const Ladder& l, const RoomFlow& f) {
        return std::vector<int>{rankOf(f, l.j1), rankOf(f, l.r1), rankOf(f, l.u1),
                                rankOf(f, l.r3), rankOf(f, l.c1), rankOf(f, l.d1),
                                rankOf(f, l.r2)};
    };
    CHECK(rk(fwd, ff) == rk(rev, fr));

    // Identical rank structure and in-rank order, keyed by designator.
    std::vector<std::string> sf = ranksByDesignator(ff, fwd.b.d);
    std::vector<std::string> sr = ranksByDesignator(fr, rev.b.d);
    CHECK_EQ(sf.size(), sr.size());
    for (std::size_t r = 0; r < sf.size() && r < sr.size(); ++r) {
        CHECK_EQ(sf[r], sr[r]);
    }
}

// ---------------------------------------------------------------------------
// Loose parts and totality.
// ---------------------------------------------------------------------------

TEST_CASE("test points and wires are Loose; totality holds with them present") {
    Ladder l = makeLadder(false);
    std::uint32_t tp = l.b.comp("TP1", "testpoint", {"TP"});
    l.b.wire(tp, 0, l.nB);
    std::uint32_t w1 = l.b.comp("W1", "wire", {"1", "2"});  // unwired conductor

    RenderModel m = buildRenderModel(l.b.d);
    const RenderRoom& room = m.pages[0].rooms[0];
    RoomFlow f = buildRoomFlow(m.pages[0], room, m);

    checkTotality(f, l.b.d, room);
    CHECK_EQ(f.verts.size(), static_cast<std::size_t>(9));
    CHECK(kindOf(f, tp) == FlowVertex::Kind::Loose);
    CHECK(kindOf(f, w1) == FlowVertex::Kind::Loose);
}

// ---------------------------------------------------------------------------
// Child block instances.
// ---------------------------------------------------------------------------

TEST_CASE("a child block is a Child vertex; an empty room yields an empty flow") {
    TB b;
    std::uint32_t u1 = b.comp("U1", "", {"IN", "P2", "P3", "P4", "P5"});
    b.d.components[u1].path = {"U1"};
    std::int32_t nX = b.net("X");
    b.wire(u1, 0, nX, PortDir::In);
    BlockInstance bi;
    bi.path = {"BLK1"};
    bi.block = "child";
    BlockPort port;
    port.name = "OUT";
    port.direction = PortDir::Out;
    port.net = nX;
    bi.ports.push_back(port);
    b.d.blocks.push_back(bi);
    b.d.top = "top";

    RenderModel m = buildRenderModel(b.d);
    CHECK_EQ(m.pages.size(), static_cast<std::size_t>(2));

    const RenderRoom& room = m.pages[0].rooms[0];
    RoomFlow f = buildRoomFlow(m.pages[0], room, m);
    checkTotality(f, b.d, room);
    CHECK_EQ(f.verts.size(), static_cast<std::size_t>(2));
    CHECK(f.verts[0].kind == FlowVertex::Kind::Anchor);
    CHECK(f.verts[1].kind == FlowVertex::Kind::Child);
    CHECK_EQ(f.verts[1].comps.size(), static_cast<std::size_t>(1));
    CHECK_EQ(f.verts[1].comps[0], static_cast<std::uint32_t>(0));  // Design::blocks index
    CHECK_EQ(f.vertexOf[u1], 0);

    // The definition page's room holds nothing of this design: legal, empty.
    const RenderRoom& empty = m.pages[1].rooms[0];
    RoomFlow g = buildRoomFlow(m.pages[1], empty, m);
    CHECK(g.verts.empty());
    CHECK(g.ranks.empty());
    for (std::int32_t v : g.vertexOf) CHECK_EQ(v, -1);
}

// ---------------------------------------------------------------------------
// Inter-room flow.
// ---------------------------------------------------------------------------

TEST_CASE("inter-room counts share signal nets only; pull follows the edges") {
    TB b;
    std::uint32_t j1 = b.comp("J1", "", {"1"}, "LEFT", PartType::BoardConnector, "input");
    std::uint32_t u1 = b.comp("U1", "", {"P1", "P2", "P3", "P4", "P5"}, "", PartType::BoardPart,
                              "input");
    std::uint32_t u2 = b.comp("U2", "", {"P1", "P2", "P3", "P4", "P5"}, "", PartType::BoardPart,
                              "logic");
    b.comp("J2", "", {"1"}, "", PartType::BoardConnector, "logic");
    b.comp("J3", "", {"1"}, "", PartType::BoardConnector, "logic");
    b.comp("J4", "", {"1"}, "RIGHT", PartType::BoardConnector, "output");
    std::int32_t sig1 = b.net("SIG1"), sig2 = b.net("SIG2"), rail = b.net("3V3");
    b.wire(u1, 0, sig1);
    b.wire(u2, 0, sig1);
    b.wire(u1, 1, sig2);
    b.wire(u2, 1, sig2);
    b.wire(u1, 2, rail);
    b.wire(u2, 2, rail);
    b.wire(j1, 0, sig1);

    RenderModel m = buildRenderModel(b.d);
    const RenderPage& page = m.pages[0];
    CHECK_EQ(page.rooms.size(), static_cast<std::size_t>(3));  // input, logic, output

    InterRoomFlow f = buildInterRoomFlow(page, m);
    CHECK_EQ(f.counts.size(), static_cast<std::size_t>(3));
    for (std::size_t r = 0; r < 3; ++r) {
        CHECK_EQ(f.counts[r].size(), static_cast<std::size_t>(3));
        CHECK_EQ(f.counts[r][r], 0u);
    }
    // Two signal nets shared, the rail not counted: 2, not 3.
    CHECK_EQ(f.counts[0][1], 2u);
    CHECK_EQ(f.counts[1][0], 2u);
    CHECK_EQ(f.counts[0][2], 0u);
    CHECK_EQ(f.counts[1][2], 0u);

    // input: one LEFT-edge connector. logic: two connectors facing in by
    // default. output: one RIGHT-edge connector.
    CHECK_EQ(f.pull.size(), static_cast<std::size_t>(3));
    CHECK_EQ(f.pull[0], -1);
    CHECK_EQ(f.pull[1], -2);
    CHECK_EQ(f.pull[2], 1);
}

TEST_MAIN()
