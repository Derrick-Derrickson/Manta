// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// '#' fields on pins (spec 4.5), which is what user-defined ERC rules read.
//
// The '&' namespace is closed because the compiler interprets it; '#' is the
// open one, "carried to BOM and documentation untouched, unknown names are
// permitted". A user-defined attribute is exactly that, so it needs no
// registration and a decorated design compiles with no rules file present.
#include <string>

#include "erc/erc.h"
#include "harness.h"
#include "lex/lexer.h"
#include "link/elaborate.h"
#include "parse/parser.h"
#include "sema/local_check.h"

using namespace manta;

namespace {

// Elaborates a design and hands back the result, so pin attributes can be
// inspected as they exist on an instantiated component.
struct Elaborated {
    SourceManager sources;
    Arena arena;
    StringInterner interner;
    std::unique_ptr<DiagEngine> diags;
    std::vector<LinkedObject> objects;
    Design design;
    std::string report;

    [[nodiscard]] const Component* component(std::string_view designator) const {
        for (const Component& c : design.components) {
            if (c.designator == designator) return &c;
        }
        return nullptr;
    }
};

std::shared_ptr<Elaborated> elaborate(const std::string& text) {
    auto e = std::make_shared<Elaborated>();
    e->diags = std::make_unique<DiagEngine>(e->sources);
    const SourceFile* file = e->sources.addVirtual("<test>", text);

    Lexer lexer(*file, *e->diags);
    TokenStream tokens = lexer.run();
    Parser parser(tokens, *file, e->arena, e->interner, *e->diags);
    SourceUnit unit = parser.run();
    LocalChecker checker(*file, e->interner, *e->diags);
    checker.run(unit);

    LinkedObject object;
    object.unit = unit;
    object.index = 0;
    e->objects.push_back(std::move(object));

    SymbolTable symbols(e->interner, *e->diags);
    symbols.addObject(e->objects[0]);

    Elaborator elaborator(symbols, e->interner, *e->diags, e->objects, ElaborateOptions{});
    e->design = elaborator.run(e->interner.intern("b"), Span{});

    RenderOptions opts;
    opts.showSource = false;
    renderDiagnostics(*e->diags, opts, e->report);
    return e;
}

// Finds a pin by its logical name.
const ComponentPin* pin(const Component* c, std::string_view logical) {
    if (!c) return nullptr;
    for (const ComponentPin& p : c->pins) {
        if (p.logical == logical) return &p;
    }
    return nullptr;
}

std::string attr(const ComponentPin* p, std::string_view name) {
    if (!p) return "<no pin>";
    const PinAttribute* a = p->attribute(name);
    return a ? a->value : "<absent>";
}

constexpr std::string_view kParts = R"(
part MCU {
    @~footprint = LQFP-8;
    [1:4] = IO[1:4]<> #VOH=2V4 #VOL=0V4 #VIH=2V0 #VIL=0V8;
    5     = SDA<>     &TYPE=OPENDRAIN #VOL=0V6;
    6     = VCC<      &TYPE=POWER &~NET=3V3;
    7     = GND<      &TYPE=POWER &~NET=GND;
};
part REG {
    @~footprint = SOT-23-5;
    1 = VOUT> &TYPE=POWER #SUPPLY=600mA;
};
)";

}  // namespace

TEST_CASE("a '#' field on a pin map line applies to every pin the line produces") {
    // The whole point of allowing it on the line rather than per pin: a 48-pin
    // bus states a figure once, not 48 times.
    auto e = elaborate(std::string(kParts) + R"(
block b {
    GND &TYPE=GROUND;
    RAIL = VOUT{REG1~REG};
    RAIL = 3V3;
    A = IO[1]{U1~MCU}IO[2] = B;
    A = GND; B = GND;
};
)");
    const Component* u1 = e->component("U1");
    CHECK(u1 != nullptr);
    for (std::string_view p : {"IO[1]", "IO[2]", "IO[3]", "IO[4]"}) {
        CHECK_EQ(attr(pin(u1, p), "VOH"), std::string("2V4"));
        CHECK_EQ(attr(pin(u1, p), "VIH"), std::string("2V"));
    }
    // A field on a different line does not leak across.
    CHECK_EQ(attr(pin(u1, "IO[1]"), "VOL"), std::string("400mV"));
    CHECK_EQ(attr(pin(u1, "SDA"), "VOL"), std::string("600mV"));
    CHECK_EQ(attr(pin(u1, "SDA"), "VOH"), std::string("<absent>"));
}

TEST_CASE("a call site overrides a pin field by declaring a stronger one") {
    // Overriding is the strength ladder, not a separate mechanism: '#!' beats
    // '#'. Two declarations of equal strength that disagree stay E-12 wherever
    // they appear, which is what keeps a field's value unambiguous.
    auto e = elaborate(std::string(kParts) + R"(
block b {
    GND &TYPE=GROUND;
    RAIL = VOUT{REG1~REG};
    RAIL = 3V3;
    A = IO[1]{U1~MCU: IO[3] #!VOH=3V0; }IO[2] = B;
    A = GND; B = GND;
};
)");
    if (e->diags->errorCount() != 0) {
        ::mantatest::fail(__FILE__, __LINE__, "override produced diagnostics:\n" + e->report);
    }
    const Component* u1 = e->component("U1");
    CHECK_EQ(attr(pin(u1, "IO[3]"), "VOH"), std::string("3V"));
    // The others keep what the part declared.
    CHECK_EQ(attr(pin(u1, "IO[1]"), "VOH"), std::string("2V4"));
    CHECK_EQ(attr(pin(u1, "IO[4]"), "VOH"), std::string("2V4"));
}

TEST_CASE("an equal-strength override is a conflict, not an override") {
    // The part declares VOH at normal strength and the call site does too.
    // Nothing says which was meant, so it is E-12 -- write '#!' to override.
    auto e = elaborate(std::string(kParts) + R"(
block b {
    GND &TYPE=GROUND;
    RAIL = VOUT{REG1~REG};
    RAIL = 3V3;
    A = IO[1]{U1~MCU: IO[3] #VOH=3V0; }IO[2] = B;
    A = GND; B = GND;
};
)");
    CHECK(e->report.find("E-12") != std::string::npos);
}

TEST_CASE("a locked pin field cannot be overridden") {
    auto e = elaborate(R"(
part P {
    @~footprint = F;
    1 = A<> #!VOH=2V4;
    2 = B<>;
};
block b {
    GND &TYPE=GROUND;
    X = A{U1~P: A #VOH=3V0; }B = GND;
};
)");
    CHECK(e->report.find("E-11") != std::string::npos);
}

TEST_CASE("two declarations of a pin field at equal strength conflict") {
    auto e = elaborate(R"(
part P {
    @~footprint = F;
    1 = A<> #VOH=2V4 #VOH=3V0;
    2 = B<>;
};
block b {
    GND &TYPE=GROUND;
    X = A{U1~P}B = GND;
};
)");
    CHECK(e->report.find("E-12") != std::string::npos);
}

TEST_CASE("spec 7.3: E-08 compares pins, not arrays") {
    // "IO[1]" as a terminal and "IO[3]" in the binding list are two different
    // pins. Comparing base names alone would reject a design that is correct.
    auto ok = elaborate(std::string(kParts) + R"(
block b {
    GND &TYPE=GROUND;
    RAIL = VOUT{REG1~REG};
    RAIL = 3V3;
    A = IO[1]{U1~MCU: IO[3]=OTHER; }IO[2] = B;
    A = GND; B = GND; OTHER = GND;
};
)");
    CHECK(ok->report.find("E-08") == std::string::npos);

    // The same element on both sides is the error the rule exists for.
    auto bad = elaborate(std::string(kParts) + R"(
block b {
    GND &TYPE=GROUND;
    A = IO[1]{U1~MCU: IO[1]=OTHER; }IO[2] = B;
};
)");
    CHECK(bad->report.find("E-08") != std::string::npos);
}

TEST_CASE("a pin field that only annotates is not a second connection") {
    // "IO[1] #!VOH=3V0" annotates the pin the chain already passes through. It
    // connects nothing, so it is not the double-connection E-08 describes.
    auto e = elaborate(std::string(kParts) + R"(
block b {
    GND &TYPE=GROUND;
    RAIL = VOUT{REG1~REG};
    RAIL = 3V3;
    A = IO[1]{U1~MCU: IO[1] #!VOH=3V0; }IO[2] = B;
    A = GND; B = GND;
};
)");
    CHECK(e->report.find("E-08") == std::string::npos);
    CHECK_EQ(attr(pin(e->component("U1"), "IO[1]"), "VOH"), std::string("3V"));
}

TEST_CASE("a decorated design needs no rules file to compile") {
    // '#' is already open, so nothing has to be registered for this to be legal.
    auto e = elaborate(std::string(kParts) + R"(
block b {
    GND &TYPE=GROUND;
    RAIL = VOUT{REG1~REG};
    RAIL = 3V3;
    A = IO[1]{U1~MCU}IO[2] = B;
    A = GND; B = GND;
};
)");
    CHECK_EQ(e->diags->errorCount(), std::size_t{0});
    CHECK_EQ(attr(pin(e->component("REG1"), "VOUT"), "SUPPLY"), std::string("600mA"));
}

TEST_CASE("a numeric pin attribute keeps its parsed value, not just its text") {
    // Rules compare quantities, so the value has to survive as a number with a
    // unit rather than as a string.
    auto e = elaborate(std::string(kParts) + R"(
block b {
    GND &TYPE=GROUND;
    RAIL = VOUT{REG1~REG};
    RAIL = 3V3;
    A = IO[1]{U1~MCU}IO[2] = B;
    A = GND; B = GND;
};
)");
    const ComponentPin* vout = pin(e->component("REG1"), "VOUT");
    CHECK(vout != nullptr);
    if (!vout) return;
    const PinAttribute* a = vout->attribute("SUPPLY");
    CHECK(a != nullptr);
    if (!a) return;
    CHECK(a->numeric);
    CHECK(a->number.unit == Unit::Ampere);
    CHECK_EQ(a->number.canonical(), std::string("600mA"));
}


// ---------------------------------------------------------------------------
// Multi-pin terminals (spec 7.3, revision 1.6)
// ---------------------------------------------------------------------------

constexpr std::string_view kBridgeParts = R"(
part RES { @~footprint = R-0603; 1 = A &CASUAL; 2 = B &CASUAL; };
part DIODE { @~footprint = SOD-123; 1 = A; 2 = K; };
part CONN4 { @~footprint = HDR-4; 1 = P1; 2 = P2; 3 = P3; 4 = P4; };
)";

TEST_CASE("spec 7.3: a dot run takes its pins onto one node, explicitly") {
    auto e = elaborate(std::string(kBridgeParts) + R"(
block b {
    GND &TYPE=GROUND;
    X = Y = ..{R1~RES};
    X = GND;
};
)");
    const Component* r1 = e->component("R1");
    CHECK(r1 != nullptr);
    if (!r1) return;
    CHECK_EQ(r1->pins.size(), std::size_t{2});
    CHECK(r1->pins[0].net >= 0);
    CHECK_EQ(r1->pins[0].net, r1->pins[1].net);
    // The bridge is the written form of intent, so W-02 records nothing.
    CHECK(e->design.shorted.empty());
}

TEST_CASE("spec 7.3: a pin list joins every listed pin to one node") {
    auto e = elaborate(std::string(kBridgeParts) + R"(
block b {
    GND &TYPE=GROUND;
    X = Y = [A,K]{D1~DIODE};
    VIN = [P1,P2]{J5~CONN4}[P3,P4] = GND;
    X = VIN;
};
)");
    const Component* d1 = e->component("D1");
    CHECK(d1 != nullptr);
    if (!d1) return;
    CHECK_EQ(d1->pins[0].net, d1->pins[1].net);
    CHECK(e->design.shorted.empty());

    // Exit-side list: paralleled connector pins, two per side, sides apart.
    const Component* j5 = e->component("J5");
    CHECK(j5 != nullptr);
    if (!j5) return;
    const ComponentPin* p1 = pin(j5, "P1");
    const ComponentPin* p2 = pin(j5, "P2");
    const ComponentPin* p3 = pin(j5, "P3");
    const ComponentPin* p4 = pin(j5, "P4");
    CHECK(p1 && p2 && p3 && p4);
    if (!(p1 && p2 && p3 && p4)) return;
    CHECK_EQ(p1->net, p2->net);
    CHECK_EQ(p3->net, p4->net);
    CHECK(p1->net != p3->net);
}

TEST_CASE("W-02 is connectivity: incidental bridges recorded, explicit ones not") {
    auto e = elaborate(std::string(kBridgeParts) + R"(
block b {
    GND &TYPE=GROUND;
    X = .{R1~RES}. = Y;
    Y = X;
    Y = ..{R2~RES};
    Y = GND;
};
)");
    CHECK_EQ(e->design.shorted.size(), std::size_t{1});
    if (e->design.shorted.size() != 1) return;
    CHECK(e->design.components[e->design.shorted[0]].designator == "R1");
}

TEST_MAIN()
