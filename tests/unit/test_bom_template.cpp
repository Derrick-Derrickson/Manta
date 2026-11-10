// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The TOML subset and the user-defined BOM it describes.
#include <string>

#include "diag/engine.h"
#include "harness.h"
#include "link/bom_template.h"
#include "toml/toml.h"

using namespace manta;

namespace {

Component part(std::string ref, std::string partName, std::string value, std::string mpn,
               bool fitted = true) {
    Component c;
    c.path = {ref};
    c.designator = ref;
    c.identity = ref;
    c.partName = std::move(partName);
    c.footprint = "R-0603";
    c.fitted = fitted;
    c.fields.emplace_back("value", std::move(value));
    if (!mpn.empty()) c.fields.emplace_back("mpn", std::move(mpn));
    return c;
}

Design board() {
    Design d;
    d.top = "board";
    d.components.push_back(part("R10", "R-10k", "10k", "RC10K"));
    d.components.push_back(part("R2", "R-10k", "10k", "RC10K"));
    d.components.push_back(part("R3", "R-1k", "1k", "RC1K"));
    d.components.push_back(part("R4", "R-10k", "10k", "RC10K", false));
    return d;
}

// Loads `text` and renders board(); returns the CSV, or "ERROR" with the
// diagnostics counted in `errors`.
std::string bomOf(std::string_view text, const Design& design, std::size_t* errors = nullptr) {
    SourceManager sources;
    DiagEngine diags(sources);
    TomlValue root;
    std::string error;
    std::string out;
    BomTemplate tpl;
    bool ok = parseToml(text, root, error) && loadBomTemplate(root, "t.toml", diags, tpl) &&
              renderBomTemplate(design, tpl, nullptr, diags, out);
    if (errors) *errors = diags.errorCount() + (error.empty() ? 0 : 1);
    return ok ? out : "ERROR";
}

constexpr std::string_view kGrouped = R"(
[fields]
carry = ["*"]          # another tool's; manta ignores it

[[output]]
kind     = "bom"
group_by = ["{value}", "{footprint}"]
columns  = [
    ["Comment", "{value}"],
    ["Designator", "{refs}"],
    ["Qty", "{qty}"],
    ["MPN", "{field:mpn}"],
    ["Note", "{field:note|none}"],
]

[[output]]
kind    = "placement"
columns = [["X", "{x:.4}"]]
)";

}  // namespace

TEST_CASE("toml: tables, arrays of tables, strings and nested arrays in file order") {
    TomlValue root;
    std::string error;
    CHECK(parseToml("a = 'lit\\eral'\nb = \"t\\tab\"\nn = -42\nf = true\n"
                    "[[o]]\nx = [[\"h\", \"v\"],]\n[[o]]\nx = []\n",
                    root, error));
    CHECK_EQ(root.find("a")->string, std::string("lit\\eral"));
    CHECK_EQ(root.find("b")->string, std::string("t\tab"));
    CHECK_EQ(root.find("n")->integer, std::int64_t{-42});
    CHECK(root.find("f")->boolean);
    CHECK_EQ(root.find("o")->array.size(), std::size_t{2});
    CHECK_EQ(root.find("o")->array[0].find("x")->array[0].array[1].string, std::string("v"));
}

TEST_CASE("toml: what it does not support is refused with a line, not misread") {
    TomlValue root;
    std::string error;
    CHECK(!parseToml("\na.b = 1\n", root, error));
    CHECK_EQ(error.substr(0, 7), std::string("line 2:"));
    CHECK(!parseToml("x = { a = 1 }\n", root, error));
    CHECK(!parseToml("x = 1.5\n", root, error));
    CHECK(!parseToml("x = 1\nx = 2\n", root, error));
    CHECK(!parseToml("x = \"open\n", root, error));
}

TEST_CASE("a grouped BOM: natural ref order, DNP at quantity zero, defaults, quoting") {
    std::string out = bomOf(kGrouped, board());
    CHECK_EQ(out, std::string("Comment,Designator,Qty,MPN,Note\n"
                              "10k,\"R2,R4,R10\",2,RC10K,none\n"
                              "1k,R3,1,RC1K,none\n"));
}

TEST_CASE("skip = [\"dnp\"] leaves unfitted parts off") {
    std::string text(kGrouped);
    text.replace(text.find("kind     = \"bom\""), 16, "kind = \"bom\"\nskip = [\"dnp\"]");
    CHECK(bomOf(text, board()).find("\"R2,R10\",2") != std::string::npos);
}

TEST_CASE("a column that differs within a group is an error, not the first value") {
    Design d = board();
    d.components[1].fields[1].second = "OTHER";
    std::size_t errors = 0;
    CHECK_EQ(bomOf(kGrouped, d, &errors), std::string("ERROR"));
    CHECK_EQ(errors, std::size_t{1});
}

TEST_CASE("no group_by: one row per component, in natural ref order") {
    std::string out = bomOf("[[output]]\nkind = \"bom\"\nseparator = \"\\t\"\n"
                            "columns = [[\"Ref\", \"{ref}\"], [\"Part\", \"{part}\"], "
                            "[\"Fit\", \"{fitted}\"]]\n",
                            board());
    CHECK_EQ(out, std::string("Ref\tPart\tFit\nR2\tR-10k\tTRUE\nR3\tR-1k\tTRUE\n"
                              "R4\tR-10k\tFALSE\nR10\tR-10k\tTRUE\n"));
}

TEST_CASE("template mistakes are refused when the file loads") {
    auto refused = [](std::string column) {
        std::size_t errors = 0;
        std::string text = "[[output]]\nkind = \"bom\"\ncolumns = [[\"H\", \"" + column + "\"]]\n";
        return bomOf(text, board(), &errors) == "ERROR" && errors == 1;
    };
    CHECK(refused("{x}"));             // placement only
    CHECK(refused("{nonsense}"));
    CHECK(refused("{value:.3}"));      // takes no argument
    CHECK(refused("{field}"));         // needs a name
    CHECK(refused("{value"));
    CHECK(refused("}"));
    CHECK(!refused("{{literal}}"));

    std::size_t errors = 0;
    CHECK_EQ(bomOf("[[output]]\nkind = \"bom\"\nquote = \"never\"\n"
                   "columns = [[\"D\", \"{refs}\"]]\n",
                   board(), &errors),
             std::string("ERROR"));
    CHECK_EQ(bomOf("[[output]]\nkind = \"bom\"\ncolums = []\n", board(), &errors),
             std::string("ERROR"));
    CHECK_EQ(bomOf("[[output]]\nkind = \"placement\"\n", board(), &errors), std::string("ERROR"));
}

TEST_CASE("natural order compares digit runs as numbers") {
    CHECK(naturalLess("R2", "R10"));
    CHECK(!naturalLess("R10", "R2"));
    CHECK(naturalLess("C9", "R1"));
    CHECK(naturalLess("R1", "R1A"));
    CHECK(naturalLess("BLK2_R1", "BLK10_R1"));
}

TEST_MAIN()
