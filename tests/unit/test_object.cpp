// The .mantaO round trip (spec 15.2, 15.4) and its determinism (spec 15.8).
//
// Writing, reading and writing again must produce byte-identical JSON. That one
// property is what keeps the writer and reader from drifting apart as the AST
// grows: a field added to one and not the other fails here immediately.
#include <string>

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

TEST_MAIN()
