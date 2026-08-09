// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "source/span.h"

namespace manta {

enum class TokenKind : std::uint8_t {
    Eof,

    // A maximal-munch lexeme: an identifier, a number, a dimensioned value, a
    // boolean or a reserved word. Which of those it *is* depends on grammatical
    // position, so the lexer records a classification bitset and the parser
    // chooses. See WordFlags and spec 2.3.
    Word,
    String,

    // Structural punctuation.
    LBrace,    // {
    RBrace,    // }
    LParen,    // (
    RParen,    // )
    LBracket,  // [
    RBracket,  // ]
    Semi,      // ;
    Comma,     // ,
    Colon,     // :
    Dot,       // .

    // Sigils and modifiers.
    Tilde,      // ~   weak strength; instance binding; xor inside $...$
    Bang,       // !   locked strength; DNP prefix; not inside $...$
    Question,   // ?   unassigned designator; &NET=? unbind
    Percent,    // %   per-copy selector; designator range; percentage suffix
    Hash,       // #   user field
    At,         // @   system field
    Amp,        // &   directive
    Dollar,     // $   substitution delimiter
    PlusMinus,  // ±  or  +-

    // Connection operators (spec 6).
    Eq,      // =
    EqEq,    // ==
    EqStar,  // =*
    StarEq,  // *=

    // Arrows (spec 10).
    Gt,    // >
    Lt,    // <
    LtGt,  // <>
    GtGt,  // >>

    // Multiplicity and arithmetic. These carry their manta meaning outside
    // $...$ and their arithmetic meaning inside it (spec 14.2).
    Plus,   // +
    Minus,  // -
    Star,   // *
    Pipe,   // |
    Caret,  // ^   adjacency outside $...$, exponent inside
    Slash,  // /   division; only ever a token inside $...$

    // Expression-mode comparisons (spec 14.3).
    Le,     // <=
    Ge,     // >=
    BangEq, // !=

    // A render section marker (revision 1.3): a line inside a block body whose
    // first non-whitespace is "---", optionally followed by whitespace and a
    // free-text title. The token covers the trimmed title text; `flags` holds
    // the byte distance back to the '-' that starts the marker, so a
    // diagnostic can span the whole line.
    SectionMarker,

    Invalid,
};

[[nodiscard]] std::string_view tokenKindName(TokenKind k) noexcept;

// What a Word lexeme could be. Computed over the *whole* lexeme, which is what
// lets "10kR-0603" fall out as an identifier while "4k7R" is a resistance,
// with no backtracking anywhere (spec 2.3, 3.2).
namespace WordFlags {
inline constexpr std::uint16_t Identifier    = 1u << 0;  // valid per spec 2.3
inline constexpr std::uint16_t Integer       = 1u << 1;  // -?[0-9]+
inline constexpr std::uint16_t Decimal       = 1u << 2;  // -?[0-9]+.[0-9]+, no unit
inline constexpr std::uint16_t Dimensioned   = 1u << 3;  // spec 3.2
inline constexpr std::uint16_t Imperial      = 1u << 4;  // raises E-17 in a value position
inline constexpr std::uint16_t TrailingDash  = 1u << 5;  // raises E-02
inline constexpr std::uint16_t Reserved      = 1u << 6;  // spec 2.6
inline constexpr std::uint16_t BoolTrue      = 1u << 7;  // "true" or "TRUE"
inline constexpr std::uint16_t BoolFalse     = 1u << 8;  // "false" or "FALSE"
inline constexpr std::uint16_t UpperCase     = 1u << 9;  // no lowercase letters; for E-34
inline constexpr std::uint16_t LeadingDash   = 1u << 10; // "-5V": net name or negative value
inline constexpr std::uint16_t MicroSign     = 1u << 11; // "µ" seen; spec 3.2 rejects it
inline constexpr std::uint16_t VersionSpec   = 1u << 12; // "1.2", "1.2+", "0.2-1.2" (spec 4.3)
}  // namespace WordFlags

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::uint16_t flags = 0;  // WordFlags for Word; marker distance for SectionMarker
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] Span span(FileId file) const noexcept { return Span{file, offset, length}; }
    [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
    [[nodiscard]] bool has(std::uint16_t f) const noexcept { return (flags & f) != 0; }
};

// Comments are not tokens (spec 2.5: "a comment may appear wherever whitespace
// may appear"), but the formatter must reproduce them exactly, so they are kept
// aside and attached to the token that follows them.
struct Comment {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t followingToken = 0;  // index into the token stream
    bool block = false;                // /* */ rather than //
    bool ownLine = false;              // nothing but whitespace before it on its line
    bool blankLineBefore = false;      // formatter preserves paragraph breaks
};

}  // namespace manta
