// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Symbol classification: the three tiers ('@type', legacy '#type', the unit
// of '#value') and the pin-name gating that keeps a classic symbol honest.
#include <string>
#include <vector>

#include "harness.h"
#include "render/classify.h"

using namespace manta;
using namespace manta::render;

namespace {

Component part(std::string type, std::vector<std::string> pinNames) {
    Component c;
    c.designator = "X1";
    c.partName = "TEST";
    c.type = std::move(type);
    int n = 0;
    for (std::string& name : pinNames) {
        ComponentPin p;
        p.physical = std::to_string(++n);
        p.logical = std::move(name);
        c.pins.push_back(std::move(p));
    }
    return c;
}

Component twoPin(std::string type) { return part(std::move(type), {"A", "B"}); }

void addField(Component& c, std::string name, std::string value) {
    c.fields.emplace_back(std::move(name), std::move(value));
}

}  // namespace

// ---------------------------------------------------------------------------
// Tier 1: '@type'
// ---------------------------------------------------------------------------

TEST_CASE("every classification word maps to its symbol") {
    CHECK(classifySymbol(twoPin("resistor")) == SymbolKind::Resistor);
    CHECK(classifySymbol(twoPin("capacitor")) == SymbolKind::Capacitor);
    CHECK(classifySymbol(twoPin("capacitor-polarised")) == SymbolKind::CapacitorPolarised);
    CHECK(classifySymbol(twoPin("capacitor-polarized")) == SymbolKind::CapacitorPolarised);
    CHECK(classifySymbol(twoPin("electrolytic")) == SymbolKind::CapacitorPolarised);
    CHECK(classifySymbol(twoPin("tantalum")) == SymbolKind::CapacitorPolarised);
    CHECK(classifySymbol(twoPin("inductor")) == SymbolKind::Inductor);
    CHECK(classifySymbol(twoPin("ferrite")) == SymbolKind::Ferrite);
    CHECK(classifySymbol(twoPin("bead")) == SymbolKind::Ferrite);
    CHECK(classifySymbol(twoPin("ferrite-bead")) == SymbolKind::Ferrite);
    CHECK(classifySymbol(part("diode", {"A", "K"})) == SymbolKind::Diode);
    CHECK(classifySymbol(part("zener", {"A", "K"})) == SymbolKind::Zener);
    CHECK(classifySymbol(part("tvs", {"A", "K"})) == SymbolKind::Tvs);
    CHECK(classifySymbol(part("led", {"A", "K"})) == SymbolKind::Led);
    CHECK(classifySymbol(twoPin("crystal")) == SymbolKind::Crystal);
    CHECK(classifySymbol(twoPin("resonator")) == SymbolKind::Crystal);
    CHECK(classifySymbol(twoPin("oscillator")) == SymbolKind::Crystal);
    CHECK(classifySymbol(part("nmos", {"G", "D", "S"})) == SymbolKind::Nmos);
    CHECK(classifySymbol(part("pmos", {"G", "D", "S"})) == SymbolKind::Pmos);
    CHECK(classifySymbol(part("npn", {"B", "C", "E"})) == SymbolKind::Npn);
    CHECK(classifySymbol(part("pnp", {"B", "C", "E"})) == SymbolKind::Pnp);
    CHECK(classifySymbol(part("opamp", {"IN+", "IN-", "OUT"})) == SymbolKind::OpAmp);
    CHECK(classifySymbol(part("comparator", {"IN+", "IN-", "OUT"})) == SymbolKind::OpAmp);
    CHECK(classifySymbol(twoPin("switch")) == SymbolKind::Switch);
    CHECK(classifySymbol(twoPin("button")) == SymbolKind::Switch);
    CHECK(classifySymbol(twoPin("fuse")) == SymbolKind::Fuse);
    CHECK(classifySymbol(twoPin("ptc")) == SymbolKind::Fuse);
    CHECK(classifySymbol(part("testpoint", {"TP"})) == SymbolKind::TestPoint);
    CHECK(classifySymbol(twoPin("wire")) == SymbolKind::Wire);
    CHECK(classifySymbol(twoPin("crimp")) == SymbolKind::Crimp);
}

TEST_CASE("classification words match case-insensitively") {
    CHECK(classifySymbol(twoPin("Resistor")) == SymbolKind::Resistor);
    CHECK(classifySymbol(twoPin("CAPACITOR")) == SymbolKind::Capacitor);
    CHECK(classifySymbol(part("LED", {"a", "k"})) == SymbolKind::Led);
    CHECK(classifySymbol(part("Nmos", {"gate", "drain", "source"})) == SymbolKind::Nmos);
}

TEST_CASE("an unknown classification stays a generic box") {
    CHECK(classifySymbol(twoPin("regulator")) == SymbolKind::Generic);
    CHECK(classifySymbol(twoPin("")) == SymbolKind::Generic);
}

// ---------------------------------------------------------------------------
// Tier 2: the legacy '#type' user field
// ---------------------------------------------------------------------------

TEST_CASE("a legacy '#type' user field still classifies") {
    Component c = twoPin("");
    addField(c, "type", "resistor");
    CHECK(classifySymbol(c) == SymbolKind::Resistor);
}

TEST_CASE("'@type' wins over the legacy field") {
    Component c = twoPin("capacitor");
    addField(c, "type", "resistor");
    CHECK(classifySymbol(c) == SymbolKind::Capacitor);
}

TEST_CASE("a recognised '@type' that fails its gate is not reinterpreted") {
    // The word spoke -- 'led' -- but the pins cannot carry the symbol, so the
    // legacy field and the value unit must not have another go.
    Component c = part("led", {"P1", "P2"});
    addField(c, "type", "resistor");
    addField(c, "value", "10kR");
    CHECK(classifySymbol(c) == SymbolKind::Generic);
}

// ---------------------------------------------------------------------------
// Tier 3: the unit of '#value' on an untyped 2-pin part
// ---------------------------------------------------------------------------

TEST_CASE("an untyped 2-pin part is classified by its value's unit") {
    Component r = twoPin("");
    addField(r, "value", "4k7R");
    CHECK(classifySymbol(r) == SymbolKind::Resistor);

    Component c = twoPin("");
    addField(c, "value", "100nF");
    CHECK(classifySymbol(c) == SymbolKind::Capacitor);

    Component l = twoPin("");
    addField(l, "value", "10uH");
    CHECK(classifySymbol(l) == SymbolKind::Inductor);

    Component x = twoPin("");
    addField(x, "value", "16MHz");
    CHECK(classifySymbol(x) == SymbolKind::Crystal);
}

TEST_CASE("a non-electrical or unparsable value stays generic") {
    Component c = twoPin("");
    addField(c, "value", "3V3");
    CHECK(classifySymbol(c) == SymbolKind::Generic);

    Component d = twoPin("");
    addField(d, "value", "green");
    CHECK(classifySymbol(d) == SymbolKind::Generic);
}

TEST_CASE("the unit fallback needs exactly two pins") {
    Component c = part("", {"A", "B", "C"});
    addField(c, "value", "10kR");
    CHECK(classifySymbol(c) == SymbolKind::Generic);
}

// ---------------------------------------------------------------------------
// Pin-name gating
// ---------------------------------------------------------------------------

TEST_CASE("a diode without identifiable anode and cathode falls back") {
    CHECK(classifySymbol(part("led", {"P1", "P2"})) == SymbolKind::Generic);
    CHECK(classifySymbol(part("diode", {"A", "B"})) == SymbolKind::Generic);
    CHECK(classifySymbol(part("zener", {"ANODE", "CATHODE"})) == SymbolKind::Zener);
}

TEST_CASE("two pins claiming the same role is as bad as none") {
    CHECK(classifySymbol(part("diode", {"A", "A"})) == SymbolKind::Generic);
}

TEST_CASE("two-terminal symbols need exactly two pins") {
    CHECK(classifySymbol(part("resistor", {"A", "B", "C"})) == SymbolKind::Generic);
    CHECK(classifySymbol(part("crystal", {"A", "B", "C", "D"})) == SymbolKind::Generic);
}

TEST_CASE("a mosfet needs gate, drain and source") {
    CHECK(classifySymbol(part("nmos", {"G", "D", "X"})) == SymbolKind::Generic);
    CHECK(classifySymbol(part("mosfet", {"GATE", "DRAIN", "SOURCE"})) == SymbolKind::Mosfet);
}

TEST_CASE("'@type=mosfet' takes its channel from '#channel'") {
    Component n = part("mosfet", {"G", "D", "S"});
    addField(n, "channel", "n");
    CHECK(classifySymbol(n) == SymbolKind::Nmos);

    Component p = part("mosfet", {"G", "D", "S"});
    addField(p, "channel", "P");
    CHECK(classifySymbol(p) == SymbolKind::Pmos);
}

TEST_CASE("a bjt needs base, collector and emitter") {
    CHECK(classifySymbol(part("npn", {"B", "C", "X"})) == SymbolKind::Generic);
    CHECK(classifySymbol(part("pnp", {"BASE", "COLLECTOR", "EMITTER"})) == SymbolKind::Pnp);
}

TEST_CASE("'@type=transistor' draws only with a '#polarity'") {
    CHECK(classifySymbol(part("transistor", {"B", "C", "E"})) == SymbolKind::Generic);

    Component c = part("transistor", {"B", "C", "E"});
    addField(c, "polarity", "pnp");
    CHECK(classifySymbol(c) == SymbolKind::Pnp);
}

TEST_CASE("an opamp needs its inputs and output, power pins optional") {
    CHECK(classifySymbol(part("opamp", {"INP", "INN", "OUT", "VCC", "VEE"})) == SymbolKind::OpAmp);
    CHECK(classifySymbol(part("opamp", {"IN+", "IN-", "OUTPUT", "V+", "V-"})) == SymbolKind::OpAmp);
    // A missing input, or any pin the symbol cannot place, spoils it.
    CHECK(classifySymbol(part("opamp", {"INP", "OUT", "VCC"})) == SymbolKind::Generic);
    CHECK(classifySymbol(part("opamp", {"INP", "INN", "OUT", "NC1"})) == SymbolKind::Generic);
}

// ---------------------------------------------------------------------------
// Structure: connectors and test points
// ---------------------------------------------------------------------------

TEST_CASE("connectors classify from the part type as before") {
    Component c = part("boardconnector", {"1", "2", "3"});
    c.partType = PartType::BoardConnector;
    CHECK(classifySymbol(c) == SymbolKind::Connector);
    c.partType = PartType::CableConnector;
    CHECK(classifySymbol(c) == SymbolKind::Connector);
}

TEST_CASE("an untyped 1-pin part is a test point") {
    CHECK(classifySymbol(part("", {"TP"})) == SymbolKind::TestPoint);
}

TEST_CASE("a 1-pin connector is still a connector") {
    Component c = part("", {"1"});
    c.partType = PartType::BoardConnector;
    CHECK(classifySymbol(c) == SymbolKind::Connector);
}

TEST_MAIN()
