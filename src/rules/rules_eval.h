// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Evaluating user rules over an elaborated design.
//
// A small interpreter. Domains are enumerated in Design order, which is already
// deterministic, so a rule's findings come out in the same order every run
// (spec 15.8).
//
// The one performance concern is the pin-pair domain, which is quadratic in the
// pins on a net: a 200-pin power rail is 40,000 pairs per rule. Guards that
// mention only one side are hoisted out of the inner loop and used to filter
// each side once, which is what makes the common case linear.
#pragma once

#include <string>
#include <vector>

#include "base/arena.h"
#include "base/flat_map.h"
#include "base/intern.h"
#include "diag/engine.h"
#include "link/netlist.h"
#include "link/part_info.h"
#include "rules/rules_ast.h"

namespace manta {

// A value during evaluation. `Absent` is what an undeclared attribute yields,
// and it is contagious: any comparison involving it is false rather than an
// error, so a guard written with has() decides whether the rule applies.
enum class RuleValueKind : std::uint8_t {
    Absent,
    Number,
    Boolean,
    Text,
    Pin,
    Net,
    Component,
    Part,
    Collection,
};

struct RuleValue {
    RuleValueKind kind = RuleValueKind::Absent;
    Dimensioned number;
    bool boolean = false;
    std::string text;
    PinRef pin;
    std::uint32_t index = 0;  // net or component index
    std::vector<RuleValue> elements;

    [[nodiscard]] bool present() const noexcept { return kind != RuleValueKind::Absent; }

    static RuleValue absent() { return {}; }
    static RuleValue ofNumber(Dimensioned d);
    static RuleValue ofBoolean(bool b);
    static RuleValue ofText(std::string s);
};

// The bindings in force for one tuple of a domain.
struct RuleScope {
    const Design* design = nullptr;
    const RuleCheck* check = nullptr;

    FlatMap<SymbolId, RuleValue> bindings;
    // Inside an aggregate's "where", a bare name is a property of the element
    // under consideration before it is anything else.
    const RuleValue* element = nullptr;
};

// Everything a rules file declared, plus the checks to run.
struct LoadedRules {
    std::vector<const RuleCheck*> checks;
    FlatMap<SymbolId, const FieldTypeDecl*> fieldTypes;

    [[nodiscard]] bool empty() const noexcept { return checks.empty(); }
};

class RuleEvaluator {
public:
    RuleEvaluator(const LoadedRules& rules, StringInterner& interner, DiagEngine& diags)
        : rules_(rules), interner_(interner), diags_(diags) {}

    // Runs every net, component and pin-pair check over an elaborated design.
    void runOnDesign(const Design& design);

    // Runs every part check over one part declaration.
    //
    // A part is presented as a one-component design, so member access, pin
    // collections and aggregates all work exactly as they do for a net or a
    // component. Building the shim costs one small allocation and saves a
    // second code path that could drift from the first.
    void runOnPart(const PartInfo& part, std::string_view partName);

private:
    void runNetCheck(const RuleCheck& check, const Design& design);
    void runComponentCheck(const RuleCheck& check, const Design& design);
    void runPinPairCheck(const RuleCheck& check, const Design& design);

    // Evaluates the guards and, if they all hold, the requirement. Reports when
    // the requirement is false.
    void apply(const RuleCheck& check, RuleScope& scope, Span at);

    [[nodiscard]] RuleValue eval(const RuleExpr* e, RuleScope& scope);
    [[nodiscard]] RuleValue evalMember(const RuleValue& base, SymbolId name, RuleScope& scope,
                                       Span at);
    [[nodiscard]] RuleValue evalAggregate(const RuleExpr* e, RuleScope& scope);
    [[nodiscard]] RuleValue evalBinary(const RuleExpr* e, RuleScope& scope);

    [[nodiscard]] bool truth(const RuleValue& v) const;
    [[nodiscard]] std::string render(const RuleValue& v) const;
    [[nodiscard]] std::string buildMessage(const RuleCheck& check, RuleScope& scope);

    // True when an expression mentions none of the bindings other than `keep`,
    // so it can be evaluated once per element rather than once per pair.
    [[nodiscard]] bool mentionsOnly(const RuleExpr* e, SymbolId keep, SymbolId other) const;

    void typeError(Span at, std::string message);

    const LoadedRules& rules_;
    StringInterner& interner_;
    DiagEngine& diags_;
    const Design* design_ = nullptr;
    // One type complaint per check is enough; the rest would be the same
    // mistake repeated once per net.
    FlatSet<SymbolId> reportedTypeErrors_;
};

// Collects the checks and field types from a parsed rules file, rejecting a
// check whose name collides with a built-in diagnostic code or mnemonic.
[[nodiscard]] LoadedRules loadRules(const RuleFile& file, StringInterner& interner,
                                    DiagEngine& diags);

}  // namespace manta
