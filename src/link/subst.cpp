// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link/subst.h"

#include <format>

#include "lex/dimensioned.h"
#include "lex/token.h"

namespace manta {

namespace {

// Spec 14.7: an integer renders as decimal with no leading zeros.
std::string renderInteger(std::int64_t n) { return std::to_string(n); }

// Describes a value for the E-41 message.
std::string_view describeValue(const Value* v) {
    switch (v->kind) {
        case ValueKind::Dimensioned:
        case ValueKind::Percentage:
        case ValueKind::Tolerance: return "a dimensioned value";
        case ValueKind::String: return "a string";
        case ValueKind::List:
        case ValueKind::Repeat: return "a list";
        case ValueKind::Decimal: return "a decimal";
        case ValueKind::Identifier: return "an identifier";
        case ValueKind::Version: return "a version constraint";
        case ValueKind::Unbind: return "'?'";
        default: return "this value";
    }
}

std::int64_t ipow(std::int64_t base, std::int64_t exp, bool& overflow) {
    std::int64_t result = 1;
    overflow = false;
    for (std::int64_t i = 0; i < exp; ++i) {
        if (base != 0 && (result > INT64_MAX / (base < 0 ? -base : base))) {
            overflow = true;
            return 0;
        }
        result *= base;
    }
    return result;
}

}  // namespace

EvalResult Substituter::lookupOperand(SymbolId name, Span at, const FieldEnv& env) {
    // A field reference resolves in either namespace: spec 14.5 says an operand
    // is "a '#' or '@' field visible in the current scope", written without its
    // sigil. User fields are searched first, being the common case.
    const FieldSlot* slot = env.lookup(FieldKey{name, FieldNamespace::User});
    if (!slot) slot = env.lookup(FieldKey{name, FieldNamespace::System});

    if (!slot || !slot->value) {
        // Spec 14.5: "An undefined field is error E-29, never an empty string."
        diags_.report(DiagId::E29, at, interner_.text(name));
        return {};
    }

    const Value* v = slot->value;
    EvalResult r;
    switch (v->kind) {
        case ValueKind::Integer:
            r.kind = EvalResult::Kind::Integer;
            r.integer = v->num.mantissa;
            return r;
        case ValueKind::Boolean:
            r.kind = EvalResult::Kind::Boolean;
            r.boolean = v->boolean;
            return r;
        default:
            // Spec 14.3: "Only integers and booleans are computed with. A
            // dimensioned value, string or list may be substituted but not used
            // as an operand; doing so is error E-41."
            diags_.report(DiagId::E41, at, describeValue(v));
            return {};
    }
}

EvalResult Substituter::eval(const Expr* e, const FieldEnv& env) {
    if (!e) return {};

    switch (e->kind) {
        case ExprKind::IntLit: {
            EvalResult r;
            r.kind = EvalResult::Kind::Integer;
            r.integer = e->intVal;
            return r;
        }
        case ExprKind::BoolLit: {
            EvalResult r;
            r.kind = EvalResult::Kind::Boolean;
            r.boolean = e->boolVal;
            return r;
        }
        case ExprKind::FieldRef:
            return lookupOperand(e->field, e->span, env);

        case ExprKind::Unary: {
            EvalResult v = eval(e->lhs, env);
            if (!v.ok()) return v;
            EvalResult r;
            if (e->unOp == UnOp::Neg) {
                if (v.kind != EvalResult::Kind::Integer) {
                    diags_.report(DiagId::E41, e->span, "a boolean");
                    return {};
                }
                r.kind = EvalResult::Kind::Integer;
                r.integer = -v.integer;
            } else {
                if (v.kind != EvalResult::Kind::Boolean) {
                    // '!' is logical not, so an integer here is the same
                    // category of mistake E-40 describes.
                    diags_.report(DiagId::E40, e->span, "!");
                    return {};
                }
                r.kind = EvalResult::Kind::Boolean;
                r.boolean = !v.boolean;
            }
            return r;
        }

        case ExprKind::Binary: {
            EvalResult a = eval(e->lhs, env);
            EvalResult b = eval(e->rhs, env);
            if (!a.ok() || !b.ok()) return {};

            EvalResult r;
            std::string_view op = binOpText(e->binOp);

            switch (e->binOp) {
                // ---- arithmetic: integers only ---------------------------
                case BinOp::Add:
                case BinOp::Sub:
                case BinOp::Mul:
                case BinOp::Div:
                case BinOp::Pow: {
                    if (a.kind != EvalResult::Kind::Integer ||
                        b.kind != EvalResult::Kind::Integer) {
                        diags_.report(DiagId::E41, e->span, "a boolean");
                        return {};
                    }
                    r.kind = EvalResult::Kind::Integer;
                    switch (e->binOp) {
                        case BinOp::Add: r.integer = a.integer + b.integer; break;
                        case BinOp::Sub: r.integer = a.integer - b.integer; break;
                        case BinOp::Mul: r.integer = a.integer * b.integer; break;
                        case BinOp::Div:
                            if (b.integer == 0) {
                                diags_.report(DiagId::E39, e->span);
                                return {};
                            }
                            // Spec 14.3: "integer division truncating toward
                            // zero", which is what C++ division already does.
                            r.integer = a.integer / b.integer;
                            break;
                        case BinOp::Pow: {
                            if (b.integer < 0) {
                                // Spec 14.3: "A negative exponent is error E-42."
                                diags_.report(DiagId::E42, e->span);
                                return {};
                            }
                            bool overflow = false;
                            r.integer = ipow(a.integer, b.integer, overflow);
                            if (overflow) {
                                diags_.report(DiagId::Type, e->span,
                                              std::string("exponent overflows a 64-bit integer"));
                                return {};
                            }
                            break;
                        }
                        default: break;
                    }
                    return r;
                }

                // ---- comparison: integers in, boolean out ----------------
                case BinOp::Lt:
                case BinOp::Gt:
                case BinOp::Le:
                case BinOp::Ge: {
                    if (a.kind != EvalResult::Kind::Integer ||
                        b.kind != EvalResult::Kind::Integer) {
                        diags_.report(DiagId::E41, e->span, "a boolean");
                        return {};
                    }
                    r.kind = EvalResult::Kind::Boolean;
                    switch (e->binOp) {
                        case BinOp::Lt: r.boolean = a.integer < b.integer; break;
                        case BinOp::Gt: r.boolean = a.integer > b.integer; break;
                        case BinOp::Le: r.boolean = a.integer <= b.integer; break;
                        default: r.boolean = a.integer >= b.integer; break;
                    }
                    return r;
                }

                // ---- equality: integer or boolean, matched ---------------
                case BinOp::Eq:
                case BinOp::Ne: {
                    if (a.kind != b.kind) {
                        diags_.report(DiagId::Type, e->span,
                                      std::format("'{}' compares an integer with a boolean", op));
                        return {};
                    }
                    bool same = a.kind == EvalResult::Kind::Integer ? a.integer == b.integer
                                                                    : a.boolean == b.boolean;
                    r.kind = EvalResult::Kind::Boolean;
                    r.boolean = e->binOp == BinOp::Eq ? same : !same;
                    return r;
                }

                // ---- logical: booleans only ------------------------------
                case BinOp::And:
                case BinOp::Xor:
                case BinOp::Or: {
                    if (a.kind != EvalResult::Kind::Boolean ||
                        b.kind != EvalResult::Kind::Boolean) {
                        // Spec 14.3: "'&', '~' and '|' take booleans only, and
                        // an integer operand there is error E-40."
                        diags_.report(DiagId::E40, e->span, op);
                        return {};
                    }
                    r.kind = EvalResult::Kind::Boolean;
                    switch (e->binOp) {
                        case BinOp::And: r.boolean = a.boolean && b.boolean; break;
                        case BinOp::Xor: r.boolean = a.boolean != b.boolean; break;
                        default: r.boolean = a.boolean || b.boolean; break;
                    }
                    return r;
                }
            }
            return {};
        }
    }
    return {};
}

std::string Substituter::render(const InterpText* text, const FieldEnv& env,
                                bool systemFieldContext) {
    std::string out;
    if (!text) return out;

    for (const InterpChunk& chunk : text->chunks) {
        if (!chunk.isExpr) {
            out += interner_.text(chunk.literal);
            continue;
        }

        // A bare field reference substitutes the field's *value*, whatever its
        // type; only operands of an operator are restricted to integers and
        // booleans (spec 14.3, 14.7).
        if (chunk.expr && chunk.expr->kind == ExprKind::FieldRef) {
            const FieldSlot* slot = env.lookup(FieldKey{chunk.expr->field, FieldNamespace::User});
            if (!slot) slot = env.lookup(FieldKey{chunk.expr->field, FieldNamespace::System});
            if (!slot || !slot->value) {
                diags_.report(DiagId::E29, chunk.span, interner_.text(chunk.expr->field));
                continue;
            }
            const Value* v = slot->value;
            if (v->kind == ValueKind::Boolean) {
                // Spec 14.7: TRUE/FALSE in a system field, true/false in a user.
                out += systemFieldContext ? (v->boolean ? "TRUE" : "FALSE")
                                          : (v->boolean ? "true" : "false");
            } else {
                out += renderValue(v, interner_);
            }
            continue;
        }

        EvalResult r = eval(chunk.expr, env);
        if (!r.ok()) continue;  // already diagnosed
        if (r.kind == EvalResult::Kind::Integer) {
            out += renderInteger(r.integer);
        } else {
            out += systemFieldContext ? (r.boolean ? "TRUE" : "FALSE")
                                      : (r.boolean ? "true" : "false");
        }
    }
    return out;
}

SymbolId Substituter::resolveName(const Name& n, const FieldEnv& env) {
    if (!n.isInterpolated()) return n.symbol;

    std::string rendered = render(n.interp, env);
    if (rendered.empty()) return SymbolId::kInvalid;

    // Spec 14.6: "It shall not produce a statement, a declaration or an
    // operator. That is error E-15." The check is here, after rendering,
    // because only then is there text to inspect -- and the diagnostic is
    // reported at the substitution's own span, which is exactly the position
    // mapping spec 15.6 asks for.
    bool trailingDash = false;
    if (!isIdentifierLexeme(rendered, trailingDash)) {
        std::string_view what = "an identifier";
        for (char c : rendered) {
            if (c == ';') { what = "a statement"; break; }
            if (c == '{' || c == '}') { what = "a declaration"; break; }
            if (c == '=' || c == '~' || c == '&') { what = "an operator"; break; }
        }
        if (trailingDash) {
            diags_.report(DiagId::E02, n.span,
                          std::format("substitution produced '{}', which ends in '-'", rendered));
        } else {
            diags_.report(DiagId::E15, n.span, what);
        }
        return SymbolId::kInvalid;
    }

    return interner_.intern(rendered);
}

std::int64_t Substituter::resolveIndex(const Index& i, const FieldEnv& env, bool& ok) {
    ok = true;
    if (!i.isExpr) return i.literal;

    EvalResult r = eval(i.expr, env);
    if (!r.ok()) {
        ok = false;
        return 0;
    }
    if (r.kind != EvalResult::Kind::Integer) {
        diags_.report(DiagId::Type, i.span, std::string("an array index shall be an integer"));
        ok = false;
        return 0;
    }
    return r.integer;
}

const Value* Substituter::resolveValue(const Value* v, const FieldEnv& env, Arena& arena) {
    if (!v || v->kind != ValueKind::Interp) return v;

    std::string rendered = render(v->interp, env);

    auto* out = arena.make<Value>();
    out->span = v->span;

    // Re-classify the rendered text the same way the lexer would, so that
    // "$100 * 2$R" becomes the resistance 200R rather than the identifier
    // "200R" (spec 14.4: "A unit is written outside the delimiters").
    WordClass wc = classifyWord(rendered);
    if (wc.flags & WordFlags::Dimensioned) {
        out->kind = ValueKind::Dimensioned;
        out->num = wc.value;
    } else if (wc.flags & WordFlags::Integer) {
        out->kind = ValueKind::Integer;
        out->num = wc.value;
    } else if (wc.flags & WordFlags::Decimal) {
        out->kind = ValueKind::Decimal;
        out->num = wc.value;
    } else if (wc.flags & (WordFlags::BoolTrue | WordFlags::BoolFalse)) {
        out->kind = ValueKind::Boolean;
        out->boolean = (wc.flags & WordFlags::BoolTrue) != 0;
        out->upperCaseSpelling = (wc.flags & WordFlags::UpperCase) != 0;
        out->text = interner_.intern(rendered);
    } else {
        out->kind = ValueKind::Identifier;
        out->text = interner_.intern(rendered);
    }
    return out;
}

}  // namespace manta
