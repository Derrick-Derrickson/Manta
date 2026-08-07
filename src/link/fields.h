// Field and directive values, the strength ladder, and scoped environments.
//
// Spec 9.2: "Every field takes a strength modifier ... The strongest declaration
// wins. Two declarations of equal strength with different values are error
// E-12", and overriding a locked field is E-11.
//
// Spec 9.3: "Fields are hierarchical and flow downward only. A field declared on
// a block is visible to every declaration instantiated within it. A field
// declared on a part or an instance stays there and does not propagate upward."
// That is why an environment is a parent-linked chain rather than a flat map:
// lookup walks outward, assignment never does.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "base/flat_map.h"
#include "base/intern.h"
#include "diag/engine.h"

namespace manta {

// A resolved value: the AST node plus the strength it was declared at and where.
struct FieldSlot {
    const Value* value = nullptr;
    Strength strength = Strength::Normal;
    Span declaredAt;
    bool overridden = false;  // tracked for W-06, "a weak field never overridden"
    bool weakDeclared = false;
};

// Field names live in two namespaces, '@' and '#', which never collide.
struct FieldKey {
    SymbolId name = SymbolId::kInvalid;
    FieldNamespace ns = FieldNamespace::User;

    friend bool operator==(const FieldKey&, const FieldKey&) = default;
};

}  // namespace manta

template <>
struct manta::DefaultHash<manta::FieldKey> {
    std::uint64_t operator()(const manta::FieldKey& k) const noexcept {
        return hashCombine(mix64(raw(k.name)), static_cast<std::uint64_t>(k.ns));
    }
};

namespace manta {

// One level of the scope chain. Lookup walks to the parent; declaration never
// leaves the level it was written in.
class FieldEnv {
public:
    FieldEnv() = default;
    explicit FieldEnv(const FieldEnv* parent) : parent_(parent) {}

    // Declares or overrides. Applies the strength ladder and reports E-11/E-12.
    //
    // Spec 9.2: "Declaration and override are distinguished by position: inside
    // a part or block definition you are declaring; inside an instantiation you
    // are overriding." E-12 is about two *declarations* disagreeing, so an
    // override wins at equal strength rather than conflicting -- which is what
    // makes a normal field "overridable, but unusual to do so" rather than
    // impossible to override at all.
    void declare(const FieldDecl* decl, StringInterner& interner, DiagEngine& diags,
                 bool isOverride = false);

    // Direct insertion, for values manta itself supplies (the '!' DNP prefix is
    // "exact sugar for @fitted=FALSE", spec 7.5).
    void set(FieldKey key, const Value* value, Strength strength, Span at);

    [[nodiscard]] const FieldSlot* lookup(FieldKey key) const {
        for (const FieldEnv* e = this; e; e = e->parent_) {
            if (const FieldSlot* s = e->slots_.find(key)) return s;
        }
        return nullptr;
    }

    // Lookup restricted to this level, used when emitting a component's own
    // fields rather than everything visible to it.
    [[nodiscard]] const FieldSlot* lookupLocal(FieldKey key) const { return slots_.find(key); }

    [[nodiscard]] const FieldEnv* parent() const noexcept { return parent_; }
    [[nodiscard]] auto entries() const { return slots_.entries(); }

    // Every field visible here, outermost first so inner levels win. Used to
    // build a component's field set for the BOM.
    void collectVisible(FlatMap<FieldKey, FieldSlot>& out) const;

private:
    const FieldEnv* parent_ = nullptr;
    FlatMap<FieldKey, FieldSlot> slots_;
};

// Compares two values for the purposes of E-12. Two declarations only conflict
// when they disagree, so "#value = 10kR" twice is fine.
[[nodiscard]] bool valuesEqual(const Value* a, const Value* b, const StringInterner& interner);

// Renders a value as the text a BOM, netlist or diagnostic should show.
// Dimensioned values come out in the canonical SI-substituted form (spec 3.2).
[[nodiscard]] std::string renderValue(const Value* v, const StringInterner& interner);

}  // namespace manta
