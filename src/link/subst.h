// Substitution evaluation (spec 14).
//
// Spec 14.1: "$...$ is evaluated at link time. A field may be overridden at an
// instantiation, so the value a substitution resolves to depends on how the
// enclosing block was instantiated, which is known only during elaboration."
//
// Spec 14.3 computes with integers and booleans only. Comparison is the bridge
// between the two domains: it takes integers and yields a boolean.
//
// Positions map back through a substitution by length alone (spec 14.6), so a
// diagnostic arising from substituted text still reports the position of the
// substitution in the original source (spec 15.6).
#pragma once

#include <string>

#include "ast/ast.h"
#include "base/intern.h"
#include "diag/engine.h"
#include "link/fields.h"

namespace manta {

// The result of evaluating an expression: an integer or a boolean, never
// anything else (spec 14.3).
struct EvalResult {
    enum class Kind : std::uint8_t { Integer, Boolean, Error } kind = Kind::Error;
    std::int64_t integer = 0;
    bool boolean = false;

    [[nodiscard]] bool ok() const noexcept { return kind != Kind::Error; }
};

class Substituter {
public:
    Substituter(StringInterner& interner, DiagEngine& diags)
        : interner_(interner), diags_(diags) {}

    // Evaluates one "$...$" expression against the fields in force.
    [[nodiscard]] EvalResult eval(const Expr* e, const FieldEnv& env);

    // Renders an interpolated run to text: literal chunks verbatim, expression
    // chunks per the spec 14.7 rendering table.
    [[nodiscard]] std::string render(const InterpText* text, const FieldEnv& env,
                                     bool systemFieldContext = false);

    // Resolves a Name that may contain substitutions to a symbol. Validates
    // that the rendered text is still a legal identifier, which is where E-15
    // catches a substitution that tried to produce a statement or a declaration.
    [[nodiscard]] SymbolId resolveName(const Name& n, const FieldEnv& env);

    // Resolves an array index, which may itself be a substitution (spec 14.6).
    [[nodiscard]] std::int64_t resolveIndex(const Index& i, const FieldEnv& env, bool& ok);

    // Resolves a value that may be an interpolation, returning a fresh Value
    // when substitution occurred and the original otherwise.
    [[nodiscard]] const Value* resolveValue(const Value* v, const FieldEnv& env, Arena& arena);

private:
    // Looks up a field operand. Spec 14.5: "An undefined field is error E-29,
    // never an empty string."
    [[nodiscard]] EvalResult lookupOperand(SymbolId name, Span at, const FieldEnv& env);

    StringInterner& interner_;
    DiagEngine& diags_;
};

}  // namespace manta
