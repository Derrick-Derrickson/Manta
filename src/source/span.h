// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Source locations.
//
// A Span is 12 bytes: file id, byte offset, byte length. Line and column are
// *not* stored — they are derived on demand from the file's LineMap, because
// diagnostics are rare and spans are everywhere.
//
// Spec 14.6 is the reason spans are byte-offset based rather than token based:
// "Because a substitution occupies one delimited span on one line, source
// positions map through it by length alone". Diagnostics arising after
// substitution must report the position of the substitution in the original
// source (spec 15.6), and byte arithmetic is what makes that mapping exact.
#pragma once

#include <cstdint>
#include <limits>

namespace manta {

using FileId = std::uint32_t;
inline constexpr FileId kNoFile = std::numeric_limits<FileId>::max();

struct Span {
    FileId file = kNoFile;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return file != kNoFile; }
    [[nodiscard]] constexpr std::uint32_t end() const noexcept { return offset + length; }

    // Smallest span covering both. Assumes the same file.
    [[nodiscard]] constexpr Span merge(Span o) const noexcept {
        if (!valid()) return o;
        if (!o.valid()) return *this;
        std::uint32_t lo = offset < o.offset ? offset : o.offset;
        std::uint32_t hi = end() > o.end() ? end() : o.end();
        return Span{file, lo, hi - lo};
    }

    // Sub-span at a byte delta, used when reporting inside a lexeme.
    [[nodiscard]] constexpr Span sub(std::uint32_t delta, std::uint32_t len) const noexcept {
        return Span{file, offset + delta, len};
    }

    [[nodiscard]] constexpr Span first() const noexcept { return Span{file, offset, 1}; }

    friend constexpr bool operator==(const Span&, const Span&) = default;
};

struct LineCol {
    std::uint32_t line = 1;    // 1-based, per spec 15.6
    std::uint32_t column = 1;  // 1-based, counted in UTF-8 codepoints
};

}  // namespace manta
