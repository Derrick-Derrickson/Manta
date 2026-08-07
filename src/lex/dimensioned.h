// Dimensioned values (spec 3.2) and their canonical rendering (spec 17).
//
// A value is stored as an exact scaled integer -- mantissa x 10^exp10 -- and
// never as a double. Spec 15.8 requires byte-identical output, and binary
// floating point cannot represent 4.7 or 0.002 exactly, so round-tripping
// "4k7R" through a double would be neither exact nor portable.
//
// Both spellings of a fractional value are accepted and mean the same thing:
//     4k7R == 4.7kR      3V3 == 3.3V      2u2H == 2.2uH      2G4Hz == 2.4GHz
// The formatter emits the SI-substituted form.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace manta {

enum class Unit : std::uint8_t {
    None,      // a bare number
    Ohm,       // R
    Farad,     // F
    Henry,     // H
    Volt,      // V
    Ampere,    // A
    Watt,      // W
    Hertz,     // Hz
    Metre,     // m
    Second,    // s
    Celsius,   // C
    Percent,   // %
};

[[nodiscard]] std::string_view unitSuffix(Unit u) noexcept;

// The unit a quantity must have for a given directive to be well typed.
[[nodiscard]] bool isTimeUnit(Unit u) noexcept;
[[nodiscard]] bool isLengthUnit(Unit u) noexcept;

struct Dimensioned {
    std::int64_t mantissa = 0;  // signed; value == mantissa * 10^exp10
    std::int32_t exp10 = 0;
    Unit unit = Unit::None;
    bool differential = false;  // the optional 'D' suffix on &IMP (spec 11.3)

    [[nodiscard]] bool isZero() const noexcept { return mantissa == 0; }

    // Canonical SI-substituted text, e.g. 4700 ohm -> "4k7R", 3.3 V -> "3V3".
    [[nodiscard]] std::string canonical() const;

    friend bool operator==(const Dimensioned&, const Dimensioned&) noexcept;
};

// Comparison across differing exponents, for directive conflict detection.
[[nodiscard]] int compareMagnitude(const Dimensioned& a, const Dimensioned& b) noexcept;

struct WordClass {
    std::uint16_t flags = 0;
    Dimensioned value;  // valid when flags has Dimensioned, Integer or Decimal
};

// Classifies a complete lexeme. This is the single place that decides what a
// word could be; the parser then picks by grammatical position.
[[nodiscard]] WordClass classifyWord(std::string_view lexeme) noexcept;

// Parses a dimensioned value, returning false if the text is not one.
[[nodiscard]] bool parseDimensioned(std::string_view text, Dimensioned& out) noexcept;

// True if the lexeme satisfies the identifier grammar of spec 2.3. Reports a
// trailing hyphen separately because that is error E-02 rather than simply
// "not an identifier".
[[nodiscard]] bool isIdentifierLexeme(std::string_view text, bool& trailingDash) noexcept;

// Reserved words (spec 2.6). Lowercase only: "Block" and "PART" are identifiers.
[[nodiscard]] bool isReservedWord(std::string_view text) noexcept;

// The imperial suffix a lexeme ends in, or empty. Recognised on purpose so that
// spec 3.4 can be reported as E-17 rather than as a mystifying parse failure.
[[nodiscard]] std::string_view imperialSuffix(std::string_view text) noexcept;

}  // namespace manta
