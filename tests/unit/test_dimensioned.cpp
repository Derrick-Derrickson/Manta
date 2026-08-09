// Spec 3.2 (dimensioned values), 3.4 (metric only), 2.3 (identifiers) and the
// canonical SI-substituted rendering the formatter emits (spec 17).
#include "lex/dimensioned.h"

#include <string>

#include "harness.h"
#include "lex/token.h"

using namespace manta;

namespace {

std::string canon(std::string_view text) {
    Dimensioned d;
    if (!parseDimensioned(text, d)) return "<not-a-value>";
    return d.canonical();
}

bool sameValue(std::string_view a, std::string_view b) {
    Dimensioned da, db;
    return parseDimensioned(a, da) && parseDimensioned(b, db) && da == db;
}

}  // namespace

TEST_CASE("spec 3.2: every table example parses") {
    CHECK_EQ(canon("100R"), std::string("100R"));
    CHECK_EQ(canon("4k7R"), std::string("4k7R"));
    CHECK_EQ(canon("1M5R"), std::string("1M5R"));
    CHECK_EQ(canon("100nF"), std::string("100nF"));
    CHECK_EQ(canon("10uF"), std::string("10uF"));
    CHECK_EQ(canon("2u2H"), std::string("2u2H"));
    CHECK_EQ(canon("100nH"), std::string("100nH"));
    CHECK_EQ(canon("3V3"), std::string("3V3"));
    CHECK_EQ(canon("-5V"), std::string("-5V"));
    CHECK_EQ(canon("48V"), std::string("48V"));
    CHECK_EQ(canon("750mA"), std::string("750mA"));
    CHECK_EQ(canon("3A"), std::string("3A"));
    CHECK_EQ(canon("250mW"), std::string("250mW"));
    CHECK_EQ(canon("100MHz"), std::string("100MHz"));
    CHECK_EQ(canon("2G4Hz"), std::string("2G4Hz"));
    CHECK_EQ(canon("5mm"), std::string("5mm"));
    CHECK_EQ(canon("100um"), std::string("100um"));
    CHECK_EQ(canon("10ns"), std::string("10ns"));
    CHECK_EQ(canon("1ms"), std::string("1ms"));
    CHECK_EQ(canon("85C"), std::string("85C"));
    CHECK_EQ(canon("-40C"), std::string("-40C"));
}

TEST_CASE("spec 3.2: both spellings are the same value") {
    CHECK(sameValue("4k7R", "4.7kR"));
    CHECK(sameValue("3V3", "3.3V"));
    CHECK(sameValue("2u2H", "2.2uH"));
    CHECK(sameValue("2G4Hz", "2.4GHz"));
}

TEST_CASE("spec 17: the formatter emits the SI-substituted form") {
    CHECK_EQ(canon("4.7kR"), std::string("4k7R"));
    CHECK_EQ(canon("3.3V"), std::string("3V3"));
    CHECK_EQ(canon("2.2uH"), std::string("2u2H"));
    CHECK_EQ(canon("2.4GHz"), std::string("2G4Hz"));
    // A value with no fractional part keeps prefix and unit adjacent.
    CHECK_EQ(canon("10kR"), std::string("10kR"));
    // With no SI prefix the unit itself stands in for the decimal point.
    CHECK_EQ(canon("1.5R"), std::string("1R5"));
}

TEST_CASE("canonical rendering is idempotent") {
    for (std::string_view v : {"4.7kR", "3.3V", "2.2uH", "2.4GHz", "100nF", "-40C", "600ps",
                               "90RD", "0.002", "1.5R", "12ps"}) {
        std::string once = canon(v);
        CHECK_EQ(canon(once), once);
    }
}

TEST_CASE("spec 11.3: &IMP takes an optional D suffix") {
    Dimensioned d;
    CHECK(parseDimensioned("90RD", d));
    CHECK(d.differential);
    CHECK_EQ(d.unit == Unit::Ohm, true);
    CHECK_EQ(canon("90RD"), std::string("90RD"));

    CHECK(parseDimensioned("50R", d));
    CHECK_FALSE(d.differential);
}

TEST_CASE("spec 3.2: 'm' is metre alone but milli before a unit") {
    Dimensioned d;
    CHECK(parseDimensioned("1m", d));
    CHECK(d.unit == Unit::Metre);
    CHECK(parseDimensioned("5mm", d));
    CHECK(d.unit == Unit::Metre);
    CHECK_EQ(canon("5mm"), std::string("5mm"));
    // Longest-match on the unit: Hz beats H.
    CHECK(parseDimensioned("100nHz", d));
    CHECK(d.unit == Unit::Hertz);
    CHECK(parseDimensioned("100nH", d));
    CHECK(d.unit == Unit::Henry);
}

TEST_CASE("an area is a length squared, and so is its prefix") {
    // 'mm2' is a square millimetre, (10^-3 m)^2 = 10^-6 m2 -- not a
    // milli-square-metre. Getting this wrong is a factor of a thousand and
    // entirely silent, which is why it is asserted on the exponent and not
    // merely on the spelling.
    Dimensioned d;
    CHECK(parseDimensioned("1mm2", d));
    CHECK(d.unit == Unit::SquareMetre);
    CHECK_EQ(d.mantissa, std::int64_t{1});
    CHECK_EQ(d.exp10, -6);

    // A bare squared unit parses: the leading 'm' is the unit, not a prefix.
    CHECK(parseDimensioned("2m2", d));
    CHECK(d.unit == Unit::SquareMetre);
    CHECK_EQ(d.exp10, 0);
    CHECK_EQ(d.mantissa, std::int64_t{2});

    // ...and a length is still a length.
    CHECK(parseDimensioned("5mm", d));
    CHECK(d.unit == Unit::Metre);
    CHECK_EQ(d.exp10, -3);
}

TEST_CASE("an area renders with an explicit point, and round-trips") {
    // '1m5m2' would be the point-substituted spelling and reads as nonsense, so
    // a squared unit always writes the point.
    auto canon = [](const char* text) {
        Dimensioned d;
        CHECK(parseDimensioned(text, d));
        return d.canonical();
    };
    CHECK_EQ(canon("1.5mm2"), std::string("1.5mm2"));
    CHECK_EQ(canon("2m2"), std::string("2m2"));
    CHECK_EQ(canon("0.5mm2"), std::string("500000um2"));

    for (const char* text : {"2m2", "0.5mm2", "1.5mm2", "0.35mm2", "1000mm2"}) {
        Dimensioned a, b;
        CHECK(parseDimensioned(text, a));
        CHECK(parseDimensioned(a.canonical(), b));
        CHECK(a == b);
    }
}

TEST_CASE("spec 3.4: imperial literals are recognised so E-17 can fire") {
    CHECK_FALSE(imperialSuffix("100mil").empty());
    CHECK_FALSE(imperialSuffix("5in").empty());
    CHECK_FALSE(imperialSuffix("2thou").empty());
    CHECK_FALSE(imperialSuffix("1ft").empty());
    CHECK_FALSE(imperialSuffix("3oz").empty());
    // Package codes are identifiers naming a footprint family, not measurements.
    CHECK(imperialSuffix("0603").empty());
    CHECK(imperialSuffix("0805").empty());
    // A bare word that happens to spell an imperial unit stays an identifier.
    CHECK(imperialSuffix("in").empty());
    CHECK(imperialSuffix("mil").empty());
}

TEST_CASE("spec 2.3: identifier rules, including the trailing hyphen") {
    bool dash = false;
    CHECK(isIdentifierLexeme("PWR-EN", dash));
    CHECK(isIdentifierLexeme("SW-NODE", dash));
    CHECK(isIdentifierLexeme("-5V", dash));
    CHECK(isIdentifierLexeme("I2C-SDA", dash));
    CHECK(isIdentifierLexeme("_x", dash));
    CHECK(isIdentifierLexeme("0603", dash));

    CHECK_FALSE(isIdentifierLexeme("VCC-", dash));
    CHECK(dash);  // this is what raises E-02
}

TEST_CASE("spec 2.3: classification is whole-lexeme, so part names win") {
    // "10kR-0603" is a part name: it fails the dimensioned test as a whole and
    // falls back to identifier with no backtracking.
    WordClass wc = classifyWord("10kR-0603");
    CHECK(wc.flags & WordFlags::Identifier);
    CHECK_FALSE(wc.flags & WordFlags::Dimensioned);

    // "4k7R" is a resistance and not a legal identifier start... but it *is* a
    // legal identifier lexeme too; position decides which reading applies.
    wc = classifyWord("4k7R");
    CHECK(wc.flags & WordFlags::Dimensioned);

    // "-5V" is both the net named -5V and the value minus five volts (spec 2.3).
    wc = classifyWord("-5V");
    CHECK(wc.flags & WordFlags::Identifier);
    CHECK(wc.flags & WordFlags::Dimensioned);
    CHECK(wc.flags & WordFlags::LeadingDash);
}

TEST_CASE("spec 2.6: reserved words are lowercase only") {
    CHECK(classifyWord("block").flags & WordFlags::Reserved);
    CHECK(classifyWord("part").flags & WordFlags::Reserved);
    CHECK(classifyWord("harness").flags & WordFlags::Reserved);
    CHECK(classifyWord("netclass").flags & WordFlags::Reserved);
    CHECK(classifyWord("match").flags & WordFlags::Reserved);
    CHECK(classifyWord("static").flags & WordFlags::Reserved);
    CHECK(classifyWord("extern").flags & WordFlags::Reserved);
    // "Block and PART are legal identifiers."
    CHECK_FALSE(classifyWord("Block").flags & WordFlags::Reserved);
    CHECK_FALSE(classifyWord("PART").flags & WordFlags::Reserved);
}

TEST_CASE("spec 4.3: version constraints are recognised") {
    CHECK(classifyWord("1.2").flags & WordFlags::VersionSpec);
    CHECK(classifyWord("1.2+").flags & WordFlags::VersionSpec);
    CHECK(classifyWord("1.2-").flags & WordFlags::VersionSpec);
    CHECK(classifyWord("0.2-1.2").flags & WordFlags::VersionSpec);
    CHECK(classifyWord("1.0+").flags & WordFlags::VersionSpec);
}

TEST_MAIN()
