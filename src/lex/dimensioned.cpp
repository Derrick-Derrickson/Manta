// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lex/dimensioned.h"

#include <array>
#include <cstdlib>

#include "base/utf8.h"
#include "lex/token.h"

namespace manta {

namespace {

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isLetter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Spec 3.2: "SI prefixes are p n u m k M G T. The prefix u denotes micro."
// Returns the power of ten, or a sentinel when the character is not a prefix.
constexpr std::int32_t kNotPrefix = 1000;

constexpr std::int32_t siPrefixExponent(char c) noexcept {
    switch (c) {
        case 'p': return -12;
        case 'n': return -9;
        case 'u': return -6;
        case 'm': return -3;
        case 'k': return 3;
        case 'M': return 6;
        case 'G': return 9;
        case 'T': return 12;
        default: return kNotPrefix;
    }
}

struct UnitEntry {
    std::string_view suffix;
    Unit unit;
};

// Ordered longest-first so that "Hz" wins over "H": "100nHz" is a frequency,
// "100nH" an inductance.
constexpr std::array<UnitEntry, 12> kUnits{{
    {"Hz", Unit::Hertz},
    {"m2", Unit::SquareMetre},

    {"R", Unit::Ohm},
    {"F", Unit::Farad},
    {"H", Unit::Henry},
    {"V", Unit::Volt},
    {"A", Unit::Ampere},
    {"W", Unit::Watt},
    {"m", Unit::Metre},
    {"s", Unit::Second},
    {"C", Unit::Celsius},
    {"%", Unit::Percent},
}};

// Spec 3.4: no inch, mil, thou, foot, mile, ounce, pound or Fahrenheit literal.
// Fahrenheit has no spelling here because 'F' is already farad.
constexpr std::array<std::string_view, 13> kImperial{
    {"thou", "inch", "inches", "mil", "mils", "in", "ft", "feet", "foot", "mile", "miles", "oz",
     "lb"}};

bool matchUnit(std::string_view s, Unit& unit, std::size_t& consumed) noexcept {
    for (const auto& e : kUnits) {
        if (s.size() >= e.suffix.size() && s.substr(0, e.suffix.size()) == e.suffix) {
            unit = e.unit;
            consumed = e.suffix.size();
            return true;
        }
    }
    return false;
}

// Accumulates digits into a mantissa, tracking the decimal exponent. Saturates
// rather than overflowing: a value that large is not a real component parameter,
// and saturation keeps the function total.
bool accumulate(std::string_view digits, std::int64_t& mantissa, std::int32_t& exp10,
                bool fractional) noexcept {
    if (digits.empty()) return false;
    for (char c : digits) {
        if (mantissa > (INT64_MAX - 9) / 10) return false;
        mantissa = mantissa * 10 + (c - '0');
        if (fractional) --exp10;
    }
    return true;
}

}  // namespace

std::string_view unitSuffix(Unit u) noexcept {
    switch (u) {
        case Unit::None: return "";
        case Unit::Ohm: return "R";
        case Unit::Farad: return "F";
        case Unit::Henry: return "H";
        case Unit::Volt: return "V";
        case Unit::Ampere: return "A";
        case Unit::Watt: return "W";
        case Unit::Hertz: return "Hz";
        case Unit::Metre: return "m";
        case Unit::SquareMetre: return "m2";
        case Unit::Second: return "s";
        case Unit::Celsius: return "C";
        case Unit::Percent: return "%";
    }
    return "";
}

bool isTimeUnit(Unit u) noexcept { return u == Unit::Second; }
bool isLengthUnit(Unit u) noexcept { return u == Unit::Metre; }
bool isSquaredUnit(Unit u) noexcept { return u == Unit::SquareMetre; }

bool operator==(const Dimensioned& a, const Dimensioned& b) noexcept {
    return a.unit == b.unit && a.differential == b.differential &&
           compareMagnitude(a, b) == 0;
}

int compareMagnitude(const Dimensioned& a, const Dimensioned& b) noexcept {
    // Normalise to the smaller exponent, saturating on overflow. Values in this
    // language span picoseconds to megaohms, well inside int64 once aligned.
    std::int64_t ma = a.mantissa;
    std::int64_t mb = b.mantissa;
    std::int32_t ea = a.exp10;
    std::int32_t eb = b.exp10;
    while (ea > eb && ma <= INT64_MAX / 10) { ma *= 10; --ea; }
    while (eb > ea && mb <= INT64_MAX / 10) { mb *= 10; --eb; }
    if (ea != eb) return ea < eb ? -1 : 1;  // saturated; compare by scale
    if (ma == mb) return 0;
    return ma < mb ? -1 : 1;
}

bool unitsCompatible(const Dimensioned& a, const Dimensioned& b) noexcept {
    return a.unit == b.unit && a.differential == b.differential;
}

Dimensioned zeroOf(Unit unit) noexcept {
    Dimensioned d;
    d.unit = unit;
    return d;
}

namespace {

// Brings two values to a common exponent so their mantissas can be combined.
// Scales the coarser one down rather than the finer one up, which is what keeps
// the result exact.
bool align(const Dimensioned& a, const Dimensioned& b, std::int64_t& ma, std::int64_t& mb,
           std::int32_t& exp) noexcept {
    ma = a.mantissa;
    mb = b.mantissa;
    exp = a.exp10;
    std::int32_t eb = b.exp10;

    while (exp > eb) {
        if (ma > INT64_MAX / 10 || ma < INT64_MIN / 10) return false;
        ma *= 10;
        --exp;
    }
    while (eb > exp) {
        if (mb > INT64_MAX / 10 || mb < INT64_MIN / 10) return false;
        mb *= 10;
        --eb;
    }
    return true;
}

}  // namespace

Dimensioned addValues(const Dimensioned& a, const Dimensioned& b, bool& ok) noexcept {
    ok = unitsCompatible(a, b);
    if (!ok) return {};

    std::int64_t ma = 0, mb = 0;
    std::int32_t exp = 0;
    if (!align(a, b, ma, mb, exp)) {
        ok = false;
        return {};
    }
    // Overflow here would need quantities no component parameter reaches, but
    // silently wrapping would be worse than saying so.
    if ((mb > 0 && ma > INT64_MAX - mb) || (mb < 0 && ma < INT64_MIN - mb)) {
        ok = false;
        return {};
    }

    Dimensioned out;
    out.mantissa = ma + mb;
    out.exp10 = exp;
    out.unit = a.unit;
    out.differential = a.differential;
    return out;
}

Dimensioned subtractValues(const Dimensioned& a, const Dimensioned& b, bool& ok) noexcept {
    Dimensioned negated = b;
    if (negated.mantissa == INT64_MIN) {
        ok = false;
        return {};
    }
    negated.mantissa = -negated.mantissa;
    return addValues(a, negated, ok);
}

Dimensioned scaleValue(const Dimensioned& a, std::int64_t factor) noexcept {
    Dimensioned out = a;
    if (factor != 0 && (a.mantissa > INT64_MAX / (factor < 0 ? -factor : factor) ||
                        a.mantissa < INT64_MIN / (factor < 0 ? -factor : factor))) {
        return a;  // saturate rather than wrap
    }
    out.mantissa = a.mantissa * factor;
    return out;
}

std::string_view unitName(Unit u) noexcept {
    switch (u) {
        case Unit::None: return "number";
        case Unit::Ohm: return "resistance";
        case Unit::Farad: return "capacitance";
        case Unit::Henry: return "inductance";
        case Unit::Volt: return "voltage";
        case Unit::Ampere: return "current";
        case Unit::Watt: return "power";
        case Unit::Hertz: return "frequency";
        case Unit::Metre: return "length";
        case Unit::SquareMetre: return "area";
        case Unit::Second: return "time";
        case Unit::Celsius: return "temperature";
        case Unit::Percent: return "percentage";
    }
    return "number";
}

bool unitFromName(std::string_view name, Unit& out) noexcept {
    struct Entry {
        std::string_view name;
        Unit unit;
    };
    static constexpr Entry kNames[] = {
        {"number", Unit::None},         {"resistance", Unit::Ohm},
        {"capacitance", Unit::Farad},   {"inductance", Unit::Henry},
        {"voltage", Unit::Volt},        {"current", Unit::Ampere},
        {"power", Unit::Watt},          {"frequency", Unit::Hertz},
        {"length", Unit::Metre},        {"area", Unit::SquareMetre},
        {"time", Unit::Second},
        {"temperature", Unit::Celsius}, {"percentage", Unit::Percent},
    };
    for (const Entry& e : kNames) {
        if (e.name == name) {
            out = e.unit;
            return true;
        }
    }
    return false;
}

std::string Dimensioned::canonical() const {
    // Split into sign, digits and decimal exponent.
    bool negative = mantissa < 0;
    std::uint64_t digitsValue =
        negative ? static_cast<std::uint64_t>(-(mantissa + 1)) + 1 : static_cast<std::uint64_t>(mantissa);

    std::string digits = std::to_string(digitsValue);
    std::int32_t pointFromRight = -exp10;  // digits after the decimal point

    // Trim trailing zeros that only exist because of the exponent, so that
    // 4700 x 10^-3 renders as "4.7" and not "4.700".
    while (pointFromRight > 0 && digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
        --pointFromRight;
    }
    while (pointFromRight < 0) {  // scale up integral values: 47 x 10^1 -> 470
        digits.push_back('0');
        ++pointFromRight;
    }
    while (static_cast<std::size_t>(pointFromRight) >= digits.size()) digits.insert(digits.begin(), '0');

    auto intPart = digits.substr(0, digits.size() - static_cast<std::size_t>(pointFromRight));
    auto fracPart = digits.substr(digits.size() - static_cast<std::size_t>(pointFromRight));

    // Choose the SI prefix that puts the integer part in [1, 1000), preferring
    // no prefix when the value already sits there.
    static constexpr std::pair<std::int32_t, char> kPrefixes[] = {
        {12, 'T'}, {9, 'G'}, {6, 'M'}, {3, 'k'}, {0, '\0'},
        {-3, 'm'}, {-6, 'u'}, {-9, 'n'}, {-12, 'p'}};

    // Magnitude of the value as a power of ten, measured on the integer part.
    std::int32_t magnitude = static_cast<std::int32_t>(intPart.size()) - 1;
    if (intPart == "0") {
        // Leading zeros: find the first significant digit in the fraction.
        std::size_t k = 0;
        while (k < fracPart.size() && fracPart[k] == '0') ++k;
        magnitude = fracPart.empty() ? 0 : -static_cast<std::int32_t>(k) - 1;
        if (k == fracPart.size()) magnitude = 0;  // the value is zero
    }

    char prefix = '\0';
    std::int32_t shift = 0;
    if (unit != Unit::None && unit != Unit::Percent && unit != Unit::Celsius) {
        // A prefix on a squared unit squares with it, so the ladder steps by
        // 10^6 and the character written is the root: 10^-6 m2 is 'mm2', a
        // square millimetre, not a milli-square-metre.
        std::int32_t step = isSquaredUnit(unit) ? 2 : 1;
        for (auto [e, ch] : kPrefixes) {
            if (magnitude >= e * step) {
                prefix = ch;
                shift = e * step;
                break;
            }
        }
    }

    // Re-render the digit string with the decimal point moved by `shift`.
    std::string all = intPart + fracPart;
    auto pointPos = static_cast<std::int32_t>(intPart.size()) - shift;
    while (pointPos <= 0) { all.insert(all.begin(), '0'); ++pointPos; }
    while (pointPos > static_cast<std::int32_t>(all.size())) all.push_back('0');

    std::string lhs = all.substr(0, static_cast<std::size_t>(pointPos));
    std::string rhs = all.substr(static_cast<std::size_t>(pointPos));
    while (!rhs.empty() && rhs.back() == '0') rhs.pop_back();
    while (lhs.size() > 1 && lhs.front() == '0') lhs.erase(lhs.begin());

    std::string out;
    if (negative) out += '-';
    out += lhs;

    std::string_view suffix = unitSuffix(unit);
    // The point-substituted spellings exist because "4k7R" is compact and reads
    // at a glance. A unit that is itself two characters ending in a digit gets
    // no such benefit -- 1.5mm2 would render as "1m5m2", which reads as
    // nonsense -- so a squared unit always writes an explicit point.
    bool substitutePoint = !isSquaredUnit(unit);
    if (rhs.empty()) {
        // No fractional part: prefix and unit simply follow. "10kR", "100nF".
        if (prefix) out += prefix;
        out += suffix;
    } else if (!substitutePoint) {
        out += '.';
        out += rhs;
        if (prefix) out += prefix;
        out += suffix;
    } else if (prefix) {
        // The prefix replaces the decimal point: "4k7R", "2u2H", "2G4Hz".
        out += prefix;
        out += rhs;
        out += suffix;
    } else if (!suffix.empty()) {
        // With no prefix the unit itself replaces the point: "3V3", "1R5".
        out += suffix;
        out += rhs;
    } else {
        out += '.';
        out += rhs;
    }

    if (differential) out += 'D';
    return out;
}

bool isReservedWord(std::string_view t) noexcept {
    return t == "block" || t == "part" || t == "harness" || t == "netclass" || t == "match" ||
           t == "cable" || t == "static" || t == "extern";
}

std::string_view imperialSuffix(std::string_view text) noexcept {
    // Only a numeric-led lexeme can be an imperial *literal*; "in" alone is a
    // perfectly good identifier and must stay one.
    std::size_t i = 0;
    if (i < text.size() && text[i] == '-') ++i;
    std::size_t digitStart = i;
    while (i < text.size() && (isDigit(text[i]) || text[i] == '.')) ++i;
    if (i == digitStart) return {};

    std::string_view tail = text.substr(i);
    for (auto s : kImperial) {
        if (tail == s) return tail;
    }
    return {};
}

bool isIdentifierLexeme(std::string_view t, bool& trailingDash) noexcept {
    trailingDash = false;
    if (t.empty()) return false;

    std::size_t i = 0;
    if (t[0] == '-') {
        // Spec 2.3: a '-' may appear as the first character. It must be followed
        // by something, or the lexeme is just a hyphen.
        ++i;
        if (i >= t.size()) return false;
    }

    // First character after any leading '-' must be a letter, '_' or digit.
    if (!(isLetter(t[i]) || t[i] == '_' || isDigit(t[i]))) return false;

    for (std::size_t k = i; k < t.size(); ++k) {
        char c = t[k];
        if (!(isLetter(c) || isDigit(c) || c == '_' || c == '-')) return false;
    }

    // Spec 2.3: '-' "shall not be the last character." That is error E-02.
    if (t.back() == '-') {
        trailingDash = true;
        return false;
    }
    return true;
}

bool parseDimensioned(std::string_view text, Dimensioned& out) noexcept {
    out = Dimensioned{};
    if (text.empty()) return false;

    std::size_t i = 0;
    bool negative = false;
    if (text[i] == '-') {
        negative = true;
        ++i;
    }

    // Integer part.
    std::size_t start = i;
    while (i < text.size() && isDigit(text[i])) ++i;
    if (i == start) return false;
    std::int64_t mantissa = 0;
    std::int32_t exp10 = 0;
    if (!accumulate(text.substr(start, i - start), mantissa, exp10, false)) return false;

    bool sawExplicitPoint = false;
    if (i < text.size() && text[i] == '.') {
        ++i;
        start = i;
        while (i < text.size() && isDigit(text[i])) ++i;
        if (i == start) return false;
        if (!accumulate(text.substr(start, i - start), mantissa, exp10, true)) return false;
        sawExplicitPoint = true;
    }

    std::string_view rest = text.substr(i);

    if (rest.empty()) {
        // A bare number: integer or decimal, no unit.
        out.mantissa = negative ? -mantissa : mantissa;
        out.exp10 = exp10;
        out.unit = Unit::None;
        return true;
    }

    // An SI prefix is only consumed when something follows it, so that "1m" is
    // one metre while "5mm" is five millimetres -- and when what follows is not
    // itself the whole unit, so that "2m2" is two square metres rather than a
    // milli-prefix with nothing left to qualify.
    Unit wholeUnit = Unit::None;
    std::size_t wholeConsumed = 0;
    bool restIsWholeUnit = matchUnit(rest, wholeUnit, wholeConsumed) &&
                           wholeConsumed == rest.size();

    std::int32_t siExp = 0;
    if (rest.size() > 1 && !restIsWholeUnit) {
        std::int32_t e = siPrefixExponent(rest[0]);
        if (e != kNotPrefix) {
            siExp = e;
            rest.remove_prefix(1);
        }
    }

    Unit unit = Unit::None;
    std::size_t consumed = 0;

    if (siExp != 0) {
        // After a prefix: optional fractional digits (the substituted form),
        // then the unit. "4k7R" -> prefix k, frac 7, unit R.
        start = 0;
        std::size_t k = 0;
        while (k < rest.size() && isDigit(rest[k])) ++k;
        if (k > 0) {
            if (sawExplicitPoint) return false;  // "4.7k7R" is not a value
            if (!accumulate(rest.substr(0, k), mantissa, exp10, true)) return false;
            rest.remove_prefix(k);
        }
        if (!matchUnit(rest, unit, consumed)) return false;
        rest.remove_prefix(consumed);
    } else {
        if (!matchUnit(rest, unit, consumed)) return false;
        rest.remove_prefix(consumed);
        // With no prefix the unit may itself stand in for the decimal point:
        // "3V3" is 3.3 volts.
        std::size_t k = 0;
        while (k < rest.size() && isDigit(rest[k])) ++k;
        if (k > 0) {
            if (sawExplicitPoint) return false;
            if (!accumulate(rest.substr(0, k), mantissa, exp10, true)) return false;
            rest.remove_prefix(k);
        }
    }

    // Spec 11.3: &IMP takes an optional 'D' suffix marking a differential value.
    if (!rest.empty() && rest[0] == 'D') {
        out.differential = true;
        rest.remove_prefix(1);
    }

    if (!rest.empty()) return false;

    // A prefix on a squared unit squares with it: 'mm2' is a square millimetre,
    // (10^-3 m)^2 = 10^-6 m^2, not a milli-square-metre. Getting this wrong
    // would be off by a thousand and entirely silent.
    if (isSquaredUnit(unit)) siExp *= 2;

    out.mantissa = negative ? -mantissa : mantissa;
    out.exp10 = exp10 + siExp;
    out.unit = unit;
    return true;
}

namespace {

// Spec 4.3 version constraints: "1.2+", "1.2-", "0.2-1.2". These share their
// lexical shape with identifiers and decimals, so they are recognised as a
// separate flag rather than a separate token.
bool looksLikeVersionSpec(std::string_view t) noexcept {
    auto revision = [](std::string_view s) {
        std::size_t dot = s.find('.');
        if (dot == std::string_view::npos || dot == 0 || dot + 1 == s.size()) return false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (i == dot) continue;
            if (!isDigit(s[i])) return false;
        }
        return true;
    };

    if (t.empty()) return false;
    if (t.back() == '+' || t.back() == '-') return revision(t.substr(0, t.size() - 1));
    if (revision(t)) return true;

    // A range: find the '-' that separates two revisions.
    for (std::size_t i = 1; i + 1 < t.size(); ++i) {
        if (t[i] == '-' && revision(t.substr(0, i)) && revision(t.substr(i + 1))) return true;
    }
    return false;
}

}  // namespace

WordClass classifyWord(std::string_view lexeme) noexcept {
    WordClass wc;
    if (lexeme.empty()) return wc;

    bool trailingDash = false;
    if (isIdentifierLexeme(lexeme, trailingDash)) wc.flags |= WordFlags::Identifier;
    if (trailingDash) wc.flags |= WordFlags::TrailingDash;

    if (lexeme.front() == '-') wc.flags |= WordFlags::LeadingDash;

    Dimensioned d;
    if (parseDimensioned(lexeme, d)) {
        wc.value = d;
        if (d.unit == Unit::None) {
            wc.flags |= (d.exp10 == 0) ? WordFlags::Integer : WordFlags::Decimal;
        } else {
            wc.flags |= WordFlags::Dimensioned;
        }
    }

    if (!imperialSuffix(lexeme).empty()) wc.flags |= WordFlags::Imperial;
    if (isReservedWord(lexeme)) wc.flags |= WordFlags::Reserved;

    // Spec 3.6: "true and false, lowercase, in user fields. TRUE and FALSE in
    // system fields." Both spellings are recognised here; which one is legal
    // where is a semantic question, checked against E-34.
    if (lexeme == "true" || lexeme == "TRUE") wc.flags |= WordFlags::BoolTrue;
    if (lexeme == "false" || lexeme == "FALSE") wc.flags |= WordFlags::BoolFalse;

    bool anyLower = false;
    for (char c : lexeme) {
        if (c >= 'a' && c <= 'z') { anyLower = true; break; }
    }
    if (!anyLower) wc.flags |= WordFlags::UpperCase;

    if (looksLikeVersionSpec(lexeme)) wc.flags |= WordFlags::VersionSpec;

    return wc;
}

}  // namespace manta
