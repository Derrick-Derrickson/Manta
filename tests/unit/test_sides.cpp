// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Flow-aware pin sides (WP2): fixed sides, partner facing, slot order, the
// balance rule, the '&EDGE' connector flip, showNet, and determinism under
// permutation of Component::pins.
#include <algorithm>
#include <string>
#include <vector>

#include "harness.h"
#include "render/sides.h"
#include "render/symbols.h"

using namespace manta;
using namespace manta::render;

namespace {

// A design under construction plus everything planSides reads. RenderModel
// carries a pointer to the Design, so a World must stay put once built.
struct World {
    Design d;
    RenderPage page;
    RoomFlow flow;
    RenderModel m;

    std::uint32_t comp(std::string des, SymbolKind kind) {
        Component c;
        c.designator = des;
        c.partName = des;
        d.components.push_back(std::move(c));
        m.kinds.push_back(kind);
        return static_cast<std::uint32_t>(d.components.size() - 1);
    }

    void pin(std::uint32_t ci, std::string phys, std::string logical,
             PinType t = PinType::Passive) {
        ComponentPin p;
        p.physical = std::move(phys);
        p.logical = std::move(logical);
        p.type = t;
        d.components[ci].pins.push_back(std::move(p));
    }

    // Wires the named pins of the given components onto one net; the page's
    // view of it gets the same spelling as its display name.
    std::int32_t net(std::string name, std::vector<std::pair<std::uint32_t, std::string>> ends,
                     NetMark mark = NetMark::Label) {
        std::int32_t idx = static_cast<std::int32_t>(d.nets.size());
        Net n;
        n.name = name;
        for (auto& [ci, logical] : ends) {
            Component& c = d.components[ci];
            for (std::uint32_t pi = 0; pi < c.pins.size(); ++pi) {
                if (c.pins[pi].logical == logical) {
                    n.pins.push_back(PinRef{ci, pi});
                    c.pins[pi].net = idx;
                }
            }
        }
        d.nets.push_back(std::move(n));
        RenderNet rn;
        rn.display = std::move(name);
        rn.mark = mark;
        page.nets.push_back(std::move(rn));
        return idx;
    }

    // One Anchor vertex per component, at the given (rank, order); rank -1
    // means "no vertex" (a component outside the room).
    void rank(std::vector<std::pair<int, int>> rankOrder) {
        flow = RoomFlow{};
        flow.vertexOf.assign(d.components.size(), -1);
        for (std::uint32_t ci = 0; ci < rankOrder.size(); ++ci) {
            auto [r, o] = rankOrder[ci];
            if (r < 0) continue;
            FlowVertex v;
            v.kind = FlowVertex::Kind::Anchor;
            v.comps.push_back(ci);
            v.rank = r;
            v.order = o;
            flow.vertexOf[ci] = static_cast<std::int32_t>(flow.verts.size());
            if (static_cast<std::size_t>(r) >= flow.ranks.size()) {
                flow.ranks.resize(static_cast<std::size_t>(r) + 1);
            }
            flow.ranks[static_cast<std::size_t>(r)].push_back(
                static_cast<std::uint32_t>(flow.verts.size()));
            flow.verts.push_back(std::move(v));
        }
    }

    SidePlan plan(std::uint32_t ci) {
        m.design = &d;
        return planSides(ci, flow, page, m);
    }
};

// The plan entry for the pin with the given logical name.
const PinPlan& entry(const World& w, std::uint32_t ci, const SidePlan& plan,
                     std::string_view logical) {
    const Component& c = w.d.components[ci];
    for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
        if (c.pins[i].logical == logical) return plan.byPin[i];
    }
    static PinPlan none{};
    CHECK(false);  // no such pin
    return none;
}

}  // namespace

// ---------------------------------------------------------------------------
// Partner facing and slot order
// ---------------------------------------------------------------------------

TEST_CASE("a UART pair sharing one partner connector lands adjacent on one side") {
    World w;
    std::uint32_t mcu = w.comp("U1", SymbolKind::Generic);
    w.pin(mcu, "10", "PA9");
    w.pin(mcu, "11", "PA10");
    w.pin(mcu, "12", "PB0");
    std::uint32_t j1 = w.comp("J1", SymbolKind::Connector);
    w.pin(j1, "1", "1");
    w.pin(j1, "2", "2");
    std::uint32_t j2 = w.comp("J2", SymbolKind::Connector);
    w.pin(j2, "1", "1");
    w.net("USART1-TX", {{mcu, "PA9"}, {j1, "1"}});
    w.net("USART1-RX", {{mcu, "PA10"}, {j1, "2"}});
    w.net("LED", {{mcu, "PB0"}, {j2, "1"}});
    // MCU at rank 0; both connectors downstream at rank 1, J1 before J2.
    w.rank({{0, 0}, {1, 0}, {1, 1}});

    SidePlan p = w.plan(mcu);
    const PinPlan& tx = entry(w, mcu, p, "PA9");
    const PinPlan& rx = entry(w, mcu, p, "PA10");
    const PinPlan& led = entry(w, mcu, p, "PB0");
    CHECK(tx.side == Side::Right);
    CHECK(rx.side == Side::Right);
    // Same partner vertex, so adjacent; PA9 before PA10 by natural name order.
    CHECK_EQ(rx.slot, tx.slot + 1);
    // J2 has a higher order within rank 1, so PB0 sorts after the pair.
    CHECK(led.side == Side::Right);
    CHECK_EQ(led.slot, rx.slot + 1);
    // Silicon names differ from the label nets, so both annotate.
    CHECK(tx.showNet);
    CHECK(rx.showNet);
}

TEST_CASE("partner rank left/right split around the component's own rank") {
    World w;
    std::uint32_t mid = w.comp("U1", SymbolKind::Generic);
    w.pin(mid, "1", "IN");
    w.pin(mid, "2", "OUT");
    std::uint32_t src = w.comp("U2", SymbolKind::Generic);
    w.pin(src, "1", "O");
    std::uint32_t dst = w.comp("U3", SymbolKind::Generic);
    w.pin(dst, "1", "I");
    w.net("A", {{mid, "IN"}, {src, "O"}});
    w.net("B", {{mid, "OUT"}, {dst, "I"}});
    w.rank({{1, 0}, {0, 0}, {2, 0}});

    SidePlan p = w.plan(mid);
    CHECK(entry(w, mid, p, "IN").side == Side::Left);
    CHECK(entry(w, mid, p, "OUT").side == Side::Right);
}

// ---------------------------------------------------------------------------
// Fixed sides
// ---------------------------------------------------------------------------

TEST_CASE("power tops, ground bottoms, dead pins end the right column contiguously") {
    World w;
    std::uint32_t u = w.comp("U1", SymbolKind::Generic);
    w.pin(u, "1", "VCC", PinType::Power);
    w.pin(u, "2", "GND", PinType::Ground);
    w.pin(u, "3", "IO");
    w.pin(u, "4", "SPARE", PinType::NC);
    w.pin(u, "5", "FREE");  // never wired: net stays -1
    std::uint32_t x = w.comp("U2", SymbolKind::Generic);
    w.pin(x, "1", "I");
    w.net("VDD", {{u, "VCC"}});
    w.net("GNDNET", {{u, "GND"}});
    w.net("SIG", {{u, "IO"}, {x, "I"}});
    w.rank({{0, 0}, {1, 0}});

    SidePlan p = w.plan(u);
    CHECK(entry(w, u, p, "VCC").side == Side::Top);
    CHECK(entry(w, u, p, "GND").side == Side::Bottom);
    const PinPlan& io = entry(w, u, p, "IO");
    const PinPlan& spare = entry(w, u, p, "SPARE");
    const PinPlan& free = entry(w, u, p, "FREE");
    CHECK(io.side == Side::Right);
    CHECK_EQ(io.slot, 0);
    // The dead block: after every live right pin, contiguous, in natural
    // pin-number order, never annotated.
    CHECK(spare.side == Side::Right);
    CHECK(free.side == Side::Right);
    CHECK_EQ(spare.slot, 1);
    CHECK_EQ(free.slot, 2);
    CHECK_FALSE(spare.showNet);
    CHECK_FALSE(free.showNet);
}

TEST_CASE("a passive pin on a rail-marked net goes top, on a ground-marked net bottom") {
    World w;
    std::uint32_t u = w.comp("U1", SymbolKind::Generic);
    w.pin(u, "1", "EN");
    w.pin(u, "2", "PAD");
    w.net("3V3", {{u, "EN"}}, NetMark::Rail);
    w.net("GND", {{u, "PAD"}}, NetMark::Ground);
    w.rank({{0, 0}});

    SidePlan p = w.plan(u);
    const PinPlan& en = entry(w, u, p, "EN");
    CHECK(en.side == Side::Top);
    CHECK_FALSE(en.showNet);  // the rail mark already says the name
    CHECK(entry(w, u, p, "PAD").side == Side::Bottom);
}

// ---------------------------------------------------------------------------
// The balance rule
// ---------------------------------------------------------------------------

TEST_CASE("partnerless pins balance left/right in name order, ties to the left") {
    World w;
    std::uint32_t u = w.comp("U1", SymbolKind::Generic);
    w.pin(u, "1", "A");
    w.pin(u, "2", "B");
    w.pin(u, "3", "C");
    w.net("NA", {{u, "A"}});
    w.net("NB", {{u, "B"}});
    w.net("NC-NET", {{u, "C"}});
    w.rank({{0, 0}});

    SidePlan p = w.plan(u);
    // Taken in name order A, B, C: 0<=0 -> Left, 1>0 -> Right, 1<=1 -> Left.
    CHECK(entry(w, u, p, "A").side == Side::Left);
    CHECK(entry(w, u, p, "B").side == Side::Right);
    CHECK(entry(w, u, p, "C").side == Side::Left);
}

TEST_CASE("an empty-flow room (the WP3 stub shape) degrades to a balanced box") {
    World w;
    std::uint32_t u = w.comp("U1", SymbolKind::Generic);
    w.pin(u, "1", "P1");
    w.pin(u, "2", "P2");
    w.pin(u, "3", "P3");
    w.pin(u, "4", "P4");
    std::uint32_t a = w.comp("U2", SymbolKind::Generic);
    w.pin(a, "1", "X");
    std::uint32_t b = w.comp("U3", SymbolKind::Generic);
    w.pin(b, "1", "Y");
    w.net("N1", {{u, "P1"}, {a, "X"}});
    w.net("N2", {{u, "P2"}, {b, "Y"}});
    w.net("N3", {{u, "P3"}});
    w.net("N4", {{u, "P4"}});
    // The stub's shape: everyone at rank 0, order = room position.
    w.rank({{0, 0}, {0, 1}, {0, 2}});

    SidePlan p1 = w.plan(u);
    SidePlan p2 = w.plan(u);
    CHECK(p1.byPin.size() == 4);
    int nLeft = 0, nRight = 0;
    for (const PinPlan& pp : p1.byPin) {
        CHECK(pp.side == Side::Left || pp.side == Side::Right);
        (pp.side == Side::Left ? nLeft : nRight) += 1;
    }
    CHECK_EQ(nLeft, 2);
    CHECK_EQ(nRight, 2);
    // Same inputs, same plan: deterministic.
    for (std::size_t i = 0; i < p1.byPin.size(); ++i) {
        CHECK(p1.byPin[i].side == p2.byPin[i].side);
        CHECK_EQ(p1.byPin[i].slot, p2.byPin[i].slot);
        CHECK(p1.byPin[i].showNet == p2.byPin[i].showNet);
    }
}

// ---------------------------------------------------------------------------
// Connectors
// ---------------------------------------------------------------------------

TEST_CASE("a connector keeps one right-hand column in natural numeric order") {
    World w;
    std::uint32_t j = w.comp("J1", SymbolKind::Connector);
    w.pin(j, "1", "1");
    w.pin(j, "2", "2");
    w.pin(j, "3", "3");
    std::uint32_t a = w.comp("U1", SymbolKind::Generic);
    w.pin(a, "1", "X");
    std::uint32_t b = w.comp("U2", SymbolKind::Generic);
    w.pin(b, "1", "Y");
    // Pin 3 wires to the earlier-ordered partner, pin 1 to the later one,
    // pin 2 to nobody: partner ranking must not reorder the column.
    w.net("N1", {{j, "3"}, {a, "X"}});
    w.net("N2", {{j, "1"}, {b, "Y"}});
    w.net("N3", {{j, "2"}});
    w.rank({{1, 0}, {0, 0}, {0, 1}});

    SidePlan p = w.plan(j);
    for (const PinPlan& pp : p.byPin) CHECK(pp.side == Side::Right);
    CHECK_EQ(entry(w, j, p, "1").slot, 0);
    CHECK_EQ(entry(w, j, p, "2").slot, 1);
    CHECK_EQ(entry(w, j, p, "3").slot, 2);
}

TEST_CASE("a connector's dead pins gather after every live pin, in numeric order") {
    World w;
    std::uint32_t j = w.comp("J1", SymbolKind::Connector);
    w.pin(j, "1", "1");
    w.pin(j, "2", "2", PinType::NC);  // typed dead
    w.pin(j, "3", "3");               // never wired: net stays -1
    w.pin(j, "10", "10");             // natural order: after 1, not after 2
    std::uint32_t u = w.comp("U1", SymbolKind::Generic);
    w.pin(u, "1", "X");
    w.pin(u, "2", "Y");
    w.net("N1", {{j, "1"}, {u, "X"}});
    w.net("N10", {{j, "10"}, {u, "Y"}});
    w.rank({{1, 0}, {0, 0}});

    SidePlan p = w.plan(j);
    for (const PinPlan& pp : p.byPin) CHECK(pp.side == Side::Right);
    CHECK_EQ(entry(w, j, p, "1").slot, 0);   // live, numeric
    CHECK_EQ(entry(w, j, p, "10").slot, 1);  // live: 10 after 1, naturally
    CHECK_EQ(entry(w, j, p, "2").slot, 2);   // dead block starts
    CHECK_EQ(entry(w, j, p, "3").slot, 3);
    CHECK_FALSE(entry(w, j, p, "2").showNet);
    CHECK_FALSE(entry(w, j, p, "3").showNet);
}

TEST_CASE("'&EDGE=RIGHT' and '&EDGE=BOTTOM' flip a connector's pins to the left") {
    for (const char* edge : {"RIGHT", "BOTTOM"}) {
        World w;
        std::uint32_t j = w.comp("J1", SymbolKind::Connector);
        w.d.components[j].edge = edge;
        w.pin(j, "1", "1");
        w.pin(j, "2", "2");
        w.net("N1", {{j, "1"}});
        w.rank({{0, 0}});
        SidePlan p = w.plan(j);
        for (const PinPlan& pp : p.byPin) CHECK(pp.side == Side::Left);
    }
    // LEFT (and unset) keep the column on the right.
    World w;
    std::uint32_t j = w.comp("J1", SymbolKind::Connector);
    w.d.components[j].edge = "LEFT";
    w.pin(j, "1", "1");
    w.net("N1", {{j, "1"}});
    w.rank({{0, 0}});
    SidePlan p = w.plan(j);
    CHECK(p.byPin[0].side == Side::Right);
}

// ---------------------------------------------------------------------------
// showNet
// ---------------------------------------------------------------------------

TEST_CASE("showNet: label nets whose display name differs from the silicon name") {
    World w;
    std::uint32_t u = w.comp("U1", SymbolKind::Generic);
    w.pin(u, "1", "PA9");
    w.pin(u, "2", "SCL");
    w.pin(u, "3", "TXD");
    std::uint32_t x = w.comp("U2", SymbolKind::Generic);
    w.pin(x, "1", "A");
    w.pin(x, "2", "B");
    w.pin(x, "3", "C");
    w.net("USART1-TX", {{u, "PA9"}, {x, "A"}});
    w.net("SCL", {{u, "SCL"}, {x, "B"}});  // display equals the silicon name
    std::int32_t port = w.net("HOST-TX", {{u, "TXD"}, {x, "C"}});
    w.page.nets[static_cast<std::size_t>(port)].direction = PortDir::Out;  // a port flag
    w.rank({{0, 0}, {1, 0}});

    SidePlan p = w.plan(u);
    CHECK(entry(w, u, p, "PA9").showNet);
    CHECK_FALSE(entry(w, u, p, "SCL").showNet);
    // A port net's flag already prints the name at the mark.
    CHECK_FALSE(entry(w, u, p, "TXD").showNet);
}

// ---------------------------------------------------------------------------
// Determinism under pin permutation
// ---------------------------------------------------------------------------

namespace {

// Builds the same MCU (pins laid down in the given order) in the same room
// and returns its plan keyed by logical pin name.
std::vector<std::pair<std::string, PinPlan>> planPermuted(const std::vector<int>& order) {
    struct Spec {
        const char* phys;
        const char* logical;
        PinType type;
    };
    const Spec pins[] = {
        {"1", "VCC", PinType::Power},   {"2", "GND", PinType::Ground},
        {"3", "PA9", PinType::Passive}, {"4", "PA10", PinType::Passive},
        {"5", "FLOAT1", PinType::Passive}, {"6", "FLOAT2", PinType::Passive},
        {"7", "SPARE", PinType::NC},
    };
    World w;
    std::uint32_t u = w.comp("U1", SymbolKind::Generic);
    for (int i : order) w.pin(u, pins[i].phys, pins[i].logical, pins[i].type);
    std::uint32_t j = w.comp("J1", SymbolKind::Connector);
    w.pin(j, "1", "1");
    w.pin(j, "2", "2");
    w.net("VDD", {{u, "VCC"}}, NetMark::Rail);
    w.net("GNDN", {{u, "GND"}}, NetMark::Ground);
    w.net("USART1-TX", {{u, "PA9"}, {j, "1"}});
    w.net("USART1-RX", {{u, "PA10"}, {j, "2"}});
    w.net("F1", {{u, "FLOAT1"}});
    w.net("F2", {{u, "FLOAT2"}});
    w.rank({{0, 0}, {1, 0}});
    SidePlan p = w.plan(u);
    std::vector<std::pair<std::string, PinPlan>> byName;
    for (std::size_t i = 0; i < p.byPin.size(); ++i) {
        byName.emplace_back(w.d.components[u].pins[i].logical, p.byPin[i]);
    }
    std::sort(byName.begin(), byName.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return byName;
}

}  // namespace

TEST_CASE("the plan agrees per pin name under any permutation of the pin list") {
    std::vector<int> forward{0, 1, 2, 3, 4, 5, 6};
    std::vector<int> reversed{6, 5, 4, 3, 2, 1, 0};
    std::vector<int> shuffled{3, 0, 6, 2, 5, 1, 4};
    auto a = planPermuted(forward);
    auto b = planPermuted(reversed);
    auto c = planPermuted(shuffled);
    CHECK_EQ(a.size(), b.size());
    CHECK_EQ(a.size(), c.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK_EQ(a[i].first, b[i].first);
        CHECK(a[i].second.side == b[i].second.side);
        CHECK_EQ(a[i].second.slot, b[i].second.slot);
        CHECK(a[i].second.showNet == b[i].second.showNet);
        CHECK_EQ(a[i].first, c[i].first);
        CHECK(a[i].second.side == c[i].second.side);
        CHECK_EQ(a[i].second.slot, c[i].second.slot);
        CHECK(a[i].second.showNet == c[i].second.showNet);
    }
}

// ---------------------------------------------------------------------------
// Scope: only box symbols are planned
// ---------------------------------------------------------------------------

TEST_CASE("classic symbols get an empty plan") {
    World w;
    std::uint32_t r = w.comp("R1", SymbolKind::Resistor);
    w.pin(r, "1", "A");
    w.pin(r, "2", "B");
    w.net("N", {{r, "A"}});
    w.rank({{0, 0}});
    CHECK(w.plan(r).byPin.empty());
}

// ---------------------------------------------------------------------------
// Geometry: the plan-consuming path in buildSymbol
// ---------------------------------------------------------------------------

TEST_CASE("buildSymbol places pins by the plan and annotates shown nets") {
    World w;
    std::uint32_t u = w.comp("U1", SymbolKind::Generic);
    w.pin(u, "1", "VCC", PinType::Power);
    w.pin(u, "2", "PA9");
    w.pin(u, "3", "PA10");
    std::uint32_t j = w.comp("J1", SymbolKind::Connector);
    w.pin(j, "1", "1");
    w.pin(j, "2", "2");
    w.net("VDD", {{u, "VCC"}}, NetMark::Rail);
    w.net("USART1-TX", {{u, "PA9"}, {j, "1"}});
    w.net("USART1-RX", {{u, "PA10"}, {j, "2"}});
    w.rank({{0, 0}, {1, 0}});
    SidePlan plan = w.plan(u);

    const Component& c = w.d.components[u];
    SymbolGeom g = buildSymbol(c, SymbolKind::Generic, &plan, &w.page);
    CHECK_EQ(g.pins.size(), c.pins.size());
    int lastRightOffset = -1;
    for (const SymPin& p : g.pins) {
        CHECK(p.side == plan.byPin[p.pin].side);
        if (p.name == "VCC") {
            CHECK(p.side == Side::Top);
            CHECK(p.netName.empty());
        }
        if (p.side == Side::Right) {
            CHECK(p.offset > lastRightOffset);  // placeColumn walks slots in order
            lastRightOffset = p.offset;
        }
        if (p.name == "PA9") CHECK_EQ(p.netName, "USART1-TX");
        if (p.name == "PA10") CHECK_EQ(p.netName, "USART1-RX");
    }

    // The shown net names widen the body beyond the plain planned box (same
    // plan, no page: nothing to show, nothing reserved).
    SymbolGeom bare = buildSymbol(c, SymbolKind::Generic, &plan, nullptr);
    for (const SymPin& p : bare.pins) CHECK(p.netName.empty());
    CHECK(g.w > bare.w);
    // The name row itself reserves the widest shown name: name column + gaps
    // + net column ("USART1-TX", 9 chars) all fit side by side.
    CHECK(g.w >= kCharWidth * 4 + 3 * kPinPitch + kCharWidth * 9 + kNetGap);

    // A null and an empty plan mean the builtin heuristic, identically.
    SidePlan empty;
    SymbolGeom h0 = buildSymbol(c, SymbolKind::Generic, nullptr, nullptr);
    SymbolGeom h1 = buildSymbol(c, SymbolKind::Generic, &empty, &w.page);
    CHECK_EQ(h0.w, h1.w);
    CHECK_EQ(h0.h, h1.h);
    CHECK_EQ(h0.pins.size(), h1.pins.size());
    for (std::size_t i = 0; i < h0.pins.size(); ++i) {
        CHECK(h0.pins[i].side == h1.pins[i].side);
        CHECK_EQ(h0.pins[i].offset, h1.pins[i].offset);
        CHECK_EQ(h0.pins[i].name, h1.pins[i].name);
        CHECK(h0.pins[i].netName.empty());
        CHECK(h1.pins[i].netName.empty());
    }
}

TEST_CASE("a planned connector keeps its geometry and can flip") {
    World w;
    std::uint32_t j = w.comp("J1", SymbolKind::Connector);
    w.d.components[j].edge = "RIGHT";
    w.pin(j, "1", "1");
    w.pin(j, "2", "2");
    w.net("N1", {{j, "1"}});
    w.net("N2", {{j, "2"}});
    w.rank({{0, 0}});
    SidePlan plan = w.plan(j);

    const Component& c = w.d.components[j];
    SymbolGeom g = buildSymbol(c, SymbolKind::Connector, &plan, &w.page);
    CHECK_EQ(g.pins.size(), c.pins.size());
    for (const SymPin& p : g.pins) CHECK(p.side == Side::Left);
    // The null path still builds today's right-hand column.
    SymbolGeom h = buildSymbol(c, SymbolKind::Connector, nullptr, nullptr);
    for (const SymPin& p : h.pins) CHECK(p.side == Side::Right);
    CHECK_EQ(g.h, h.h);
}

TEST_MAIN()
