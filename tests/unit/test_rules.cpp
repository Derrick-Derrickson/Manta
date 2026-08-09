// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// User-defined ERC rules: parsing a .mantaRules file and evaluating it over an
// elaborated design.
//
// Both directions matter. A rule that never fires is useless, and one that
// always fires is worse, because people switch it off. Every check here is
// tested against a design that violates it *and* one that does not.
#include <string>

#include "cli/rules_loader.h"
#include "harness.h"
#include "lex/lexer.h"
#include "link/elaborate.h"
#include "parse/parser.h"
#include "rules/rules_eval.h"
#include "rules/rules_parser.h"
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

std::string fixture(std::string_view name) {
    return readFile(std::string(MANTA_TEST_DIR) + "/rules/" + std::string(name));
}

// Compiles a design, loads a rules file, and runs every rule over the result.
struct Run {
    SourceManager sources;
    Arena arena;
    StringInterner interner;
    std::unique_ptr<DiagEngine> diags;
    std::vector<LinkedObject> objects;
    Design design;
    std::string report;

    [[nodiscard]] bool fired(std::string_view code) const {
        return report.find(std::string("[") + std::string(code) + "]") != std::string::npos;
    }
};

std::shared_ptr<Run> check(const std::string& designText, const std::string& rulesText,
                           SeverityPolicy policy = {}) {
    auto r = std::make_shared<Run>();
    r->diags = std::make_unique<DiagEngine>(r->sources, policy);

    const SourceFile* design = r->sources.addVirtual("<design>", designText);
    Lexer designLexer(*design, *r->diags);
    TokenStream designTokens = designLexer.run();
    Parser parser(designTokens, *design, r->arena, r->interner, *r->diags);
    SourceUnit unit = parser.run();
    LocalChecker local(*design, r->interner, *r->diags);
    local.run(unit);

    LinkedObject object;
    object.unit = unit;
    object.index = 0;
    r->objects.push_back(std::move(object));

    SymbolTable symbols(r->interner, *r->diags);
    symbols.addObject(r->objects[0]);
    Elaborator elaborator(symbols, r->interner, *r->diags, r->objects, ElaborateOptions{});
    r->design = elaborator.run(r->interner.intern("b"), Span{});

    // The rules share the design's interner here, as they do at link.
    const SourceFile* rulesFile = r->sources.addVirtual("<rules>", rulesText);
    Lexer rulesLexer(*rulesFile, *r->diags);
    TokenStream rulesTokens = rulesLexer.run();
    RulesParser rulesParser(rulesTokens, *rulesFile, r->arena, r->interner, *r->diags);
    RuleFile parsed = rulesParser.run();
    LoadedRules loaded = loadRules(parsed, r->interner, *r->diags);

    RuleEvaluator evaluator(loaded, r->interner, *r->diags);
    evaluator.runOnDesign(r->design);
    for (const auto& [key, decl] : symbols.all()) {
        if (decl.item->kind != ItemKind::Part) continue;
        PartInfo info = buildPartInfo(decl.item, decl.objectIndex, r->interner, *r->diags);
        evaluator.runOnPart(info, r->interner.text(decl.item->name.symbol));
    }

    RenderOptions opts;
    opts.showSource = false;
    renderDiagnostics(*r->diags, opts, r->report);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// The two motivating checks
// ---------------------------------------------------------------------------

TEST_CASE("logic levels: a driver that cannot clear a receiver's threshold") {
    auto bad = check(fixture("violations.manta"), fixture("checks.mantaRules"));
    CHECK(bad->fired("drive-high"));
    CHECK(bad->report.find("2V4") != std::string::npos);
    CHECK(bad->report.find("3V5") != std::string::npos);

    auto good = check(fixture("clean.manta"), fixture("checks.mantaRules"));
    CHECK_FALSE(good->fired("drive-high"));
}

TEST_CASE("current budget: a rail that cannot supply what hangs off it") {
    auto bad = check(fixture("violations.manta"), fixture("checks.mantaRules"));
    CHECK(bad->fired("current-budget"));
    // 20mA + 200mA against 150mA.
    CHECK(bad->report.find("220mA") != std::string::npos);
    CHECK(bad->report.find("150mA") != std::string::npos);

    auto good = check(fixture("clean.manta"), fixture("checks.mantaRules"));
    CHECK_FALSE(good->fired("current-budget"));
}

TEST_CASE("a correct design provokes no rule at all") {
    // The other half of the argument: the checks above prove each rule fires
    // when it should, and this proves none fires when it should not.
    auto good = check(fixture("clean.manta"), fixture("checks.mantaRules"));
    CHECK_FALSE(good->fired("drive-high"));
    CHECK_FALSE(good->fired("pull-low"));
    CHECK_FALSE(good->fired("current-budget"));
    CHECK_FALSE(good->fired("input-draw"));
    CHECK_FALSE(good->fired("needs-footprint"));
    CHECK_FALSE(good->fired("pin-count"));
}

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

TEST_CASE("a part rule sees what the part declares") {
    auto r = check(R"(
part HAS-FOOT { @~footprint = R-0603; 1 = A &CASUAL; 2 = B &CASUAL; };
part NO-FOOT  { 1 = A &CASUAL; 2 = B &CASUAL; };
block b { GND &TYPE=GROUND; X = .{R1~HAS-FOOT}. = GND; };
)", fixture("checks.mantaRules"));
    CHECK(r->fired("needs-footprint"));
    CHECK(r->report.find("NO-FOOT") != std::string::npos);
    // The part that declares one is not reported.
    CHECK(r->report.find("HAS-FOOT declares no footprint") == std::string::npos);
}

TEST_CASE("a pin-pair rule never pairs a pin with itself") {
    // A bidirectional pin is both a driver and a receiver on its own net. It
    // must not be asked to clear its own threshold.
    auto r = check(R"(
part IO { @~footprint = F; 1 = P<> #VOH=2V4 #VIH=3V5; 2 = G< &TYPE=POWER &~NET=GND; };
block b { GND &TYPE=GROUND; X = P{U1~IO}G = GND; };
)", R"(
rules t {
    #VOH : voltage;
    #VIH : voltage;
    check self for net.a -> net.b {
        when    has(a.VOH) & has(b.VIH);
        require a.VOH >= b.VIH;
        error   "{a} against {b}";
    };
};
)");
    CHECK_FALSE(r->fired("self"));
}

// ---------------------------------------------------------------------------
// Types and units
// ---------------------------------------------------------------------------

TEST_CASE("comparing a current with a voltage is a rules-file error") {
    // This is what the '#NAME : quantity' declarations are for. Without the
    // check, the comparison would silently always be false and the rule would
    // look like it passed.
    auto r = check(fixture("clean.manta"), R"(
rules oops {
    #DRAW : current;
    #VOH  : voltage;
    check mismatched for net {
        require sum(pins.DRAW) <= min(pins.VOH);
        error "net {net}";
    };
};
)");
    CHECK(r->report.find("compares current with voltage") != std::string::npos);
}

TEST_CASE("an empty sum is zero in the declared unit, not absent") {
    // A guard should not have to special-case a net nobody decorated.
    auto r = check(R"(
part P { @~footprint = F; 1 = A<> #SUPPLY=100mA; 2 = B<>; };
block b { GND &TYPE=GROUND; X = A{U1~P}B = GND; };
)", R"(
rules t {
    #SUPPLY : current;
    #DRAW   : current;
    check budget for net {
        when    any(pins.SUPPLY);
        require sum(pins.DRAW) <= sum(pins.SUPPLY);
        error   "net {net} draws {sum(pins.DRAW)}";
    };
};
)");
    // Nothing declares DRAW, so the sum is 0A and the requirement holds.
    CHECK_FALSE(r->fired("budget"));
}

TEST_CASE("an absent attribute makes a comparison false, not an error") {
    auto r = check(R"(
part P { @~footprint = F; 1 = A> #VOH=2V4; 2 = B< ; };
block b { GND &TYPE=GROUND; X = A{U1~P}B = GND; };
)", R"(
rules t {
    #VOH : voltage;
    #VIH : voltage;
    check unguarded for net.d -> net.r {
        require d.VOH >= r.VIH;
        error "{d} against {r}";
    };
};
)");
    // B declares no VIH. Without a guard the comparison is false, so the rule
    // reports -- which is the honest answer, and why guards are written.
    CHECK_EQ(r->report.find("E-TYPE"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Rules-file validation
// ---------------------------------------------------------------------------

TEST_CASE("a check name may not collide with a built-in diagnostic") {
    // "-Wno-E-22" has to mean one thing.
    auto r = check("block b { GND &TYPE=GROUND; };", R"(
rules t { check E-22 for net { require has(net.name); error "x"; }; };
)");
    CHECK(r->report.find("collides with the built-in diagnostic") != std::string::npos);

    auto byMnemonic = check("block b { GND &TYPE=GROUND; };", R"(
rules t { check shorted-device for net { require has(net.name); error "x"; }; };
)");
    CHECK(byMnemonic->report.find("collides with") != std::string::npos);
}

TEST_CASE("a check without a requirement or a message is refused") {
    auto noRequire = check("block b { GND &TYPE=GROUND; };", R"(
rules t { check pointless for net { error "x"; }; };
)");
    CHECK(noRequire->report.find("never fire") != std::string::npos);

    auto noMessage = check("block b { GND &TYPE=GROUND; };", R"(
rules t { check silent for net { require has(net.name); }; };
)");
    CHECK(noMessage->report.find("needs an 'error' or 'warning' message") != std::string::npos);
}

TEST_CASE("a check declared twice is refused") {
    auto r = check("block b { GND &TYPE=GROUND; };", R"(
rules t {
    check dup for net { require has(net.name); error "a"; };
    check dup for net { require has(net.name); error "b"; };
};
)");
    CHECK(r->report.find("declared twice") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Severity
// ---------------------------------------------------------------------------

TEST_CASE("a user rule answers to -Wno- and --warn= by its own name") {
    SeverityPolicy silenced;
    silenced.setUser("drive-high", Severity::Ignored);
    auto quiet = check(fixture("violations.manta"), fixture("checks.mantaRules"), silenced);
    CHECK_FALSE(quiet->fired("drive-high"));
    // Silencing one rule leaves the others alone.
    CHECK(quiet->fired("current-budget"));

    SeverityPolicy demoted;
    demoted.setUser("drive-high", Severity::Warning);
    auto warned = check(fixture("violations.manta"), fixture("checks.mantaRules"), demoted);
    CHECK(warned->report.find("warning[drive-high]") != std::string::npos);
}

TEST_CASE("-Werror promotes a rule's warning but does not resurrect a silenced one") {
    SeverityPolicy werror;
    werror.setWerror(true);
    auto r = check(fixture("violations.manta"), fixture("checks.mantaRules"), werror);
    // "input-draw" is declared a warning in the rules file.
    CHECK_EQ(r->report.find("warning[input-draw]"), std::string::npos);

    SeverityPolicy both;
    both.setWerror(true);
    both.setUser("input-draw", Severity::Ignored);
    auto silenced = check(fixture("violations.manta"), fixture("checks.mantaRules"), both);
    CHECK_FALSE(silenced->fired("input-draw"));
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST_CASE("spec 15.8: rules produce identical output across runs") {
    auto first = check(fixture("violations.manta"), fixture("checks.mantaRules"));
    auto second = check(fixture("violations.manta"), fixture("checks.mantaRules"));
    CHECK_EQ(first->report, second->report);
    CHECK(!first->report.empty());
}

TEST_MAIN()
