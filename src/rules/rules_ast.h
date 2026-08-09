// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The AST for a .mantaRules file.
//
// A rules file declares the '#' fields it reads, then the checks over them. It
// is a toolchain extension rather than part of the language: the fields it
// names are already legal manta, so a decorated design compiles and links
// whether or not any rules file is present. What a rules file adds is checking.
//
// Nodes are arena-allocated and trivially destructible, exactly as the manta
// AST is, so child lists are std::span over arena storage.
#pragma once

#include <cstdint>
#include <span>

#include "base/intern.h"
#include "lex/dimensioned.h"
#include "source/span.h"

namespace manta {

struct RuleExpr;

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class RuleExprKind : std::uint8_t {
    Number,      // a dimensioned or bare numeric literal
    String,      // a quoted literal
    Name,        // a bare word: a binding, or an enum value such as `out`
    Member,      // <expr> "." <name>
    Unary,
    Binary,
    Aggregate,   // sum(...), min(...), count(...), any(...), all(...)
    Has,         // has(<expr>)
};

enum class RuleUnOp : std::uint8_t { Negate, Not };

enum class RuleBinOp : std::uint8_t {
    Add, Subtract, Multiply, Divide,
    Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual,
    And, Or,
};

[[nodiscard]] std::string_view ruleBinOpText(RuleBinOp op) noexcept;

enum class Aggregate : std::uint8_t { Sum, Min, Max, Count, Any, All };

[[nodiscard]] std::string_view aggregateName(Aggregate a) noexcept;
[[nodiscard]] bool aggregateFromName(std::string_view name, Aggregate& out) noexcept;

struct RuleExpr {
    RuleExprKind kind = RuleExprKind::Number;
    Span span;

    Dimensioned number;                  // Number
    SymbolId text = SymbolId::kInvalid;  // String, Name, Member (the member name)
    RuleUnOp unOp = RuleUnOp::Negate;
    RuleBinOp binOp = RuleBinOp::Add;
    Aggregate aggregate = Aggregate::Sum;

    RuleExpr* lhs = nullptr;   // Unary operand, Binary left, Member target,
                               // Aggregate/Has argument
    RuleExpr* rhs = nullptr;   // Binary right
    RuleExpr* filter = nullptr;  // an aggregate's "where" clause, if any
};

// ---------------------------------------------------------------------------
// Declarations and checks
// ---------------------------------------------------------------------------

// "#VOH : voltage;" -- a type assertion, not a registration. The field was
// already legal; declaring its quantity is what lets units be checked, so that
// comparing a current against a voltage is an error in the rules file rather
// than a check that silently always passes.
struct FieldTypeDecl {
    SymbolId name = SymbolId::kInvalid;
    Unit unit = Unit::None;
    bool isBoolean = false;
    bool isText = false;
    Span span;
};

// What a check iterates over.
enum class RuleDomain : std::uint8_t {
    Part,       // each part declaration        -- compile and link
    Net,        // each net                     -- link
    Component,  // each instantiated component  -- link
    PinPair,    // ordered pin pairs on one net -- link
};

[[nodiscard]] std::string_view ruleDomainName(RuleDomain d) noexcept;

enum class RuleSeverity : std::uint8_t { Error, Warning };

// A message is literal text with "{...}" holes, pre-parsed at load so that
// evaluation never re-parses. A hole takes a binding, a dotted path from one,
// or an aggregate over such a path -- "{driver}", "{driver.VOH}",
// "{sum(pins.DRAW)}" -- which is everything a diagnostic needs to say.
struct MessageChunk {
    bool isExpr = false;
    SymbolId literal = SymbolId::kInvalid;
    RuleExpr* expr = nullptr;
};

struct RuleCheck {
    SymbolId name = SymbolId::kInvalid;  // also its diagnostic code
    RuleDomain domain = RuleDomain::Net;

    // For PinPair: the two binding names, as in "for net.driver -> net.receiver".
    SymbolId leftBinding = SymbolId::kInvalid;
    SymbolId rightBinding = SymbolId::kInvalid;

    std::span<RuleExpr*> guards;    // every "when" must hold
    RuleExpr* requirement = nullptr;
    RuleSeverity severity = RuleSeverity::Error;
    std::span<MessageChunk> message;  // adjacent literals already joined

    Span span;
    Span nameSpan;
};

struct RuleSet {
    SymbolId name = SymbolId::kInvalid;
    std::span<FieldTypeDecl*> fields;
    std::span<RuleCheck*> checks;
    Span span;
};

struct RuleFile {
    std::span<RuleSet*> sets;
    FileId file = kNoFile;
};

}  // namespace manta
