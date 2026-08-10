// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The formatter against spec 17: indentation only, the author's line structure
// untouched. Each case feeds a file through the same lex-parse-format sequence
// the driver runs and compares whole output, so a stray rewrite anywhere on a
// line -- not just a wrong indent -- fails the test.
#include "fmt/formatter.h"

#include <string>

#include "harness.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace manta;

namespace {

// Formats text as the driver does, requiring a clean parse first.
std::string format(const std::string& text) {
    SourceManager sources;
    const SourceFile* file = sources.addVirtual("<test>", text);
    DiagEngine diags(sources);
    Lexer lexer(*file, diags);
    TokenStream tokens = lexer.run();
    Arena arena;
    StringInterner interner;
    Parser parser(tokens, *file, arena, interner, diags);
    static_cast<void>(parser.run());
    if (diags.errorCount() != 0) {
        ::mantatest::fail(__FILE__, __LINE__, "fixture does not parse:\n" + text);
        return text;
    }
    return formatSource(tokens, *file);
}

}  // namespace

TEST_CASE("a sensibly indented file is already canonical") {
    const std::string canonical =
        "// blinky-ish header comment\n"
        "block b {\n"
        "    GND &TYPE=GROUND;\n"
        "\n"
        "    --- POWER\n"
        "\n"
        "    // a comment above a device\n"
        "    {U1~res:\n"
        "        A = GND;\n"
        "        B = VBUS;\n"
        "    };\n"
        "\n"
        "    X = .{R1~res}.\n"
        "        = Y\n"
        "        &CURRENT=1A;\n"
        "};\n";
    CHECK_EQ(format(canonical), canonical);
}

TEST_CASE("only leading whitespace is rewritten") {
    // Mis-indented everywhere; alignment and spacing within each line must
    // survive, including the two-space '=' alignment and trailing spaces.
    const std::string input =
        "block b {\n"
        "GND &TYPE=GROUND;\n"
        "            {U1~res:\n"
        "  nRESET  = nRESET;   \n"
        "  GPIO-A  = LED[0];\n"
        "                };\n"
        "};\n";
    const std::string want =
        "block b {\n"
        "    GND &TYPE=GROUND;\n"
        "    {U1~res:\n"
        "        nRESET  = nRESET;   \n"
        "        GPIO-A  = LED[0];\n"
        "    };\n"
        "};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("lines are never joined or split") {
    // The same binding list one-lined and five-lined: both are canonical.
    const std::string oneLine = "block b {\n    {U1~res: A = X; B = Y;};\n};\n";
    CHECK_EQ(format(oneLine), oneLine);

    const std::string fiveLines =
        "block b {\n"
        "    {U1~res:\n"
        "        A = X;\n"
        "        B = Y;\n"
        "    };\n"
        "};\n";
    CHECK_EQ(format(fiveLines), fiveLines);
}

TEST_CASE("continuation lines take one unit past the depth") {
    const std::string input =
        "block b {\n"
        "    SW == SW-NODE\n"
        "= .{L1~res}.\n"
        "                == 3V3\n"
        "   &CURRENT=3A;\n"
        "};\n";
    const std::string want =
        "block b {\n"
        "    SW == SW-NODE\n"
        "        = .{L1~res}.\n"
        "        == 3V3\n"
        "        &CURRENT=3A;\n"
        "};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("a binding broken after '=' continues; the list body does not") {
    // ':' finishes the binding-list head and ';' finishes each binding, so the
    // body sits at bracket depth; a binding split mid-way continues one deeper.
    const std::string input =
        "block b {\n"
        "    {U1~res:\n"
        "    A =\n"
        "    X;\n"
        "    };\n"
        "};\n";
    const std::string want =
        "block b {\n"
        "    {U1~res:\n"
        "        A =\n"
        "            X;\n"
        "    };\n"
        "};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("a closer outdents to its opener") {
    const std::string input =
        "block b {\n"
        "    X = (\n"
        "        Y = .{R1~res}.\n"
        "        ) = Z;\n"
        "    };\n";
    const std::string want =
        "block b {\n"
        "    X = (\n"
        "        Y = .{R1~res}.\n"
        "    ) = Z;\n"
        "};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("'}' and ';' split across lines stay split") {
    // The ';' still belongs to the unfinished statement, so it continues.
    const std::string input =
        "block b {\n"
        "    {U1~res: A = X;\n"
        "    }\n"
        "    ;\n"
        "};\n";
    const std::string want =
        "block b {\n"
        "    {U1~res: A = X;\n"
        "    }\n"
        "        ;\n"
        "};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("brackets inside strings do not count") {
    const std::string input =
        "block b {\n"
        "  #note = \"{ ( [ } never counted\";\n"
        "  GND &TYPE=GROUND;\n"
        "};\n";
    const std::string want =
        "block b {\n"
        "    #note = \"{ ( [ } never counted\";\n"
        "    GND &TYPE=GROUND;\n"
        "};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("comment-only lines take the depth; block-comment interiors are untouched") {
    const std::string input =
        "block b {\n"
        "            // a stray line comment\n"
        "  /* art:\n"
        "        aligned by hand\n"
        "     never touched */\n"
        "    GND &TYPE=GROUND; // beside code, stays put\n"
        "};\n";
    const std::string want =
        "block b {\n"
        "    // a stray line comment\n"
        "    /* art:\n"
        "        aligned by hand\n"
        "     never touched */\n"
        "    GND &TYPE=GROUND; // beside code, stays put\n"
        "};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("section markers take body depth, with no blank line invented") {
    const std::string input =
        "block b {\n"
        "    GND &TYPE=GROUND;\n"
        "--- POWER RAILS\n"
        "    VBUS &CLASS=power;\n"
        "};\n";
    const std::string want =
        "block b {\n"
        "    GND &TYPE=GROUND;\n"
        "    --- POWER RAILS\n"
        "    VBUS &CLASS=power;\n"
        "};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("blank lines survive; whitespace-only lines are emptied") {
    const std::string input =
        "block b {\n"
        "\n"
        "    GND &TYPE=GROUND;\n"
        "        \n"
        "\n"
        "};\n"
        "\n";
    const std::string want =
        "block b {\n"
        "\n"
        "    GND &TYPE=GROUND;\n"
        "\n"
        "\n"
        "};\n"
        "\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("CRLF becomes LF and a missing final newline is added") {
    const std::string input = "block b {\r\n  GND &TYPE=GROUND;\r\n};";
    const std::string want = "block b {\n    GND &TYPE=GROUND;\n};\n";
    CHECK_EQ(format(input), want);
}

TEST_CASE("the end-of-content marker and everything after it are byte-for-byte") {
    const std::string tail =
        "---\n"
        "# not manta\n"
        "  trailing spaces kept:   \n"
        "\tblock b { unparsed };\r\n"
        "no final newline";
    const std::string input = "part p {\n1 = A;\n};\n" + tail;
    const std::string want = "part p {\n    1 = A;\n};\n" + tail;
    CHECK_EQ(format(input), want);
}

TEST_CASE("formatting is idempotent") {
    const std::string input =
        "// header\n"
        "block b {\n"
        "GND &TYPE=GROUND;\n"
        "--- SECTION ONE\n"
        "  /* multi\n"
        "line */\n"
        "        X = .{R1~res}.\n"
        "  = Y;\n"
        "  {U1~res: #note = \"}\";\n"
        "  B = X;\n"
        "  };\n"
        "};\n"
        "---\n"
        "tail   \n";
    std::string once = format(input);
    CHECK_EQ(format(once), once);
}

TEST_CASE("unifiedDiff is empty exactly when nothing changed") {
    CHECK_EQ(unifiedDiff("a\nb\n", "a\nb\n", "f"), std::string{});
    std::string diff = unifiedDiff("a\nb\n", "a\nc\n", "f");
    CHECK(diff.find("--- f") != std::string::npos);
    CHECK(diff.find("-b") != std::string::npos);
    CHECK(diff.find("+c") != std::string::npos);
}

TEST_MAIN()
