// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The parser against the grammar of spec 19 and the worked examples of spec 20.
//
// Every example in the specification must parse with no diagnostics: spec 1.3
// requires a conforming implementation to "accept every construct in this
// document".
#include "parse/parser.h"

#include <string>

#include "harness.h"
#include "lex/lexer.h"

using namespace manta;

namespace {

struct ParseResult {
    SourceManager sources;
    Arena arena;
    StringInterner interner;
    std::unique_ptr<DiagEngine> diags;
    SourceUnit unit;
    std::string report;
};

// Parses text and collects any diagnostics as a readable string.
std::shared_ptr<ParseResult> parse(std::string text) {
    auto r = std::make_shared<ParseResult>();
    const SourceFile* file = r->sources.addVirtual("<test>", std::move(text));
    r->diags = std::make_unique<DiagEngine>(r->sources);
    Lexer lexer(*file, *r->diags);
    TokenStream toks = lexer.run();
    Parser parser(toks, *file, r->arena, r->interner, *r->diags);
    r->unit = parser.run();
    RenderOptions opts;
    opts.showSource = false;
    renderDiagnostics(*r->diags, opts, r->report);
    return r;
}

std::string readAt(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Reads one of the spec fixture files from tests/spec/.
std::string readFixture(const char* name) {
    return readAt(std::string(MANTA_TEST_DIR) + "/spec/" + name);
}

// ...and one of the conformance fixtures from tests/diag/.
std::string readDiagFixture(const char* name) {
    return readAt(std::string(MANTA_TEST_DIR) + "/diag/" + name);
}

void expectClean(const std::shared_ptr<ParseResult>& r, const char* what) {
    if (r->diags->errorCount() != 0) {
        ::mantatest::fail(__FILE__, __LINE__,
                          std::string(what) + " produced diagnostics:\n" + r->report);
    }
}

}  // namespace

TEST_CASE("spec 20.1-20.3: part declarations parse") {
    auto r = parse(readFixture("parts.manta"));
    expectClean(r, "parts.manta");
    CHECK_EQ(r->unit.items.size(), std::size_t{3});
    CHECK(r->unit.items[0]->isStatic);          // "static part R-10k-1pct-0603"
    CHECK(r->unit.items[0]->kind == ItemKind::Part);
    CHECK_FALSE(r->unit.items[1]->isStatic);
}

TEST_CASE("spec 20.4-20.5: harnesses, netclasses and match groups parse") {
    auto r = parse(readFixture("types.manta"));
    expectClean(r, "types.manta");
    CHECK_EQ(r->unit.items.size(), std::size_t{4});
    CHECK(r->unit.items[0]->kind == ItemKind::Harness);
    CHECK(r->unit.items[2]->kind == ItemKind::Netclass);
    CHECK(r->unit.items[3]->kind == ItemKind::Match);
    // Spec 11.4: "A match group may contain another."
    bool foundNested = false;
    for (const auto& e : r->unit.items[3]->body) {
        if (e.kind == BodyKind::Item && e.item->kind == ItemKind::Match) foundNested = true;
    }
    CHECK(foundNested);
}

TEST_CASE("spec 20.6: a block with substitution parses") {
    auto r = parse(readFixture("rc-filter.manta"));
    expectClean(r, "rc-filter.manta");
    CHECK_EQ(r->unit.items.size(), std::size_t{1});
    CHECK(r->unit.items[0]->kind == ItemKind::Block);
}

TEST_CASE("spec 20.7: the complete board parses") {
    auto r = parse(readFixture("board.manta"));
    expectClean(r, "board.manta");
    CHECK_EQ(r->unit.items.size(), std::size_t{1});
    CHECK(r->unit.items[0]->kind == ItemKind::Block);
}

// ---------------------------------------------------------------------------
// Targeted grammar checks
// ---------------------------------------------------------------------------

TEST_CASE("spec 2.4: whitespace is insignificant") {
    auto multi = parse("block b { SW = SW-NODE\n   = .{L1~MT100UFA}.\n   = 3V3; };");
    auto single = parse("block b { SW = SW-NODE = .{L1~MT100UFA}. = 3V3; };");
    expectClean(multi, "multi-line chain");
    expectClean(single, "single-line chain");
}

TEST_CASE("spec 2.3: a leading '-' is resolved by grammatical position") {
    // In a net position "-5V" names a net; in a value position it is a number.
    auto r = parse("block b { BIAS = -5V; #min-supply = -5V; };");
    expectClean(r, "leading hyphen");

    const Item* b = r->unit.items[0];
    const Stmt* chain = b->body[0].stmt;
    CHECK(chain->kind == StmtKind::Chain);
    const Element* second = chain->chain->segments[0]->elements[1];
    CHECK(second->kind == ElementKind::Net);

    const Stmt* field = b->body[1].stmt;
    CHECK(field->kind == StmtKind::Field);
    CHECK(field->field->value->kind == ValueKind::Dimensioned);
    CHECK_EQ(field->field->value->num.mantissa, std::int64_t{-5});
    CHECK(field->field->value->num.unit == Unit::Volt);
}

TEST_CASE("spec 16.1: E-02 fires on an identifier ending in '-'") {
    auto r = parse("block b { VCC- = GND; };");
    CHECK(r->report.find("E-02") != std::string::npos);
}

TEST_CASE("spec 4.3: a version constraint ending in '-' is not E-02") {
    // "1.2-" means revision 1.2 or earlier, and lexes as a hyphen-terminated
    // word. Only the parser can tell it from a malformed identifier.
    auto r = parse("block b { @VERSION = 1.2-; };");
    CHECK(r->report.find("E-02") == std::string::npos);
    expectClean(r, "upper-bound version constraint");

    auto range = parse("block b { @VERSION = 0.2-1.2; };");
    expectClean(range, "range version constraint");
    const Value* v = range->unit.items[0]->body[0].stmt->field->value;
    CHECK(v->kind == ValueKind::Version);
    CHECK_EQ(v->version.loMinor, std::uint32_t{2});
    CHECK_EQ(v->version.hiMinor, std::uint32_t{2});
    CHECK_EQ(v->version.hiMajor, std::uint32_t{1});
}

TEST_CASE("spec 8.3: replication forms") {
    auto inferred = parse("block b { >A[0:3] = [[ I.{U?~amp}.O ]] = B[0:3]>; };");
    expectClean(inferred, "inferred replication");

    auto counted = parse("block b { >A[0:3] = [4[ I.{U?~splitter}.O[0:1] ]8] = B[0:7]>; };");
    expectClean(counted, "counted replication");

    const Element* e = counted->unit.items[0]->body[0].stmt->chain->segments[0]->elements[1];
    CHECK(e->kind == ElementKind::Replication);
    CHECK(e->replication->counted);
    CHECK_EQ(e->replication->inWidth, std::int64_t{4});
    CHECK_EQ(e->replication->outWidth, std::int64_t{8});
}

TEST_CASE("spec 8.3: E-37 rejects multiplicity on a replication") {
    auto direct = parse("block b { A = [[.{R?~r}.]]+2 = B; };");
    CHECK(direct->report.find("E-37") != std::string::npos);

    // "a group whose sole content is a replication" does not launder it.
    auto grouped = parse("block b { A = ([[.{R?~r}.]])+2 = B; };");
    CHECK(grouped->report.find("E-37") != std::string::npos);
}

TEST_CASE("spec 8.6: each multiplicity operator parses") {
    auto r = parse(
        "block b {"
        "  A = (.{L?~ind}.)+2 = B;"
        "  C = (A.{D?~dio}.K)|2 = D;"
        "  E = ({C?~cap: .=GND}.)*4;"
        "};");
    expectClean(r, "multiplicity");
    const Item* b = r->unit.items[0];
    CHECK(b->body[0].stmt->chain->segments[0]->elements[1]->group->mult == MultKind::Series);
    CHECK(b->body[1].stmt->chain->segments[0]->elements[1]->group->mult == MultKind::Parallel);
    CHECK(b->body[2].stmt->chain->segments[0]->elements[1]->group->mult == MultKind::Node);
}

TEST_CASE("spec 6.2/6.3: the connector states whether the chain moved") {
    // '=' advances through a far side; '==' continues on the near side of a
    // dead-end element. The buck idiom: diode and boot cap on the SW node,
    // the chain leaving through the inductor, decoupling on the far node.
    auto buck = parse(
        "block b { SW = K.{D2~d: .A = GND;} == .{C3~c: . = BST;} == A.{L1~l}.B"
        " = .{C4~c: . = GND;} == FIVE; };");
    expectClean(buck, "buck chain");

    // A shunt ladder: attach, then continue on the node, twice.
    auto ladder = parse(
        "block b { V3 = .{C1~c: . = GND;} == .{C2~c: . = GND;} == .{C3~c: . = GND;}; };");
    expectClean(ladder, "decap ladder");

    // '==' after an element that passes through is E-49...
    auto afterNet = parse("block b { A == B; };");
    CHECK(afterNet->report.find("E-49") != std::string::npos);
    auto afterDevice = parse("block b { A = .{R1~r}. == B; };");
    CHECK(afterDevice->report.find("E-49") != std::string::npos);

    // ...and so is '=' after one with no far side.
    auto advanceDead = parse("block b { A = .{C1~c: . = GND;} = B; };");
    CHECK(advanceDead->report.find("E-49") != std::string::npos);

    // A '==' with nothing after it continues the node into nowhere.
    auto trailing = parse("block b { A = .{C1~c: . = GND;} ==; };");
    CHECK(trailing->report.find("E-49") != std::string::npos);

    // A binding is rooted at a pin, and a pin passes through, so a binding
    // never opens with '=='.
    auto bindingSame = parse("block b { {U1~p: VIN == VPOS; }; };");
    CHECK(bindingSame->report.find("E-49") != std::string::npos);
}

TEST_CASE("spec 7.3: multi-pin terminals parse") {
    // A run of dots takes that many casual pins onto one node -- the written
    // form of a deliberate short -- and a pin list does the same by name.
    auto dots = parse("block b { A = B = ..{R1~r}; };");
    expectClean(dots, "dot-run terminal");
    const Element* e = dots->unit.items[0]->body[0].stmt->chain->segments[0]->elements[2];
    CHECK(e->kind == ElementKind::Device);
    CHECK(e->device->hasEntry);
    CHECK(e->device->entry.dot);
    CHECK_EQ(e->device->entry.dotCount, std::uint32_t{2});
    CHECK_FALSE(e->device->hasExit);

    auto list = parse("block b { A = B = [A,K].{D1~d}; };");
    expectClean(list, "pin-list terminal");
    const Element* le = list->unit.items[0]->body[0].stmt->chain->segments[0]->elements[2];
    CHECK(le->kind == ElementKind::Device);
    CHECK(le->device->entry.hasList);
    CHECK_EQ(le->device->entry.list.size(), std::size_t{2});

    // A pin list works on the exit side too: paralleled connector pins.
    auto both = parse("block b { VIN = [1,2].{J5~conn}.[3,4] = GND; };");
    expectClean(both, "pin lists both sides");
    const Element* be = both->unit.items[0]->body[0].stmt->chain->segments[0]->elements[1];
    CHECK(be->device->entry.hasList);
    CHECK(be->device->exit.hasList);
}

TEST_CASE("spec 6.2: '=' is the plain join, bare nets included") {
    // Revision 1.6 retires E-22: "A = B;" puts two names on one node.
    auto r = parse("block b { A = B; };");
    expectClean(r, "bare-net join");
}

TEST_CASE("spec 6: every connection operator parses") {
    auto r = parse(
        "block b {"
        "  A = .{C0~c: . = GND;} == B;"
        "  C = .{R?~r}. = D;"
        "  E[0:3] = [[.{R?~r}.]] =* F;"
        "  G *= H[0:7];"
        "  I = 1.{J?~c} ^ {J?~c}.1 = K;"
        "};");
    expectClean(r, "connectors");
    const Item* b = r->unit.items[0];
    CHECK(b->body[0].stmt->chain->segments[0]->connectors[1] == Connector::Same);
    CHECK(b->body[1].stmt->chain->segments[0]->connectors[0] == Connector::Advance);
    CHECK(b->body[2].stmt->chain->segments[0]->connectors[1] == Connector::Gather);
    CHECK(b->body[3].stmt->chain->segments[0]->connectors[0] == Connector::Broadcast);
    // '^' partitions the statement into independent segments (spec 6.4).
    CHECK_EQ(b->body[4].stmt->chain->segments.size(), std::size_t{2});
}

TEST_CASE("spec 7.4: E-09 rejects an empty binding list") {
    auto colon = parse("block b { A = .{L1~ind:}. = B; };");
    CHECK(colon->report.find("E-09") != std::string::npos);
    auto semi = parse("block b { A = .{L1~ind;}. = B; };");
    CHECK(semi->report.find("E-09") != std::string::npos);
    // The bare form is correct and must stay clean.
    auto bare = parse("block b { A = .{L1~ind}. = B; };");
    expectClean(bare, "bare device");
}

TEST_CASE("spec 7.2: '~' distinguishes declaring from referencing") {
    auto r = parse("block b { A = I.{U?~AMP012}.O = B; C = I.{U3}.O = D; };");
    expectClean(r, "declare vs reference");
    const Item* b = r->unit.items[0];
    const Device* decl = b->body[0].stmt->chain->segments[0]->elements[1]->device;
    const Device* ref = b->body[1].stmt->chain->segments[0]->elements[1]->device;
    CHECK(decl->instance->declares);
    CHECK_FALSE(ref->instance->declares);
    CHECK(decl->instance->designator.kind == DesignatorKind::Unassigned);
    CHECK(ref->instance->designator.kind == DesignatorKind::Numbered);
    CHECK_EQ(ref->instance->designator.number, std::int64_t{3});
}

TEST_CASE("spec 13.3: range designators parse, contiguous and not") {
    auto r = parse("block b { A = (.{BLK%[1:4]~blk}.)*4 == B; C = .{BLK%[1:4,9:10]~blk}. = D; };");
    expectClean(r, "range designators");
    const Item* b = r->unit.items[0];
    const Designator& d1 =
        b->body[0].stmt->chain->segments[0]->elements[1]->group->body->elements[0]->device->instance->designator;
    CHECK(d1.kind == DesignatorKind::Range);
    CHECK_EQ(d1.parts.size(), std::size_t{1});
    CHECK_EQ(d1.parts[0].lo, std::int64_t{1});
    CHECK_EQ(d1.parts[0].hi, std::int64_t{4});

    const Designator& d2 =
        b->body[1].stmt->chain->segments[0]->elements[1]->device->instance->designator;
    CHECK_EQ(d2.parts.size(), std::size_t{2});
    CHECK_EQ(d2.parts[1].lo, std::int64_t{9});
}

TEST_CASE("spec 12.4: '+' and '-' are harness member names") {
    auto r = parse("block b { USB.+ = MCU-USB.+; USB.- = MCU-USB.-; X = Y.[+,-]; };");
    expectClean(r, "diff members");
}

TEST_CASE("spec 14: substitution parses in every value position") {
    auto r = parse(
        "block b {"
        "  A = .{R?~$val$-0603}. = B;"
        "  @fitted=$fit-amp$;"
        "  C = D.GPIO[$n$];"
        "  E = F &CURRENT=$amps$A;"
        "  #w = $100 * 2$R;"
        "};");
    expectClean(r, "substitution positions");
}

TEST_CASE("spec 14.3: substitution operator precedence") {
    // "$ (a + b) * 2 $" and "$ n ^ 2 $" and "$ mode = 3 | override $".
    auto r = parse("block b { #x = $ (a + b) * 2 $; #y = $ n ^ 2 $; #z = $ mode = 3 | override $; };");
    expectClean(r, "expression precedence");

    const Item* b = r->unit.items[0];
    // (a + b) * 2 -- the multiplication is the root, its lhs the addition.
    const Expr* e = b->body[0].stmt->field->value->interp->chunks[0].expr;
    CHECK(e->kind == ExprKind::Binary);
    CHECK(e->binOp == BinOp::Mul);
    CHECK(e->lhs->binOp == BinOp::Add);

    // "mode = 3 | override": '|' is looser than '=', so or is the root.
    const Expr* z = b->body[2].stmt->field->value->interp->chunks[0].expr;
    CHECK(z->binOp == BinOp::Or);
    CHECK(z->lhs->binOp == BinOp::Eq);
}

TEST_CASE("spec 14.5: '-' inside a substitution is always subtraction") {
    auto r = parse("block b { #x = $min - supply$; #y = $\"min-supply\" + 1$; };");
    expectClean(r, "hyphen inside substitution");
    const Item* b = r->unit.items[0];
    const Expr* sub = b->body[0].stmt->field->value->interp->chunks[0].expr;
    CHECK(sub->binOp == BinOp::Sub);
    CHECK(sub->lhs->kind == ExprKind::FieldRef);

    // A quoted name is the hyphenated field itself, not a subtraction.
    const Expr* quoted = b->body[1].stmt->field->value->interp->chunks[0].expr;
    CHECK(quoted->binOp == BinOp::Add);
    CHECK(quoted->lhs->kind == ExprKind::FieldRef);
}

TEST_CASE("spec 9.4: import and export sigil orders both parse") {
    auto r = parse("block b { >#author = TJM; >#~source = digikey; #!>board-rev = C; };");
    expectClean(r, "field direction");
    const Item* b = r->unit.items[0];
    CHECK(b->body[0].stmt->field->direction == FieldDirection::Import);
    CHECK(b->body[1].stmt->field->direction == FieldDirection::Import);
    CHECK(b->body[1].stmt->field->strength == Strength::Weak);
    CHECK(b->body[2].stmt->field->direction == FieldDirection::Export);
    CHECK(b->body[2].stmt->field->strength == Strength::Locked);
}

TEST_CASE("spec 10: every port arrow form parses") {
    auto r = parse("block b { >SIG; SIG2<; <SIG3; SIG4>; <>SIG5; SIG6<>; >>VIN; V3V3>>; };");
    expectClean(r, "port arrows");
    const Item* b = r->unit.items[0];
    auto dirOf = [&](std::size_t i, bool leading) {
        const NetExpr* n = b->body[i].stmt->chain->segments[0]->elements[0]->net;
        return leading ? n->leading.dir : n->trailing.dir;
    };
    CHECK(dirOf(0, true) == PortDir::In);    // >SIG
    CHECK(dirOf(1, false) == PortDir::In);   // SIG2<
    CHECK(dirOf(2, true) == PortDir::Out);   // <SIG3
    CHECK(dirOf(3, false) == PortDir::Out);  // SIG4>
    CHECK(dirOf(4, true) == PortDir::Bidir);
    CHECK(dirOf(5, false) == PortDir::Bidir);
    const NetExpr* vin = b->body[6].stmt->chain->segments[0]->elements[0]->net;
    CHECK(vin->leading.global);
    CHECK(vin->leading.dir == PortDir::In);
}

TEST_CASE("spec 10.3: the list form of a global port parses") {
    auto r = parse("block b { [V3V3, GND]>>; };");
    expectClean(r, "port list");
    const Stmt* s = r->unit.items[0]->body[0].stmt;
    CHECK(s->kind == StmtKind::PortList);
    CHECK_EQ(s->ports.size(), std::size_t{2});
    CHECK(s->listArrow.global);
}

TEST_CASE("spec 11.5: a pin may carry a directive without a net") {
    auto r = parse("block b { A = .{U5~ddr-chip: .DQ[0] &PINDELAY=18ps; }. = B; };");
    expectClean(r, "pin-scoped directive");
    const Instance* inst =
        r->unit.items[0]->body[0].stmt->chain->segments[0]->elements[1]->device->instance;
    CHECK_EQ(inst->bindings.size(), std::size_t{1});
    CHECK(inst->bindings[0]->kind == BindingKind::PinNet);
    CHECK_EQ(inst->bindings[0]->pinDirectives.size(), std::size_t{1});
    CHECK(inst->bindings[0]->net == nullptr);
}

TEST_CASE("spec 11.6: '&NET=?' and 'pin = ?' unbind") {
    auto r = parse("block b { A = .{U5~iso: .GNDB=?; }. = B; };");
    expectClean(r, "unbind");
    const Instance* inst =
        r->unit.items[0]->body[0].stmt->chain->segments[0]->elements[1]->device->instance;
    CHECK(inst->bindings[0]->unbind);
}

TEST_CASE("spec 7.5: the DNP prefix parses on parts and blocks") {
    auto r = parse("block b { {!R?~0R-0603}; A = .{!BLK?~audio-stage}. = B; };");
    expectClean(r, "DNP");
    const Item* b = r->unit.items[0];
    CHECK(b->body[0].stmt->chain->segments[0]->elements[0]->device->instance->dnp);
    CHECK(b->body[1].stmt->chain->segments[0]->elements[1]->device->instance->dnp);
}

TEST_CASE("a pin name follows the identifier rules like any other") {
    // A trailing '-' is E-02 wherever it appears, including on a pin. A part
    // that needs a negative supply rail names it something the identifier
    // grammar can produce.
    auto bad = parse("block b { X = INA.{U1~op: .V-=GND; }.OUTA = Y; };");
    CHECK(bad->report.find("E-02") != std::string::npos);

    auto good = parse("block b { X = INA.{U1~op: .v-neg=GND; }.OUTA = Y; };");
    expectClean(good, "hyphenated pin name");
}

TEST_CASE("a device is always braced") {
    // An instance written bare is not a device and does not parse.
    auto bare = parse("block b { R?~0R-0603; };");
    CHECK(bare->diags->errorCount() > 0);
}

TEST_CASE("spec 6.6: 'extern' prefixes a statement") {
    auto r = parse("block b { extern U5.1 = GND; };");
    expectClean(r, "extern");
    CHECK(r->unit.items[0]->body[0].stmt->isExtern);
}

TEST_CASE("spec 2.5: comments may appear wherever whitespace may") {
    auto r = parse(
        "block b { // the switching node\n"
        "  SW = SW-NODE   // trailing\n"
        "     = .{L1~MT100UFA /* 10uH */}.\n"
        "     = 3V3;\n"
        "};");
    expectClean(r, "comments");
}

TEST_CASE("spec 2.5: block comments do not nest") {
    // "the first '*/' closes the comment", so the '/*' inside is not special
    // and the text after '*/' is live code again.
    auto r = parse("block b { /* outer /* inner */ A = B; };");
    expectClean(r, "non-nesting block comment");
}

TEST_CASE("spec 2.2: identifiers are case sensitive") {
    auto r = parse("block b { X = .{U1~p: .SDA=A; .sda=B; .Sda=C; }. = Y; };");
    expectClean(r, "case sensitivity");
    const Instance* inst =
        r->unit.items[0]->body[0].stmt->chain->segments[0]->elements[1]->device->instance;
    CHECK_EQ(inst->bindings.size(), std::size_t{3});
    CHECK(inst->bindings[0]->pin.symbol != inst->bindings[1]->pin.symbol);
    CHECK(inst->bindings[1]->pin.symbol != inst->bindings[2]->pin.symbol);
}

// ---------------------------------------------------------------------------
// Revision 1.4 -- a binding is a chain rooted at a pin (spec 7.4)
// ---------------------------------------------------------------------------

namespace {

// The instance of the device that is the first element of body entry `stmt`.
const Instance* instanceIn(const std::shared_ptr<ParseResult>& r, std::size_t stmt) {
    return r->unit.items[0]->body[stmt].stmt->chain->segments[0]->elements[0]->device->instance;
}

}  // namespace

TEST_CASE("rev 1.4: 'PIN = NET' keeps exactly the shape it had") {
    // The degenerate case of the new rule, and the only one that existed
    // before it. It must still arrive through Binding::net, with no chain.
    auto r = parse("block b { {U1~p: .GND = AGND; }; };");
    expectClean(r, "bare net binding");
    const Binding* b = instanceIn(r, 0)->bindings[0];
    CHECK(b->connector == Connector::Advance);
    CHECK(b->net != nullptr);
    CHECK(b->rhs == nullptr);
    CHECK_FALSE(b->unbind);
}

TEST_CASE("rev 1.4: a binding opens with any connector and carries a chain") {
    auto r = parse(
        "block b {"
        "  {U1~p:"
        "    .VIN       = VPOS = .{C1~c: . = GND;};"
        "    .SW        = K.{D2~d: .A = GND;} == .{C3~c: . = BST;};"
        "    .LANE[0:3] =* COMMON;"
        "    .REF       *= FANOUT[0:3];"
        "    .EN        = .{R5~r}. = VPOS;"
        "    .NC        = ?;"
        "  };"
        "};");
    expectClean(r, "chain bindings");
    const Instance* inst = instanceIn(r, 0);
    CHECK_EQ(inst->bindings.size(), std::size_t{6});

    // "VIN = VPOS = .{C1~c: . = GND;}": the rail, then the shunt on it --
    // plain joins, since neither VPOS nor the shunt asks for more.
    const Binding* vin = inst->bindings[0];
    CHECK(vin->connector == Connector::Advance);
    CHECK(vin->net == nullptr);
    CHECK(vin->rhs != nullptr);
    CHECK_EQ(vin->rhs->elements.size(), std::size_t{2});
    CHECK(vin->rhs->connectors[0] == Connector::Advance);
    CHECK(vin->rhs->elements[1]->kind == ElementKind::Device);
    CHECK_EQ(vin->rhs->elements[1]->device->instance->bindings.size(), std::size_t{1});

    // The diode dead-ends at the pin's node and '==' continues there
    // (spec 6.3); both elements are devices with terminals of their own.
    const Binding* sw = inst->bindings[1];
    CHECK(sw->connector == Connector::Advance);
    CHECK(sw->rhs != nullptr);
    CHECK_EQ(sw->rhs->elements.size(), std::size_t{2});
    CHECK(sw->rhs->elements[0]->device->hasEntry);

    // '=*' and '*=' (spec 6.5). A lone net still lands in 'rhs': only the
    // '=' spelling is the pre-1.4 form.
    CHECK(inst->bindings[2]->connector == Connector::Gather);
    CHECK(inst->bindings[2]->net == nullptr);
    CHECK(inst->bindings[2]->rhs != nullptr);
    CHECK_EQ(inst->bindings[2]->rhs->elements.size(), std::size_t{1});
    CHECK(inst->bindings[3]->connector == Connector::Broadcast);
    CHECK(inst->bindings[3]->rhs != nullptr);

    // A device with a net after it is a chain, not a bare net.
    CHECK(inst->bindings[4]->net == nullptr);
    CHECK_EQ(inst->bindings[4]->rhs->elements.size(), std::size_t{2});

    // Spec 11.6 is untouched: "= ?" is an unbind, not a segment.
    CHECK(inst->bindings[5]->unbind);
    CHECK(inst->bindings[5]->net == nullptr);
    CHECK(inst->bindings[5]->rhs == nullptr);
}

TEST_CASE("rev 1.4: a trailing '&' or '#' still belongs to the pin") {
    // "{U1~x: VIN = NET &STUB;}" writes a *pin* directive, not a directive on
    // the chain: a segment stops at anything that is not a connector, so the
    // binding's own directive loop gets it either way.
    auto r = parse(
        "block b {"
        "  {U1~p:"
        "    .VIN = NET &STUB;"
        "    .SW  = .{C1~c: . = GND;} &~PINDELAY=8ps;"
        "    .FB  = NET2 #VOH=3V0;"
        "    .DQ[0] &PINDELAY=18ps;"
        "  };"
        "};");
    expectClean(r, "pin directives after a binding");
    const Instance* inst = instanceIn(r, 0);

    CHECK(inst->bindings[0]->net != nullptr);
    CHECK(inst->bindings[0]->rhs == nullptr);
    CHECK_EQ(inst->bindings[0]->pinDirectives.size(), std::size_t{1});

    CHECK(inst->bindings[1]->rhs != nullptr);
    CHECK_EQ(inst->bindings[1]->rhs->elements.size(), std::size_t{1});
    CHECK_EQ(inst->bindings[1]->pinDirectives.size(), std::size_t{1});

    CHECK(inst->bindings[2]->net != nullptr);
    CHECK_EQ(inst->bindings[2]->pinFields.size(), std::size_t{1});

    // A pin carrying nothing but a directive has no right-hand side at all.
    CHECK(inst->bindings[3]->net == nullptr);
    CHECK(inst->bindings[3]->rhs == nullptr);
    CHECK_FALSE(inst->bindings[3]->unbind);
    CHECK_EQ(inst->bindings[3]->pinDirectives.size(), std::size_t{1});
}

TEST_CASE("rev 1.4: '.' as the pin takes a chain, and nests") {
    auto r = parse("block b { A = .{U1~p: . = .{C1~c: . = .{C2~c: . = GND;};}; }. = B; };");
    expectClean(r, "dot pin with a chain");
    const Instance* inst =
        r->unit.items[0]->body[0].stmt->chain->segments[0]->elements[1]->device->instance;
    const Binding* b = inst->bindings[0];
    CHECK(b->pinIsDot);
    CHECK(b->connector == Connector::Advance);
    CHECK(b->rhs != nullptr);

    // ...and the device inside it holds a device inside that.
    const Instance* shunt = b->rhs->elements[0]->device->instance;
    CHECK(shunt->bindings[0]->rhs != nullptr);
    CHECK(shunt->bindings[0]->rhs->elements[0]->kind == ElementKind::Device);
}

TEST_CASE("rev 1.4: the whole range of chain bindings parses") {
    auto r = parse(readFixture("bindings.manta"));
    expectClean(r, "bindings.manta");
}

TEST_CASE("rev 1.4: a chain binding leaves E-09 and the trailing ';' alone") {
    // An empty binding list is still E-09, whatever the bindings could have been.
    auto empty = parse("block b { A = .{L1~ind:}. = B; };");
    CHECK(empty->report.find("E-09") != std::string::npos);

    // A ';' before the '}' is permitted and is what the formatter emits.
    auto trailing = parse("block b { {U1~p: .A = .{C1~c: . = GND;}; }; };");
    expectClean(trailing, "trailing semicolon after a chain binding");
    CHECK_EQ(instanceIn(trailing, 0)->bindings.size(), std::size_t{1});
}

TEST_CASE("rev 1.4: one bad binding is reported once, not once per line after it") {
    // The device's '{' is consumed before its binding list is read, so a
    // binding that fails mid-parse leaves that brace open. Resuming at
    // block-body level from inside the binding list turned one mistake into a
    // page of diagnostics on lines that were never wrong.
    auto r = parse(readDiagFixture("E-SYNTAX-cascade.manta"));
    CHECK(r->diags->errorCount() > 0);
    if (r->diags->errorCount() > 3) {
        ::mantatest::fail(__FILE__, __LINE__,
                          "one bad binding cascaded into " +
                              std::to_string(r->diags->errorCount()) + " errors:\n" + r->report);
    }
    // The give-away of the old behaviour: the braces closing the block and the
    // file were read as the start of a declaration.
    CHECK(r->report.find("expected 'block', 'part'") == std::string::npos);

    // The statement after the device parses as a statement of the block, which
    // it could not do if the parser were still adrift inside the binding list.
    const Item* b = r->unit.items[0];
    CHECK_EQ(b->body.size(), std::size_t{3});
    const BodyEntry& last = b->body[2];
    CHECK(last.kind == BodyKind::Stmt);
    CHECK(last.stmt->kind == StmtKind::Chain);
    CHECK_EQ(last.stmt->chain->segments[0]->elements.size(), std::size_t{2});
    CHECK(last.stmt->chain->segments[0]->connectors[0] == Connector::Advance);
}

TEST_CASE("rev 1.4: only '=' takes the unbind '?'") {
    // The other three connectors join a run of elements and there is no run to
    // join to nothing, so "A == ?" is a syntax error -- reported, and contained.
    auto r = parse(readDiagFixture("E-SYNTAX-binding.manta"));
    CHECK(r->diags->errorCount() > 0);
    CHECK(r->report.find("expected a name") != std::string::npos);
    CHECK(r->report.find("expected 'block', 'part'") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Spec 2.8 -- the end-of-content marker
// ---------------------------------------------------------------------------

namespace {

// Lexes text and reports where the content ended.
std::uint32_t contentEndOf(const std::string& text, std::size_t* errors = nullptr) {
    static SourceManager sources;
    DiagEngine diags(sources);
    const SourceFile* file = sources.addVirtual("<eoc>", text);
    Lexer lexer(*file, diags);
    TokenStream toks = lexer.run();
    if (errors) *errors = diags.errorCount();
    return toks.contentEnd;
}

}  // namespace

TEST_CASE("spec 2.8: a '---' line ends the manta content") {
    auto r = parse("part p { 1: A &CASUAL; };\n---\nnot manta at all: } ; ~ $\n");
    expectClean(r, "file with a datasheet");
    CHECK_EQ(r->unit.items.size(), std::size_t{1});
}

TEST_CASE("spec 2.8: the marker must be a line of its own") {
    // Only whitespace may follow it...
    CHECK(contentEndOf("part p { 1: A &CASUAL; };\n---   \ntail\n") != TokenStream::kNoContentEnd);
    CHECK(contentEndOf("part p { 1: A &CASUAL; };\n---\t\ntail\n") != TokenStream::kNoContentEnd);
    // ...and it may be the last line, with or without a newline.
    CHECK(contentEndOf("part p { 1: A &CASUAL; };\n---") != TokenStream::kNoContentEnd);

    // Anything else on the line means it is not a marker.
    CHECK_EQ(contentEndOf("part p { 1: A &CASUAL; };\n--- tail\n"), TokenStream::kNoContentEnd);
    CHECK_EQ(contentEndOf("part p { 1: A &CASUAL; };\n----\n"), TokenStream::kNoContentEnd);
    // Nor is it one when it does not begin a line.
    CHECK_EQ(contentEndOf("part p { 1: A &CASUAL; };  ---\n"), TokenStream::kNoContentEnd);
}

TEST_CASE("spec 2.8: '---' inside a declaration is never a truncation") {
    // Silently discarding the rest of the file would be far worse than an error,
    // so the end-of-content marker is recognised only where a declaration could
    // begin. Inside a block the same shape is a *section* marker (revision
    // 1.3), and a bare one has no title, which the parser rejects.
    CHECK_EQ(contentEndOf("block b {\n    A = B;\n---\n    C = D;\n};\n"),
             TokenStream::kNoContentEnd);

    auto r = parse("block b {\n    A = B;\n---\n    C = D;\n};\n");
    CHECK(r->diags->errorCount() > 0);
}

TEST_CASE("spec 2.8: a file that is nothing but a marker") {
    auto r = parse("---\neverything here is documentation\n");
    expectClean(r, "marker-only file");
    CHECK_EQ(r->unit.items.size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Revision 1.3 -- render section markers
// ---------------------------------------------------------------------------

namespace {

// The interned title of a Section body entry, or "" when it is not one.
std::string sectionTitle(const std::shared_ptr<ParseResult>& r, const BodyEntry& e) {
    if (e.kind != BodyKind::Section) return {};
    return std::string(r->interner.text(e.section->name));
}

}  // namespace

TEST_CASE("rev 1.3: section markers parse inside a block body, nested included") {
    auto r = parse(readFixture("sections.manta"));
    expectClean(r, "sections.manta");

    const Item* demo = r->unit.items[1];
    CHECK(demo->kind == ItemKind::Block);
    CHECK_EQ(sectionTitle(r, demo->body[0]), std::string("POWER SUPPLY"));
    // A title is raw text: spaces and punctuation included, '//' not a comment.
    CHECK_EQ(sectionTitle(r, demo->body[3]), std::string("INPUTS / OUTPUTS"));
    CHECK_EQ(sectionTitle(r, demo->body[6]),
             std::string("HOUSEKEEPING // not a comment: a title runs to the end of the line"));

    const Item* inner = demo->body[5].item;
    CHECK(inner->kind == ItemKind::Block);
    CHECK_EQ(sectionTitle(r, inner->body[0]), std::string("ANALOG FRONT END"));
    CHECK_EQ(sectionTitle(r, inner->body[2]), std::string("DIGITAL"));

    // A block with no markers has none.
    for (const BodyEntry& e : r->unit.items[2]->body) CHECK(e.kind != BodyKind::Section);
}

TEST_CASE("rev 1.3: a section title is trimmed of trailing whitespace only") {
    auto r = parse("block b {\n    ---   twin  spaced title   \n    A = B;\n};");
    expectClean(r, "trimmed title");
    CHECK_EQ(sectionTitle(r, r->unit.items[0]->body[0]), std::string("twin  spaced title"));
}

TEST_CASE("rev 1.3: a bare '---' inside a block needs a title") {
    auto r = parse("block b {\n    ---\n    A = B;\n};");
    CHECK(r->report.find("needs a title") != std::string::npos);
    // The parser recovers past the marker; the statement after it survives.
    CHECK_EQ(r->unit.items[0]->body.size(), std::size_t{1});
    CHECK(r->unit.items[0]->body[0].kind == BodyKind::Stmt);
}

TEST_CASE("rev 1.3: a section marker is legal only in a block body") {
    const char* offenders[] = {
        "part p {\n    --- PINS\n    1: A &CASUAL;\n};",
        "harness h {\n    --- WIRES\n    SCL;\n};",
        "netclass n {\n    --- RULES\n    &LENGTH = 5mm;\n};",
        "match m {\n    --- LANES\n    #len = 5mm;\n};",
        "cable c {\n    --- CORES\n    A = B;\n};",
    };
    for (const char* text : offenders) {
        auto r = parse(text);
        CHECK(r->report.find("only legal in a block body") != std::string::npos);
    }
}

TEST_CASE("rev 1.3: '----' and a mid-line '---' are not section markers") {
    // The separator after '---' is required, and the marker must be the first
    // non-whitespace on its line.
    auto quad = parse("block b {\n    ----\n};");
    CHECK(quad->diags->errorCount() > 0);
    auto glued = parse("block b {\n    ---GLUED\n};");
    CHECK(glued->diags->errorCount() > 0);
    auto mid = parse("block b { A = B; --- MID\n};");
    CHECK(mid->diags->errorCount() > 0);
}

TEST_MAIN()
