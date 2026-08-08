// Building a concrete pin list from a 'part' declaration (spec 4.5, 8.2, 11.6).
//
// A pin map line expands to one pin per physical pin:
//
//   1       = VCC<        -> one pin,  physical "1",  logical "VCC"
//   [3:11]  = GPIO[1:9]<> -> nine pins, physical "3".."11", logical "GPIO[1]".."GPIO[9]"
//   [12:13] = USB.[+,-]<> -> two pins,  physical "12","13", logical "USB.+","USB.-"
//
// Spec 4.5: "A part exports all of its pins. There is no separate export
// declaration: every logical name is addressable at a call site as a binding
// target, and as a net reference."
#pragma once

#include <string>
#include <vector>

#include "ast/ast.h"
#include "base/flat_map.h"
#include "diag/engine.h"
#include "link/fields.h"
#include "link/netlist.h"

namespace manta {

// Lookup key for a pin: a base name plus either an array index or a member.
struct PinKey {
    SymbolId base = SymbolId::kInvalid;
    std::int64_t index = 0;
    SymbolId member = SymbolId::kInvalid;

    friend bool operator==(const PinKey&, const PinKey&) = default;
};

}  // namespace manta

template <>
struct manta::DefaultHash<manta::PinKey> {
    std::uint64_t operator()(const manta::PinKey& k) const noexcept {
        return hashCombine(hashCombine(mix64(raw(k.base)), static_cast<std::uint64_t>(k.index)),
                           mix64(raw(k.member)));
    }
};

namespace manta {

struct PartInfo {
    const Item* decl = nullptr;
    std::uint32_t objectIndex = 0;
    std::vector<ComponentPin> pins;
    FlatMap<PinKey, std::uint32_t> byKey;
    // Fields declared on the part itself, before any call-site override.
    std::vector<const FieldDecl*> fields;

    // Resolves a whole array or a single element. `index` is ignored when the
    // pin is scalar. Returns nullptr when the name is not a pin of this part.
    [[nodiscard]] const ComponentPin* find(SymbolId base, std::int64_t index,
                                           bool hasIndex) const;
    [[nodiscard]] const ComponentPin* findMember(SymbolId base, SymbolId member) const;

    // Every pin belonging to one array base, in declared index order. Used to
    // resolve "GPIO[1:9] = IO[1:9]" and to give a terminal its width.
    [[nodiscard]] std::vector<std::uint32_t> arrayElements(SymbolId base) const;

    // True when the part declares any pin under this base name.
    [[nodiscard]] bool hasBase(SymbolId base) const;
};

// Builds a part's pin list. Diagnostics raised here are local ones deferred
// from compile because they need the whole declaration in hand.
PartInfo buildPartInfo(const Item* part, std::uint32_t objectIndex, StringInterner& interner,
                       DiagEngine& diags);

// Applies one '#' field to a pin, honouring the strength ladder of spec 9.2:
// the strongest declaration wins, an equal-strength disagreement is E-12, and
// overriding a locked one is E-11.
void applyPinField(ComponentPin& pin, const FieldDecl* decl, StringInterner& interner,
                   DiagEngine& diags);

}  // namespace manta
