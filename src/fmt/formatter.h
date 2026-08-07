// 'manta fmt' (spec 17), which is normative.
//
// The formatter:
//   - normalises whitespace and indentation
//   - reflows statements freely across lines
//   - never adds or removes a ';', since statement boundaries are semantic
//   - emits canonical sigil order (spec 9.4)
//   - emits canonical port arrows (spec 10.1)
//   - emits SI-substituted values (spec 3.2)
//   - emits a trailing ';' in binding lists
//   - aligns '=' within a binding block and within a pin map
//   - does not alter case, comments, or the order of declarations
//
// Comments are re-attached by source offset rather than by token index: before
// emitting any construct, every comment lying before its span is flushed. A
// comment that shared a line with the code before it is appended to that line;
// one that stood alone keeps its own line. That reproduces comments exactly
// while leaving the formatter free to reflow everything around them.
#pragma once

#include <string>

#include "ast/ast.h"
#include "base/intern.h"
#include "lex/lexer.h"
#include "source/source_manager.h"

namespace manta {

struct FormatOptions {
    std::size_t lineWidth = 96;
    std::size_t indentWidth = 4;
};

// Renders a parsed unit as canonical source. Line endings are LF (spec 1.4:
// "the formatter normalises them to LF").
[[nodiscard]] std::string formatUnit(const SourceUnit& unit, const TokenStream& tokens,
                                     const SourceFile& file, const StringInterner& interner,
                                     const FormatOptions& options = {});

// A unified diff, for 'manta fmt --diff'.
[[nodiscard]] std::string unifiedDiff(std::string_view before, std::string_view after,
                                      std::string_view path);

}  // namespace manta
