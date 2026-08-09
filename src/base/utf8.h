// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal UTF-8 support.
//
// Spec 2.1: outside comments and string literals source is ASCII, so the lexer's
// hot path is pure ASCII. UTF-8 matters in exactly three places: skipping over
// comment and string bytes, counting columns for diagnostics, and recognising
// the one non-ASCII token the language does accept, "±" (spec 3.3).
#pragma once

#include <cstdint>
#include <string_view>

namespace manta {

// Byte length of the sequence a lead byte introduces. Never returns 0, so a
// malformed byte still advances and cannot hang a scan loop.
[[nodiscard]] constexpr int utf8SequenceLength(unsigned char lead) noexcept {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

[[nodiscard]] constexpr bool isAscii(unsigned char c) noexcept { return c < 0x80; }

// U+00B1 PLUS-MINUS SIGN, encoded as C2 B1. Spec 3.3 writes tolerances as "±1%"
// and permits "+-1%" as an alternative spelling.
inline constexpr std::string_view kPlusMinus = "\xC2\xB1";

[[nodiscard]] constexpr bool startsWithPlusMinus(std::string_view s) noexcept {
    return s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xC2 &&
           static_cast<unsigned char>(s[1]) == 0xB1;
}

// U+00B5 MICRO SIGN (C2 B5) and U+03BC GREEK SMALL LETTER MU (CE BC). Spec 3.2
// states plainly that "µ is not accepted"; recognising it lets the lexer say so
// rather than emit a baffling generic error.
[[nodiscard]] constexpr bool startsWithMicro(std::string_view s) noexcept {
    if (s.size() < 2) return false;
    auto a = static_cast<unsigned char>(s[0]);
    auto b = static_cast<unsigned char>(s[1]);
    return (a == 0xC2 && b == 0xB5) || (a == 0xCE && b == 0xBC);
}

}  // namespace manta
