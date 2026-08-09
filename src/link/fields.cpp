#include "link/fields.h"

#include <format>

namespace manta {

bool valuesEqual(const Value* a, const Value* b, const StringInterner& interner) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case ValueKind::Integer:
        case ValueKind::Decimal:
        case ValueKind::Dimensioned:
        case ValueKind::Percentage:
        case ValueKind::Tolerance:
            return a->num == b->num;
        case ValueKind::Range:
            return a->rangeLo == b->rangeLo && a->rangeHi == b->rangeHi;
        case ValueKind::String:
        case ValueKind::Identifier:
            return a->text == b->text;
        case ValueKind::Boolean:
            return a->boolean == b->boolean;
        case ValueKind::List: {
            if (a->list.size() != b->list.size()) return false;
            for (std::size_t i = 0; i < a->list.size(); ++i) {
                if (!valuesEqual(a->list[i], b->list[i], interner)) return false;
            }
            return true;
        }
        case ValueKind::Repeat:
            return a->count == b->count && valuesEqual(a->inner, b->inner, interner);
        case ValueKind::Version:
            return a->text == b->text;
        case ValueKind::Unbind:
            return true;
        case ValueKind::Interp:
            // Two unevaluated substitutions are compared after evaluation; the
            // elaborator only ever reaches here with resolved values.
            return a->interp == b->interp;
    }
    return false;
}

std::string renderValue(const Value* v, const StringInterner& interner) {
    if (!v) return {};
    switch (v->kind) {
        case ValueKind::Integer:
        case ValueKind::Decimal:
        case ValueKind::Dimensioned:
        case ValueKind::Percentage:
            return v->num.canonical();
        case ValueKind::Tolerance:
            // Spec 3.3 writes the sign explicitly; the canonical form keeps it.
            return "\xC2\xB1" + v->num.canonical();
        case ValueKind::String:
        case ValueKind::Identifier:
        case ValueKind::Version:
            return std::string(interner.text(v->text));
        case ValueKind::Boolean:
            // Spec 14.7: TRUE/FALSE in a system field, true/false in a user
            // field. The spelling written is what round-trips.
            return v->upperCaseSpelling ? (v->boolean ? "TRUE" : "FALSE")
                                        : (v->boolean ? "true" : "false");
        case ValueKind::Range:
            // Rendered as written, descending included: a reversed pin map is
            // "20:1" and reordering it would change what it means.
            return std::format("{}:{}", v->rangeLo, v->rangeHi);
        case ValueKind::List: {
            // Spec 14.7: a list renders comma-separated, without brackets, so
            // it can drop straight into a %[...] or @dest position.
            std::string out;
            for (std::size_t i = 0; i < v->list.size(); ++i) {
                if (i) out += ',';
                out += renderValue(v->list[i], interner);
            }
            return out;
        }
        case ValueKind::Repeat: {
            std::string one = renderValue(v->inner, interner);
            std::string out;
            for (std::int64_t i = 0; i < v->count; ++i) {
                if (i) out += ',';
                out += one;
            }
            return out;
        }
        case ValueKind::Unbind:
            return "?";
        case ValueKind::Interp:
            return "$...$";  // never reached after elaboration
    }
    return {};
}

void FieldEnv::set(FieldKey key, const Value* value, Strength strength, Span at) {
    FieldSlot slot;
    slot.value = value;
    slot.strength = strength;
    slot.declaredAt = at;
    slot.weakDeclared = strength == Strength::Weak;
    slots_.set(key, slot);
}

void FieldEnv::declare(const FieldDecl* decl, StringInterner& interner, DiagEngine& diags) {
    if (!decl || !valid(decl->name.symbol)) return;
    FieldKey key{decl->name.symbol, decl->ns};

    std::string_view sigil = decl->ns == FieldNamespace::System ? "@" : "#";
    std::string_view name = interner.text(decl->name.symbol);

    FieldSlot* existing = slots_.find(key);
    if (!existing) {
        // Not declared at this level. If an outer level declared it locked,
        // this is an override attempt and must be refused (spec 9.2).
        if (const FieldSlot* outer = lookup(key); outer && outer->strength == Strength::Locked) {
            if (!valuesEqual(outer->value, decl->value, interner)) {
                diags.report(DiagId::E11, decl->span, sigil, name, "an enclosing scope")
                    .note(outer->declaredAt, "locked here");
                return;
            }
        }
        FieldSlot slot;
        slot.value = decl->value;
        slot.strength = decl->strength;
        slot.declaredAt = decl->span;
        slot.weakDeclared = decl->strength == Strength::Weak;
        slots_.insert(key, slot);
        return;
    }

    // Spec 9.2: "Not overridable. Overriding is error E-11."
    if (existing->strength == Strength::Locked) {
        if (!valuesEqual(existing->value, decl->value, interner)) {
            diags.report(DiagId::E11, decl->span, sigil, name, "this scope")
                .note(existing->declaredAt, "locked here");
        }
        return;
    }

    // Spec 9.2: "The strongest declaration wins."
    if (decl->strength > existing->strength) {
        existing->overridden = true;
        existing->value = decl->value;
        existing->strength = decl->strength;
        existing->declaredAt = decl->span;
        return;
    }

    if (decl->strength < existing->strength) {
        // A weaker declaration is simply ignored, but it still counts as the
        // stronger one having *been* an override for W-06 purposes.
        return;
    }

    // Spec 9.2: "Two declarations of equal strength with different values are
    // error E-12." A call site that means to override writes a stronger one.
    if (!valuesEqual(existing->value, decl->value, interner)) {
        diags.report(DiagId::E12, decl->span, std::format("{}{}", sigil, name),
                     renderValue(existing->value, interner), renderValue(decl->value, interner))
            .note(existing->declaredAt, "first declared here");
    }
}

void FieldEnv::collectVisible(FlatMap<FieldKey, FieldSlot>& out) const {
    // Outermost first, so that inner declarations overwrite outer ones and the
    // result is what the innermost scope sees (spec 9.3).
    if (parent_) parent_->collectVisible(out);
    for (const auto& [key, slot] : slots_) out.set(key, slot);
}

}  // namespace manta
