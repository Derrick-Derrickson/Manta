// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Parsing a .mantaRules file.
//
// Reuses the manta lexer unchanged: a rules file is written in the same tokens,
// so `&`, `#`, `:`, `->` and the rest all arrive already recognised. Only the
// grammar differs, so only the parser is new.
//
//   file        = { rules_def } ;
//   rules_def   = "rules" identifier "{" { field_decl | check_def } "}" ";" ;
//
//   field_decl  = "#" identifier ":" quantity ";" ;
//   quantity    = "voltage" | "current" | "resistance" | "capacitance"
//               | "inductance" | "power" | "frequency" | "time" | "length"
//               | "temperature" | "number" | "boolean" | "text" ;
//
//   check_def   = "check" identifier "for" domain "{" { clause } "}" ";" ;
//   domain      = "part" | "net" | "component"
//               | "net" "." identifier "-" ">" "net" "." identifier ;
//   clause      = "when" expr ";"
//               | "require" expr ";"
//               | ( "error" | "warning" ) string { string } ";" ;
#pragma once

#include <vector>

#include "base/arena.h"
#include "base/intern.h"
#include "diag/engine.h"
#include "lex/lexer.h"
#include "rules/rules_ast.h"
#include "source/source_manager.h"

namespace manta {

class RulesParser {
public:
    RulesParser(const TokenStream& tokens, const SourceFile& file, Arena& arena,
                StringInterner& interner, DiagEngine& diags)
        : toks_(tokens), file_(file), arena_(arena), interner_(interner), diags_(diags) {}

    [[nodiscard]] RuleFile run();

private:
    // ---- token access -----------------------------------------------------
    [[nodiscard]] const Token& cur() const { return toks_.at(pos_); }
    [[nodiscard]] const Token& ahead(std::size_t n = 1) const { return toks_.at(pos_ + n); }
    [[nodiscard]] bool at(TokenKind k) const { return cur().kind == k; }
    [[nodiscard]] bool atEnd() const { return cur().kind == TokenKind::Eof; }
    [[nodiscard]] Span here() const { return cur().span(file_.id()); }
    [[nodiscard]] std::string_view text(const Token& t) const {
        return file_.text().substr(t.offset, t.length);
    }
    [[nodiscard]] std::string_view curText() const { return text(cur()); }
    [[nodiscard]] bool atWord(std::string_view word) const {
        return at(TokenKind::Word) && curText() == word;
    }

    const Token& advance() { return toks_.at(pos_++); }
    bool accept(TokenKind k) {
        if (at(k)) { ++pos_; return true; }
        return false;
    }
    bool acceptWord(std::string_view word) {
        if (atWord(word)) { ++pos_; return true; }
        return false;
    }
    bool expect(TokenKind k, std::string_view context);

    void error(Span at, std::string message);
    void recoverToSemi();

    // ---- productions ------------------------------------------------------
    RuleSet* parseRuleSet();
    FieldTypeDecl* parseFieldDecl();
    RuleCheck* parseCheck();
    bool parseDomain(RuleCheck& check);

    RuleExpr* parseExpr();
    RuleExpr* parseOr();
    RuleExpr* parseAnd();
    RuleExpr* parseComparison();
    RuleExpr* parseSum();
    RuleExpr* parseProduct();
    RuleExpr* parseUnary();
    RuleExpr* parsePostfix();
    RuleExpr* parsePrimary();

    // Adjacent string literals concatenate, so a long message wraps without
    // escapes. The "{...}" holes are parsed here rather than at evaluation.
    std::span<MessageChunk> parseMessage();
    RuleExpr* parseMessageHole(std::string_view body, Span messageSpan);
    [[nodiscard]] std::string unescape(std::string_view raw) const;

    template <typename T>
    std::span<T> commit(std::vector<T>& v) {
        auto dst = arena_.makeArray<T>(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) dst[i] = v[i];
        return dst;
    }

    const TokenStream& toks_;
    const SourceFile& file_;
    Arena& arena_;
    StringInterner& interner_;
    DiagEngine& diags_;
    std::size_t pos_ = 0;
    int errorBudget_ = 100;
};

}  // namespace manta
