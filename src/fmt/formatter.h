// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// 'manta fmt' (spec 17), which is normative.
//
// The formatter manages indentation and nothing else. Line structure is the
// author's: it never joins or splits lines, never adds or removes a blank line
// or a ';', never reorders or realigns anything within a line, and never
// touches a comment's content or its position on its line. The only bytes it
// rewrites are each line's leading whitespace, plus two file-level
// normalisations carried over from spec 1.4: line endings become LF and the
// file gains a final newline if it lacks one.
//
// A line's indentation, in units of four spaces, is decided by the token
// stream -- so brackets inside strings and comments do not count:
//
//   - depth: the number of '{', '(' and '[' still open at the start of the
//     line.
//   - A line whose first token is '}', ')' or ']' outdents to the matching
//     open's depth, which puts '};' level with its opener.
//   - A continuation line -- one that begins inside an unfinished unit at the
//     current depth -- indents one unit past the depth. A unit is a statement,
//     a binding, or a binding-list head: it becomes unfinished at its first
//     token and finishes at ';', at ':', or at an opening bracket (whose
//     contents indent by depth instead); a closing bracket resumes the outer
//     unit, still unfinished until its own ';'.
//   - A '--- TITLE' section marker takes plain depth, like any statement.
//   - A line whose first content is a comment takes plain depth.
//   - Interior lines of a multi-line block comment -- every line after the one
//     carrying '/*' -- keep their bytes untouched, so comment art survives.
//   - A blank line stays blank: no indentation, no trailing spaces.
//
// The end-of-content marker of spec 2.8 and everything after it are reproduced
// byte for byte, line endings included: it is not manta and is not formatted.
#pragma once

#include <string>

#include "lex/lexer.h"
#include "source/source_manager.h"

namespace manta {

struct FormatOptions {
    std::size_t indentWidth = 4;
};

// Re-indents a lexed file. Purely lexical: the caller is expected to have
// parsed the file first and refused to format one that does not parse, since
// bracket balance is what the depth rule leans on.
[[nodiscard]] std::string formatSource(const TokenStream& tokens, const SourceFile& file,
                                       const FormatOptions& options = {});

// A unified diff, for 'manta fmt --diff'.
[[nodiscard]] std::string unifiedDiff(std::string_view before, std::string_view after,
                                      std::string_view path);

}  // namespace manta
