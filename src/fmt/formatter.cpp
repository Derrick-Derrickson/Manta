// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fmt/formatter.h"

#include <algorithm>
#include <format>
#include <vector>

namespace manta {

namespace {

constexpr bool isCloser(TokenKind k) noexcept {
    return k == TokenKind::RBrace || k == TokenKind::RParen || k == TokenKind::RBracket;
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            std::string_view line = text.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back(line);
            start = i + 1;
        }
    }
    if (start < text.size()) lines.push_back(text.substr(start));
    return lines;
}

}  // namespace

// The indentation rules are specified in formatter.h. The walk is line by
// line: each line's target indent is computed from the state the tokens of
// the previous lines left behind, the line is emitted with only its leading
// whitespace replaced, and then the tokens on the line advance the state.
//
// State is two scalars. `depth` counts unclosed brackets. `unitActive` says
// whether the innermost context holds an unfinished unit; it needs no stack
// because the bracket that opens a context is itself a token of the outer
// unit, so on close the outer unit is unfinished by construction.
std::string formatSource(const TokenStream& tokens, const SourceFile& file,
                         const FormatOptions& options) {
    std::string_view text = file.text();
    const std::size_t contentEnd = tokens.hasEndMarker() ? tokens.contentEnd : text.size();

    std::string out;
    out.reserve(text.size() + 64);

    const std::vector<Token>& toks = tokens.tokens;
    const std::vector<Comment>& comments = tokens.comments;
    std::size_t ti = 0;
    std::size_t ci = 0;

    std::size_t depth = 0;
    bool unitActive = false;
    // End offset of a block comment still open when a line begins; lines that
    // start inside it are its interior and are reproduced verbatim.
    std::size_t commentSpansTo = 0;

    std::size_t lineStart = 0;
    while (lineStart < text.size()) {
        // Spec 2.8: from the end-of-content marker on, byte for byte --
        // not reindented, line endings not normalised, nothing.
        if (lineStart >= contentEnd) {
            out += text.substr(lineStart);
            return out;
        }

        std::size_t newline = text.find('\n', lineStart);
        std::size_t lineEnd = newline == std::string_view::npos ? text.size() : newline;
        std::size_t contentStop = lineEnd;  // excludes the '\r' of a CRLF ending
        if (contentStop > lineStart && text[contentStop - 1] == '\r') --contentStop;

        std::size_t firstByte = lineStart;
        while (firstByte < contentStop && (text[firstByte] == ' ' || text[firstByte] == '\t')) {
            ++firstByte;
        }

        if (lineStart < commentSpansTo) {
            // Interior of a multi-line block comment: authors align comment
            // art, so the bytes pass through untouched.
            out.append(text, lineStart, contentStop - lineStart);
            out += '\n';
        } else if (firstByte == contentStop) {
            // A blank line stays blank, with no trailing spaces.
            out += '\n';
        } else {
            const Token* firstTok = ti < toks.size() && toks[ti].offset < lineEnd &&
                                            toks[ti].kind != TokenKind::Eof
                                        ? &toks[ti]
                                        : nullptr;
            bool commentFirst = ci < comments.size() && comments[ci].offset < lineEnd &&
                                (firstTok == nullptr || comments[ci].offset < firstTok->offset);

            std::size_t indent = depth;
            if (!commentFirst && firstTok != nullptr) {
                if (isCloser(firstTok->kind)) {
                    indent = depth > 0 ? depth - 1 : 0;
                } else if (firstTok->kind != TokenKind::SectionMarker && unitActive) {
                    indent = depth + 1;
                }
            }
            out.append(indent * options.indentWidth, ' ');
            out.append(text, firstByte, contentStop - firstByte);
            out += '\n';
        }

        // Advance the state over this line's tokens, interior lines included:
        // code may follow a '*/' that closes on this line.
        while (ti < toks.size() && toks[ti].offset < lineEnd && toks[ti].kind != TokenKind::Eof) {
            switch (toks[ti].kind) {
                case TokenKind::Semi:
                case TokenKind::Colon:
                case TokenKind::SectionMarker:
                    unitActive = false;
                    break;
                case TokenKind::LBrace:
                case TokenKind::LParen:
                case TokenKind::LBracket:
                    ++depth;
                    unitActive = false;
                    break;
                case TokenKind::RBrace:
                case TokenKind::RParen:
                case TokenKind::RBracket:
                    if (depth > 0) --depth;
                    unitActive = true;
                    break;
                default:
                    unitActive = true;
                    break;
            }
            ++ti;
        }
        while (ci < comments.size() && comments[ci].offset < lineEnd) {
            std::size_t end = comments[ci].offset + comments[ci].length;
            if (comments[ci].block && end > lineEnd) commentSpansTo = end;
            ++ci;
        }

        lineStart = newline == std::string_view::npos ? text.size() : newline + 1;
    }
    return out;
}

// A unified diff over whole lines. The longest-common-subsequence table is
// bounded so that a pathological input degrades to "replace everything" rather
// than to quadratic memory.
std::string unifiedDiff(std::string_view before, std::string_view after, std::string_view path) {
    auto a = splitLines(before);
    auto b = splitLines(after);

    std::string out;
    if (a == b) return out;

    out += std::format("--- {}\n+++ {}\n", path, path);

    constexpr std::size_t kMaxCells = 4u * 1000u * 1000u;
    if (a.size() * b.size() > kMaxCells) {
        out += std::format("@@ -1,{} +1,{} @@\n", a.size(), b.size());
        for (auto line : a) { out += '-'; out += line; out += '\n'; }
        for (auto line : b) { out += '+'; out += line; out += '\n'; }
        return out;
    }

    // Classic LCS, then walk it back into a hunk list.
    std::vector<std::vector<std::uint32_t>> lcs(a.size() + 1,
                                                std::vector<std::uint32_t>(b.size() + 1, 0));
    for (std::size_t i = a.size(); i-- > 0;) {
        for (std::size_t j = b.size(); j-- > 0;) {
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1
                                     : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }

    struct Edit {
        char kind;  // ' ', '-', '+'
        std::string_view line;
    };
    std::vector<Edit> edits;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            edits.push_back({' ', a[i]});
            ++i;
            ++j;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            edits.push_back({'-', a[i++]});
        } else {
            edits.push_back({'+', b[j++]});
        }
    }
    while (i < a.size()) edits.push_back({'-', a[i++]});
    while (j < b.size()) edits.push_back({'+', b[j++]});

    // Group into hunks with three lines of context.
    constexpr std::size_t kContext = 3;
    std::size_t index = 0;
    while (index < edits.size()) {
        if (edits[index].kind == ' ') {
            ++index;
            continue;
        }
        std::size_t start = index > kContext ? index - kContext : 0;
        std::size_t stop = index;
        while (stop < edits.size()) {
            std::size_t run = 0;
            std::size_t probe = stop;
            while (probe < edits.size() && edits[probe].kind == ' ' && run < kContext * 2) {
                ++probe;
                ++run;
            }
            if (run >= kContext * 2 || probe >= edits.size()) break;
            stop = probe + 1;
        }
        stop = std::min(stop + kContext, edits.size());

        std::size_t aStart = 0, bStart = 0;
        for (std::size_t k = 0; k < start; ++k) {
            if (edits[k].kind != '+') ++aStart;
            if (edits[k].kind != '-') ++bStart;
        }
        std::size_t aCount = 0, bCount = 0;
        for (std::size_t k = start; k < stop; ++k) {
            if (edits[k].kind != '+') ++aCount;
            if (edits[k].kind != '-') ++bCount;
        }

        out += std::format("@@ -{},{} +{},{} @@\n", aStart + 1, aCount, bStart + 1, bCount);
        for (std::size_t k = start; k < stop; ++k) {
            out += edits[k].kind;
            out += edits[k].line;
            out += '\n';
        }
        index = stop;
    }

    return out;
}

}  // namespace manta
