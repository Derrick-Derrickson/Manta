// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The conformance suite (spec 1.3).
//
// Every fixture in tests/diag is named for the diagnostic code it must provoke.
// The test runs the whole pipeline over it -- lex, parse, local check, link,
// ERC -- and asserts that the named code fires. Codes E-03, E-16, E-19, E-35
// and W-05 do not exist in the specification and are asserted absent from the
// table rather than tested.
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "erc/erc.h"
#include "harness.h"
#include "lex/lexer.h"
#include "link/elaborate.h"
#include "parse/parser.h"
#include "sema/local_check.h"

using namespace manta;

namespace {

std::string readFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Runs the full pipeline over a set of sources and returns every code emitted.
std::vector<std::string> pipeline(const std::vector<std::string>& paths, std::string& report) {
    SourceManager sources;
    // Every diagnostic is enabled, including those that default to off: the
    // point of this suite is that each code *can* fire on its own fixture.
    SeverityPolicy policy;
    for (std::size_t i = 0; i < static_cast<std::size_t>(DiagId::Count); ++i) {
        auto id = static_cast<DiagId>(i);
        if (diagInfo(id).defaultSeverity == Severity::Ignored) {
            policy.set(id, Severity::Warning);
        }
    }
    DiagEngine diags(sources, policy);
    Arena arena;
    StringInterner interner;

    std::vector<LinkedObject> objects;
    for (const std::string& path : paths) {
        auto loaded = sources.load(path);
        if (!loaded) continue;
        const SourceFile* file = *loaded;

        Lexer lexer(*file, diags);
        TokenStream tokens = lexer.run();
        Parser parser(tokens, *file, arena, interner, diags);
        SourceUnit unit = parser.run();
        LocalChecker checker(*file, interner, diags);
        checker.run(unit);

        LinkedObject object;
        object.unit = unit;
        object.path = path;
        object.index = static_cast<std::uint32_t>(objects.size());
        objects.push_back(std::move(object));
    }

    SymbolTable symbols(interner, diags);
    for (const LinkedObject& o : objects) symbols.addObject(o);
    symbols.checkVersions(Revision::toolchain());

    Elaborator elaborator(symbols, interner, diags, objects, ElaborateOptions{});
    Design design = elaborator.run(interner.intern("b"), Span{});

    ErcChecker erc(design, interner, diags);
    erc.run();

    RenderOptions opts;
    opts.showSource = false;
    renderDiagnostics(diags, opts, report);

    std::vector<std::string> codes;
    for (const Diagnostic& d : diags.diagnostics()) {
        codes.emplace_back(diagInfo(d.id).code);
    }
    return codes;
}

std::string fixture(std::string_view name) {
    return std::string(MANTA_TEST_DIR) + "/diag/" + std::string(name) + ".manta";
}

// Asserts that `code` appears among the diagnostics its own fixture provokes.
void expectFires(std::string_view code, std::vector<std::string> extraFixtures = {}) {
    std::vector<std::string> paths{fixture(code)};
    for (const std::string& extra : extraFixtures) paths.push_back(fixture(extra));

    if (readFile(paths[0]).empty()) {
        ::mantatest::fail(__FILE__, __LINE__,
                          "missing conformance fixture for " + std::string(code));
        return;
    }

    std::string report;
    std::vector<std::string> codes = pipeline(paths, report);
    if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
        ::mantatest::fail(__FILE__, __LINE__,
                          std::string(code) + " did not fire on its fixture. Got:\n" + report);
    }
}

// Asserts that `code` fires on a named fixture. For a code with several
// provoking forms, each form gets its own file -- E-30's pattern -- and the
// suffixed name says which one this is.
void expectFiresOn(std::string_view code, std::string_view fixtureName) {
    std::vector<std::string> paths{fixture(fixtureName)};
    if (readFile(paths[0]).empty()) {
        ::mantatest::fail(__FILE__, __LINE__,
                          "missing conformance fixture " + std::string(fixtureName));
        return;
    }

    std::string report;
    std::vector<std::string> codes = pipeline(paths, report);
    if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
        ::mantatest::fail(__FILE__, __LINE__,
                          std::string(code) + " did not fire on " + std::string(fixtureName) +
                              ". Got:\n" + report);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Section 16.1 -- errors
// ---------------------------------------------------------------------------

TEST_CASE("E-01 multiple drivers") { expectFires("E-01"); }
TEST_CASE("E-02 no driver, and a trailing hyphen") { expectFires("E-02"); }
TEST_CASE("E-04 width mismatch") { expectFires("E-04"); }
TEST_CASE("E-05 replication divisibility") { expectFires("E-05"); }
TEST_CASE("E-06 counted replication arity") { expectFires("E-06"); }
TEST_CASE("E-07 undeclared designator") { expectFires("E-07"); }
TEST_CASE("E-08 terminal also bound") { expectFires("E-08"); }
TEST_CASE("E-09 empty binding list") { expectFires("E-09"); }
TEST_CASE("E-10 unknown system field") { expectFires("E-10"); }
TEST_CASE("E-11 locked override") { expectFires("E-11"); }
TEST_CASE("E-12 equal-strength conflict") { expectFires("E-12"); }
TEST_CASE("E-13 unknown directive") { expectFires("E-13"); }
TEST_CASE("E-14 single-ended impedance on a diff pair") { expectFires("E-14"); }
TEST_CASE("E-15 substitution in a non-value position") { expectFires("E-15"); }
TEST_CASE("E-17 imperial unit") { expectFires("E-17"); }
TEST_CASE("E-18 tolerance as a length") { expectFires("E-18"); }
TEST_CASE("E-20 fitted part with no footprint") { expectFires("E-20"); }
TEST_CASE("E-21 harness name clashes with a designator") { expectFires("E-21"); }
TEST_CASE("E-23 '.' selects a non-casual pin") { expectFires("E-23"); }
TEST_CASE("E-24 no ground declared") { expectFires("E-24"); }
TEST_CASE("E-49 an unpaired '=='") { expectFires("E-49"); }

TEST_CASE("E-02 does not fire when a '<>' pin can drive the input") {
    std::string report;
    std::vector<std::string> codes = pipeline({fixture("E-02-bidir-clean")}, report);
    if (std::find(codes.begin(), codes.end(), "E-02") != codes.end()) {
        ::mantatest::fail(__FILE__, __LINE__,
                          "E-02 fired on a bidir-driven input:\n" + report);
    }
}
TEST_CASE("E-25 a NC pin is connected") { expectFires("E-25"); }
TEST_CASE("E-26 single reference without &STUB") { expectFires("E-26"); }
TEST_CASE("E-27 unpowered net") { expectFires("E-27"); }
TEST_CASE("E-28 two power sources on one net") { expectFires("E-28"); }
TEST_CASE("E-29 undefined field in a substitution") { expectFires("E-29"); }
TEST_CASE("E-30 name declared in more than one object") { expectFires("E-30", {"E-30b"}); }
TEST_CASE("E-31 name referenced but never declared") { expectFires("E-31"); }
TEST_CASE("E-32 block port with no arrow") { expectFires("E-32"); }
TEST_CASE("E-33 &STUB on a multi-pin net") { expectFires("E-33"); }
TEST_CASE("E-34 fixed-set value not upper case") { expectFires("E-34"); }
TEST_CASE("E-36 unsatisfiable version constraint") { expectFires("E-36"); }
TEST_CASE("E-37 multiplicity on a replication") { expectFires("E-37"); }
TEST_CASE("E-38 harness member list width") { expectFires("E-38"); }
TEST_CASE("E-39 division by zero") { expectFires("E-39"); }
TEST_CASE("E-40 integer given to a boolean operator") { expectFires("E-40"); }
TEST_CASE("E-41 non-arithmetic operand") { expectFires("E-41"); }
TEST_CASE("E-42 negative exponent") { expectFires("E-42"); }
TEST_CASE("E-43 a part exports a field") { expectFires("E-43"); }

// Not in the numbered table: the general syntax error, provoked here by a
// section marker with no title (revision 1.3).
TEST_CASE("E-SYNTAX an untitled section marker") { expectFires("E-SYNTAX"); }

// ---------------------------------------------------------------------------
// Revision 1.5 -- '&RAIL' and '&EDGE' (spec 11.3, 11.10)
// ---------------------------------------------------------------------------

TEST_CASE("rev 1.5: '&RAIL' outside net scope is E-13") {
    expectFiresOn("E-13", "E-13-rail-pin");
}
TEST_CASE("rev 1.5: '&EDGE' at net scope is E-13") {
    expectFiresOn("E-13", "E-13-edge-net");
}
TEST_CASE("rev 1.5: a pin directive written bare in a binding list is E-13") {
    // Before 1.5 this form was accepted and applied to nothing; it is now
    // checked against the Instance context (docs/assumptions.md, F2).
    expectFiresOn("E-13", "E-13-instance");
}
TEST_CASE("rev 1.5: '&EDGE=left' is the E-34 case error") {
    expectFiresOn("E-34", "E-34-edge");
}
TEST_CASE("rev 1.5: an unknown '&EDGE' value is E-TYPE") {
    expectFiresOn("E-TYPE", "E-TYPE-edge");
}
TEST_CASE("rev 1.5: duplicate '&EDGE' at equal strength is E-12") {
    expectFiresOn("E-12", "E-12-edge");
}

// ---------------------------------------------------------------------------
// Section 16.2 -- warnings
// ---------------------------------------------------------------------------

TEST_CASE("W-01 unused pins") { expectFires("W-01"); }
TEST_CASE("W-02 device shorted by a '==' run") { expectFires("W-02"); }
TEST_CASE("W-03 capacitor in series between two non-ground nets") { expectFires("W-03"); }
TEST_CASE("W-04 undecoupled supply pin") { expectFires("W-04"); }
TEST_CASE("W-06 weak field never overridden") { expectFires("W-06"); }
TEST_CASE("W-07 identifiers differing only by '-' versus '_'") { expectFires("W-07"); }
TEST_CASE("W-08 frozen swap group") { expectFires("W-08"); }
TEST_CASE("W-09 supply with no consumers") { expectFires("W-09"); }
TEST_CASE("W-TYPE near miss on a structural role") { expectFires("W-TYPE"); }

// ---------------------------------------------------------------------------
// The table itself
// ---------------------------------------------------------------------------

TEST_CASE("every code in the table has a distinct name and mnemonic") {
    for (std::size_t i = 0; i < static_cast<std::size_t>(DiagId::Count); ++i) {
        const DiagInfo& a = diagInfo(static_cast<DiagId>(i));
        CHECK_FALSE(a.code.empty());
        CHECK_FALSE(a.mnemonic.empty());
        for (std::size_t j = i + 1; j < static_cast<std::size_t>(DiagId::Count); ++j) {
            const DiagInfo& b = diagInfo(static_cast<DiagId>(j));
            CHECK(a.code != b.code);
            CHECK(a.mnemonic != b.mnemonic);
        }
    }
}

TEST_CASE("codes absent from the specification are never emitted") {
    // Spec 16 has no E-03, E-16, E-19, E-35 or W-05.
    for (std::string_view absent : {"E-03", "E-16", "E-19", "E-35", "W-05"}) {
        DiagId id{};
        CHECK_FALSE(lookupDiag(absent, id));
    }
}

TEST_CASE("-W, -Wno- and --error= resolve by code and by mnemonic") {
    DiagId byCode{};
    DiagId byMnemonic{};
    CHECK(lookupDiag("W-02", byCode));
    CHECK(lookupDiag("shorted-device", byMnemonic));
    CHECK(byCode == byMnemonic);

    SeverityPolicy policy;
    CHECK(policy.resolve(byCode) == Severity::Warning);
    policy.set(byCode, Severity::Ignored);
    CHECK(policy.resolve(byCode) == Severity::Ignored);

    // -Werror promotes warnings but must not resurrect a silenced one.
    SeverityPolicy werror;
    werror.setWerror(true);
    CHECK(werror.resolve(byCode) == Severity::Error);
    werror.set(byCode, Severity::Ignored);
    CHECK(werror.resolve(byCode) == Severity::Ignored);

    // ...and must not demote an error.
    DiagId e49{};
    CHECK(lookupDiag("E-49", e49));
    CHECK(werror.resolve(e49) == Severity::Error);
}

TEST_MAIN()
