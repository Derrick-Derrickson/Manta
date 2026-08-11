// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The .mantaO round trip (spec 15.2, 15.4) and its determinism (spec 15.8).
//
// Writing, reading and writing again must produce byte-identical JSON. That one
// property is what keeps the writer and reader from drifting apart as the AST
// grows: a field added to one and not the other fails here immediately.
#include <span>
#include <string>
#include <vector>

#include "harness.h"
#include "json/json.h"
#include "lex/lexer.h"
#include "link/symbols.h"
#include "obj/mantao.h"
#include "parse/parser.h"

using namespace manta;

namespace {

std::string readFixture(const char* name) {
    std::string path = std::string(MANTA_TEST_DIR) + "/spec/" + name;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Compiles text to a .mantaO string.
std::string compileToObject(const std::string& text, std::size_t* errors = nullptr) {
    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    const SourceFile* file = sources.addVirtual("fixture.manta", text);
    Lexer lexer(*file, diags);
    TokenStream toks = lexer.run();
    Parser parser(toks, *file, arena, interner, diags);
    SourceUnit unit = parser.run();
    if (errors) *errors = diags.errorCount();
    std::string out;
    writeObject(unit, interner, "fixture.manta", out);
    return out;
}

// Reads a .mantaO string and writes it back out.
std::string reserialise(const std::string& objectText, bool* ok) {
    JsonParseError err;
    JsonPtr root = jsonParse(objectText, err);
    if (!root) {
        *ok = false;
        return {};
    }
    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    const SourceFile* stand_in = sources.addVirtual("fixture.manta", "");
    ObjectFile obj;
    *ok = readObject(*root, arena, interner, stand_in->id(), diags, Span{stand_in->id(), 0, 0}, obj);
    if (!*ok) return {};
    std::string out;
    writeObject(obj.unit, interner, obj.sourcePath, out);
    return out;
}

// The same identity check, on source given inline rather than as a fixture.
void checkRoundTripOf(const char* what, const std::string& text) {
    std::size_t errors = 0;
    std::string first = compileToObject(text, &errors);
    if (errors != 0) {
        ::mantatest::fail(__FILE__, __LINE__, std::string(what) + " failed to compile");
        return;
    }
    bool ok = false;
    std::string second = reserialise(first, &ok);
    if (!ok) {
        ::mantatest::fail(__FILE__, __LINE__, std::string(what) + " failed to read back");
        return;
    }
    if (first != second) {
        ::mantatest::fail(__FILE__, __LINE__, std::string(what) + ": round trip differs\n" +
                                                  first + "\n---\n" + second);
    }
}

void checkRoundTrip(const char* fixture) {
    std::string text = readFixture(fixture);
    if (text.empty()) {
        ::mantatest::fail(__FILE__, __LINE__, std::string("could not read fixture ") + fixture);
        return;
    }
    std::size_t errors = 0;
    std::string first = compileToObject(text, &errors);
    if (errors != 0) {
        ::mantatest::fail(__FILE__, __LINE__, std::string(fixture) + " failed to compile");
        return;
    }
    bool ok = false;
    std::string second = reserialise(first, &ok);
    if (!ok) {
        ::mantatest::fail(__FILE__, __LINE__, std::string(fixture) + " failed to read back");
        return;
    }
    if (first != second) {
        // Report the first differing line, which localises the drifted field.
        std::size_t line = 1;
        std::size_t i = 0;
        while (i < first.size() && i < second.size() && first[i] == second[i]) {
            if (first[i] == '\n') ++line;
            ++i;
        }
        ::mantatest::fail(__FILE__, __LINE__,
                          std::string(fixture) + ": round trip differs at line " +
                              std::to_string(line) + "\n      wrote: " +
                              first.substr(i > 40 ? i - 40 : 0, 90) + "\n      read : " +
                              second.substr(i > 40 ? i - 40 : 0, 90));
    }
}

// ---------------------------------------------------------------------------
// A small AST builder (revision 1.4)
//
// A binding carrying a chain is produced by the parser, so a test that went
// through source would only pass once the parser lands. These build the tree
// directly, which keeps the object format's own coverage independent of the
// front end -- exactly the split the round trip is meant to police.
// ---------------------------------------------------------------------------

struct Builder {
    Arena arena;
    StringInterner interner;

    template <typename T>
    std::span<T> copy(const std::vector<T>& v) {
        auto dst = arena.makeArray<T>(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) dst[i] = v[i];
        return dst;
    }

    Name name(std::string_view text) {
        Name n;
        n.symbol = interner.intern(text);
        return n;
    }

    NetExpr* net(std::string_view text) {
        auto* n = arena.make<NetExpr>();
        n->path = copy(std::vector<Name>{name(text)});
        return n;
    }

    Element* netElement(std::string_view text) {
        auto* e = arena.make<Element>();
        e->kind = ElementKind::Net;
        e->net = net(text);
        return e;
    }

    Instance* instance(std::string_view prefix, std::int64_t number, std::string_view part,
                       std::vector<Binding*> bindings) {
        auto* i = arena.make<Instance>();
        i->designator.prefix = name(prefix);
        i->designator.kind = DesignatorKind::Numbered;
        i->designator.number = number;
        i->declares = true;
        i->partOrBlock = name(part);
        i->bindings = copy(bindings);
        return i;
    }

    Element* deviceElement(Instance* inst, bool dotEntry = false) {
        auto* d = arena.make<Device>();
        if (dotEntry) {
            d->entry.dot = true;
            d->hasEntry = true;
        }
        d->instance = inst;
        auto* e = arena.make<Element>();
        e->kind = ElementKind::Device;
        e->device = d;
        return e;
    }

    Segment* segment(std::vector<Element*> elements, std::vector<Connector> connectors) {
        auto* s = arena.make<Segment>();
        s->elements = copy(elements);
        s->connectors = copy(connectors);
        return s;
    }

    Directive* directive(std::string_view text) {
        auto* d = arena.make<Directive>();
        d->name = name(text);
        return d;
    }

    // A pin binding. `net` and `rhs` are the two spellings of one slot, so a
    // caller passes at most one of them.
    Binding* pinBinding(std::string_view pin, Connector connector, NetExpr* n, Segment* rhs) {
        auto* b = arena.make<Binding>();
        b->kind = BindingKind::PinNet;
        b->pin = name(pin);
        b->connector = connector;
        b->net = n;
        b->rhs = rhs;
        return b;
    }

    // Wraps one segment in the block/statement/chain scaffolding an object
    // needs, and returns the finished unit.
    SourceUnit unitWith(Segment* top) {
        auto* chain = arena.make<Chain>();
        chain->segments = copy(std::vector<Segment*>{top});
        auto* stmt = arena.make<Stmt>();
        stmt->kind = StmtKind::Chain;
        stmt->chain = chain;
        BodyEntry entry;
        entry.kind = BodyKind::Stmt;
        entry.stmt = stmt;
        auto* item = arena.make<Item>();
        item->kind = ItemKind::Block;
        item->name = name("b");
        item->body = copy(std::vector<BodyEntry>{entry});
        SourceUnit unit;
        unit.items = copy(std::vector<Item*>{item});
        return unit;
    }
};

// Reads an object and hands back the first binding of the first device in it,
// or nullptr when the shape is not the one these tests build.
const Binding* firstBinding(const ObjectFile& obj) {
    if (obj.unit.items.empty()) return nullptr;
    const Item* item = obj.unit.items[0];
    if (item->body.empty() || item->body[0].kind != BodyKind::Stmt) return nullptr;
    const Stmt* stmt = item->body[0].stmt;
    if (!stmt || !stmt->chain || stmt->chain->segments.empty()) return nullptr;
    const Segment* seg = stmt->chain->segments[0];
    if (!seg || seg->elements.empty()) return nullptr;
    const Element* e = seg->elements[0];
    if (e->kind != ElementKind::Device || !e->device || !e->device->instance) return nullptr;
    const Instance* inst = e->device->instance;
    if (inst->bindings.empty()) return nullptr;
    return inst->bindings[0];
}

// Parses an object string into `obj`, reporting through the harness.
bool readInto(const std::string& objectText, SourceManager& sources, Arena& arena,
              StringInterner& interner, DiagEngine& diags, ObjectFile& obj) {
    JsonParseError err;
    JsonPtr root = jsonParse(objectText, err);
    if (!root) return false;
    const SourceFile* stand_in = sources.addVirtual("fixture.manta", "");
    return readObject(*root, arena, interner, stand_in->id(), diags, Span{stand_in->id(), 0, 0},
                      obj);
}

// The byte-identity check, on a unit built rather than parsed.
void checkAstRoundTrip(const char* what, const SourceUnit& unit, const StringInterner& interner) {
    std::string first;
    writeObject(unit, interner, "fixture.manta", first);
    bool ok = false;
    std::string second = reserialise(first, &ok);
    if (!ok) {
        ::mantatest::fail(__FILE__, __LINE__, std::string(what) + " failed to read back");
        return;
    }
    if (first != second) {
        ::mantatest::fail(__FILE__, __LINE__, std::string(what) + ": round trip differs\n" +
                                                  first + "\n---\n" + second);
    }
}

}  // namespace

TEST_CASE("spec 15.2: parts round-trip through .mantaO") {
    checkRoundTrip("parts.manta");
}

TEST_CASE("spec 15.2: harnesses, netclasses and matches round-trip") {
    checkRoundTrip("types.manta");
}

TEST_CASE("spec 15.2: a block with substitutions round-trips unevaluated") {
    // Spec 14.1: "A .mantaO object carries substitutions unevaluated."
    checkRoundTrip("rc-filter.manta");
}

TEST_CASE("spec 15.2: the complete board round-trips") {
    checkRoundTrip("board.manta");
    checkRoundTrip("cable.manta");
}

TEST_CASE("rev 1.3: section markers round-trip through .mantaO") {
    checkRoundTrip("sections.manta");
}

TEST_CASE("a range value survives the object round trip") {
    // '1:20' inside a list is what keeps a twenty-way pin map to one pair. A
    // range may descend -- that is how a reversed map is written -- so the
    // order of its endpoints is part of its meaning and must not be normalised.
    checkRoundTripOf("range value", R"(part p {
    @~footprint = F;
    #map = [[1:20],[20:1]];
    #mixed = [3, 7:9, 4];
    1 = A &CASUAL;
    2 = B &CASUAL;
};
)");
}

TEST_CASE("spec 15.8: compiling twice gives byte-identical objects") {
    std::string text = readFixture("board.manta");
    CHECK_EQ(compileToObject(text), compileToObject(text));
}

TEST_CASE("spec 15.4: the object declares its kind and language revision") {
    std::string obj = compileToObject("block b { A == B; };");
    JsonParseError err;
    JsonPtr root = jsonParse(obj, err);
    CHECK(root != nullptr);
    if (!root) return;
    CHECK_EQ(root->str("kind"), std::string_view("mantaO"));
    CHECK_EQ(root->str("version"), kLanguageVersion);
}

TEST_CASE("an object from a newer revision is rejected") {
    std::string obj = compileToObject("block b { A == B; };");
    // Rewrite the version field to a revision this toolchain does not implement.
    std::string current = "\"" + std::string(kLanguageVersion) + "\"";
    std::size_t at = obj.find(current);
    CHECK(at != std::string::npos);
    obj.replace(at, current.size(), "\"9.9\"");

    JsonParseError err;
    JsonPtr root = jsonParse(obj, err);
    CHECK(root != nullptr);
    if (!root) return;

    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    const SourceFile* f = sources.addVirtual("x", "");
    ObjectFile out;
    bool ok = readObject(*root, arena, interner, f->id(), diags, Span{f->id(), 0, 0}, out);
    CHECK_FALSE(ok);
    CHECK(diags.errorCount() > 0);
}

TEST_CASE("json writer escapes and the parser round-trips it") {
    std::string out;
    {
        JsonWriter w(out, false);
        w.beginObject();
        w.field("a", "quote\" backslash\\ newline\n tab\t");
        w.field("b", std::int64_t{-42});
        w.field("c", true);
        w.key("d");
        w.beginArray();
        w.value("x");
        w.value(std::int64_t{1});
        w.endArray();
        w.endObject();
    }
    JsonParseError err;
    JsonPtr v = jsonParse(out, err);
    CHECK(v != nullptr);
    if (!v) return;
    CHECK_EQ(v->str("a"), std::string_view("quote\" backslash\\ newline\n tab\t"));
    CHECK_EQ(v->integer("b"), std::int64_t{-42});
    CHECK(v->boolean_("c"));
    CHECK_EQ(v->arr("d")->array.size(), std::size_t{2});
}

TEST_CASE("json objects preserve insertion order, never hash order") {
    // Spec 15.8 depends on this: emitted key order must be the order written.
    std::string out;
    {
        JsonWriter w(out, false);
        w.beginObject();
        w.field("zebra", std::int64_t{1});
        w.field("alpha", std::int64_t{2});
        w.field("middle", std::int64_t{3});
        w.endObject();
    }
    CHECK_EQ(out, std::string(R"({"zebra":1,"alpha":2,"middle":3})"));

    JsonParseError err;
    JsonPtr v = jsonParse(out, err);
    CHECK(v != nullptr);
    if (!v) return;
    CHECK_EQ(v->object.size(), std::size_t{3});
    CHECK_EQ(v->object[0].first, std::string("zebra"));
    CHECK_EQ(v->object[2].first, std::string("middle"));
}

TEST_CASE("the toolchain revision is parsed from kLanguageVersion, not assumed") {
    Revision r;
    CHECK(Revision::parse("1.1", r));
    CHECK_EQ(r.major, std::uint32_t{1});
    CHECK_EQ(r.minor, std::uint32_t{1});
    CHECK(Revision::parse("10.20", r));
    CHECK_EQ(r.major, std::uint32_t{10});
    CHECK_EQ(r.minor, std::uint32_t{20});
    CHECK_FALSE(Revision::parse("1", r));
    CHECK_FALSE(Revision::parse("1.x", r));
    CHECK_FALSE(Revision::parse(".1", r));

    CHECK_EQ(Revision::toolchain().text(), std::string(kLanguageVersion));
}

TEST_CASE("an object from an older revision is still readable") {
    // 1.1 only added a lexical marker, so a 1.0 object contains nothing this
    // toolchain cannot read. Rejecting it would break every existing build.
    CHECK(revisionAtMost("1.0", "1.1"));
    CHECK(revisionAtMost("1.1", "1.1"));
    CHECK_FALSE(revisionAtMost("1.2", "1.1"));
    CHECK_FALSE(revisionAtMost("2.0", "1.1"));
    CHECK_FALSE(revisionAtMost("nonsense", "1.1"));

    std::string obj = compileToObject("block b { A == B; };");
    std::string current = "\"" + std::string(kLanguageVersion) + "\"";
    obj.replace(obj.find(current), current.size(), "\"1.0\"");

    JsonParseError err;
    JsonPtr root = jsonParse(obj, err);
    CHECK(root != nullptr);
    if (!root) return;

    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    const SourceFile* f = sources.addVirtual("x", "");
    ObjectFile out;
    CHECK(readObject(*root, arena, interner, f->id(), diags, Span{f->id(), 0, 0}, out));
    CHECK_EQ(diags.errorCount(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Revision 1.4: a binding carries a chain, not only a net
// ---------------------------------------------------------------------------

TEST_CASE("rev 1.4: a binding chain round-trips under each connector") {
    // "PIN <connector> <segment>" in a binding list means what
    // "<designator>.PIN <connector> <segment>;" means in the enclosing body, so
    // every connector of spec 6 may open a binding.
    const Connector connectors[] = {Connector::Advance, Connector::Same, Connector::Gather,
                                    Connector::Broadcast};
    const char* spelling[] = {"=", "==", "=*", "*="};

    for (std::size_t k = 0; k < 4; ++k) {
        Builder b;
        // VIN <c> VPOS == .{C1~C-10uF: . = GND;}
        auto* inner = b.instance("C", 1, "C-10uF",
                                 {b.pinBinding("A", Connector::Advance, b.net("GND"), nullptr)});
        Segment* rhs = b.segment({b.netElement("VPOS"), b.deviceElement(inner, /*dotEntry=*/true)},
                                 {Connector::Same});
        auto* outer = b.instance("U", 2, "regulator",
                                 {b.pinBinding("VIN", connectors[k], nullptr, rhs)});
        SourceUnit unit = b.unitWith(b.segment({b.deviceElement(outer)}, {}));

        checkAstRoundTrip(spelling[k], unit, b.interner);

        // And the connector and chain survive as themselves, not merely as
        // bytes: a writer and reader that both dropped them would still be
        // byte-identical.
        std::string text;
        writeObject(unit, b.interner, "fixture.manta", text);
        SourceManager sources;
        Arena arena;
        StringInterner interner;
        DiagEngine diags(sources);
        ObjectFile obj;
        CHECK(readInto(text, sources, arena, interner, diags, obj));
        const Binding* got = firstBinding(obj);
        CHECK(got != nullptr);
        if (!got) continue;
        CHECK(got->connector == connectors[k]);
        CHECK(got->rhs != nullptr);
        CHECK(got->net == nullptr);
        if (!got->rhs) continue;
        CHECK_EQ(got->rhs->elements.size(), std::size_t{2});
        CHECK_EQ(got->rhs->connectors.size(), std::size_t{1});
        CHECK(got->rhs->connectors[0] == Connector::Same);
    }
}

TEST_CASE("rev 1.4: a binding chain may nest a device with its own bindings") {
    // The nested instance carries a binding list of its own, so the encoding
    // has to recurse: binding -> segment -> device -> instance -> binding.
    Builder b;
    auto* deep = b.instance("R", 9, "R-100kR",
                            {b.pinBinding("A", Connector::Advance, b.net("VPOS"), nullptr)});
    Segment* rhs = b.segment({b.deviceElement(deep, /*dotEntry=*/true)}, {});
    auto* outer = b.instance("U", 1, "mcu", {b.pinBinding("EN", Connector::Advance, nullptr, rhs)});
    SourceUnit unit = b.unitWith(b.segment({b.deviceElement(outer)}, {}));

    checkAstRoundTrip("nested binding list", unit, b.interner);

    std::string text;
    writeObject(unit, b.interner, "fixture.manta", text);
    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    ObjectFile obj;
    CHECK(readInto(text, sources, arena, interner, diags, obj));
    const Binding* got = firstBinding(obj);
    CHECK(got != nullptr);
    if (!got || !got->rhs || got->rhs->elements.empty()) return;
    const Element* e = got->rhs->elements[0];
    CHECK(e->kind == ElementKind::Device);
    if (e->kind != ElementKind::Device) return;
    CHECK(e->device->hasEntry);
    CHECK(e->device->entry.dot);
    CHECK_EQ(e->device->instance->bindings.size(), std::size_t{1});
    CHECK(e->device->instance->bindings[0]->net != nullptr);
    CHECK(e->device->instance->bindings[0]->rhs == nullptr);
}

TEST_CASE("a 'PIN = ?' unbind binding round-trips with neither net nor chain") {
    // Spec 11.6: the pin is deliberately floating, so there is no right-hand
    // side at all -- neither encoding is written.
    Builder b;
    auto* binding = b.pinBinding("GNDB", Connector::Advance, nullptr, nullptr);
    binding->unbind = true;
    auto* inst = b.instance("U", 1, "mcu", {binding});
    SourceUnit unit = b.unitWith(b.segment({b.deviceElement(inst)}, {}));

    checkAstRoundTrip("unbind binding", unit, b.interner);

    std::string text;
    writeObject(unit, b.interner, "fixture.manta", text);
    CHECK(text.find("\"rhs\"") == std::string::npos);
    CHECK(text.find("\"connector\"") == std::string::npos);

    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    ObjectFile obj;
    CHECK(readInto(text, sources, arena, interner, diags, obj));
    const Binding* got = firstBinding(obj);
    CHECK(got != nullptr);
    if (!got) return;
    CHECK(got->unbind);
    CHECK(got->net == nullptr);
    CHECK(got->rhs == nullptr);
}

TEST_CASE("a binding of directives alone round-trips with no right-hand side") {
    // "{U5~ddr-chip: DQ[0] &PINDELAY=18ps; }" (spec 11.5): the pin is named to
    // be spoken about, not connected.
    Builder b;
    auto* binding = b.pinBinding("DQ", Connector::Advance, nullptr, nullptr);
    binding->pinDirectives = b.copy(std::vector<Directive*>{b.directive("PINDELAY")});
    auto* inst = b.instance("U", 5, "ddr-chip", {binding});
    SourceUnit unit = b.unitWith(b.segment({b.deviceElement(inst)}, {}));

    checkAstRoundTrip("directive-only binding", unit, b.interner);

    std::string text;
    writeObject(unit, b.interner, "fixture.manta", text);
    CHECK(text.find("\"rhs\"") == std::string::npos);
    CHECK(text.find("\"net\"") == std::string::npos);
    CHECK(text.find("\"connector\"") == std::string::npos);

    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    ObjectFile obj;
    CHECK(readInto(text, sources, arena, interner, diags, obj));
    const Binding* got = firstBinding(obj);
    CHECK(got != nullptr);
    if (!got) return;
    CHECK(got->net == nullptr);
    CHECK(got->rhs == nullptr);
    CHECK(got->connector == Connector::Advance);
    CHECK_EQ(got->pinDirectives.size(), std::size_t{1});
}

TEST_CASE("rev 1.4: a plain binding is written exactly as 1.3 wrote it") {
    // The two new keys are written only when they carry something, so the
    // overwhelmingly common binding -- one bare net reached through '=' --
    // serialises to the bytes an earlier toolchain produced.
    std::size_t errors = 0;
    std::string obj = compileToObject("block b {\n    {U1~mcu:\n        GND = AGND;\n"
                                      "        VCC = VBUS;\n    };\n};\n", &errors);
    CHECK_EQ(errors, std::size_t{0});
    CHECK(obj.find("\"connector\"") == std::string::npos);
    CHECK(obj.find("\"rhs\"") == std::string::npos);
    CHECK(obj.find("\"net\"") != std::string::npos);
}

// A .mantaO exactly as revision 1.3 wrote one: a binding with a bare 'net' and
// neither of the keys 1.4 added. Written out literally rather than produced by
// today's writer, so it keeps testing 1.3's encoding after the writer moves on.
constexpr const char* kLegacyObject = R"({
  "version": "1.3",
  "kind": "mantaO",
  "source": "legacy.manta",
  "declarations": [
    {
      "kind": "block",
      "name": "b",
      "static": false,
      "body": [
        {
          "stmt": {
            "s": "chain",
            "segments": [
              {
                "elements": [
                  {
                    "e": "device",
                    "device": {
                      "instance": {
                        "designator": {
                          "prefix": "U",
                          "kind": "n",
                          "number": 1,
                          "assignAt": [12, 1],
                          "at": [11, 2]
                        },
                        "declares": true,
                        "part": "mcu",
                        "bindings": [
                          {
                            "b": "pin",
                            "pin": "GND",
                            "net": { "path": [{ "n": "AGND" }], "at": [26, 4] },
                            "at": [20, 10]
                          }
                        ],
                        "at": [11, 21]
                      },
                      "at": [10, 23]
                    },
                    "at": [10, 23]
                  }
                ],
                "connectors": [],
                "at": [10, 23]
              }
            ],
            "at": [10, 24]
          }
        }
      ],
      "at": [0, 36]
    }
  ]
}
)";

TEST_CASE("rev 1.4: a 1.3 object reads back as the AST 1.3 produced") {
    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    ObjectFile obj;
    CHECK(readInto(kLegacyObject, sources, arena, interner, diags, obj));
    CHECK_EQ(diags.errorCount(), std::size_t{0});
    CHECK_EQ(obj.version, std::string("1.3"));

    const Binding* got = firstBinding(obj);
    CHECK(got != nullptr);
    if (!got) return;
    // Absent keys mean what they meant before they existed.
    CHECK(got->connector == Connector::Advance);
    CHECK(got->rhs == nullptr);
    CHECK_FALSE(got->unbind);
    CHECK(got->net != nullptr);
    if (!got->net) return;
    CHECK_EQ(got->net->path.size(), std::size_t{1});
    CHECK_EQ(interner.text(got->net->path[0].symbol), std::string_view("AGND"));

    // Writing it back adds nothing: the only difference from a 1.3 emission is
    // the revision stamp, which always names the toolchain that wrote the file.
    std::string out;
    writeObject(obj.unit, interner, obj.sourcePath, out);
    CHECK(out.find("\"connector\"") == std::string::npos);
    CHECK(out.find("\"rhs\"") == std::string::npos);
    CHECK(out.find("\"net\"") != std::string::npos);
    bool ok = false;
    CHECK_EQ(reserialise(out, &ok), out);
    CHECK(ok);
}

TEST_CASE("a binding that sets both 'net' and 'rhs' is rejected") {
    // At most one of the two is ever set. An object claiming both is describing
    // an AST that cannot exist, so it is refused rather than half-read.
    std::string bad = kLegacyObject;
    const std::string net = R"("net": { "path": [{ "n": "AGND" }], "at": [26, 4] },)";
    std::size_t at = bad.find(net);
    CHECK(at != std::string::npos);
    if (at == std::string::npos) return;
    bad.replace(at, net.size(),
                net + R"("rhs": { "elements": [], "connectors": [], "at": [26, 4] },)");

    SourceManager sources;
    Arena arena;
    StringInterner interner;
    DiagEngine diags(sources);
    ObjectFile obj;
    CHECK_FALSE(readInto(bad, sources, arena, interner, diags, obj));
    CHECK(diags.errorCount() > 0);
}

TEST_MAIN()
