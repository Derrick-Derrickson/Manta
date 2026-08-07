#include "link/part_info.h"

#include <format>

#include "link/fields.h"
#include "sema/registry.h"

namespace manta {

void applyPinField(ComponentPin& pin, const FieldDecl* decl, StringInterner& interner,
                   DiagEngine& diags, bool isOverride) {
    if (!decl || !valid(decl->name.symbol) || decl->ns != FieldNamespace::User) return;

    std::string name(interner.text(decl->name.symbol));
    std::string rendered = renderValue(decl->value, interner);

    PinAttribute incoming;
    incoming.name = name;
    incoming.value = rendered;
    incoming.strength = decl->strength;
    incoming.declaredAt = decl->span;
    if (decl->value) {
        switch (decl->value->kind) {
            case ValueKind::Dimensioned:
            case ValueKind::Integer:
            case ValueKind::Decimal:
            case ValueKind::Percentage:
                incoming.number = decl->value->num;
                incoming.numeric = true;
                break;
            default:
                break;
        }
    }

    for (PinAttribute& existing : pin.attributes) {
        if (existing.name != name) continue;

        // Spec 9.2, unchanged for pins: strongest wins, locked cannot be
        // overridden, and an equal-strength disagreement is a conflict.
        if (existing.strength == Strength::Locked) {
            if (existing.value != rendered) {
                diags.report(DiagId::E11, decl->span, "#", name, "this pin")
                    .note(existing.declaredAt, "locked here");
            }
            return;
        }
        if (decl->strength > existing.strength) {
            existing = incoming;
            return;
        }
        if (decl->strength < existing.strength) return;
        // An override at a call site wins at equal strength; two declarations
        // in the same position conflict (spec 9.2).
        if (isOverride) {
            existing = incoming;
            return;
        }
        if (existing.value != rendered) {
            diags.report(DiagId::E12, decl->span, "#" + name, existing.value, rendered)
                .note(existing.declaredAt, "first declared here");
        }
        return;
    }
    pin.attributes.push_back(std::move(incoming));
}

const ComponentPin* PartInfo::find(SymbolId base, std::int64_t index, bool hasIndex) const {
    if (const std::uint32_t* i = byKey.find(PinKey{base, hasIndex ? index : 0, SymbolId::kInvalid})) {
        return &pins[*i];
    }
    if (!hasIndex) {
        // A scalar reference to an array base is legal when the array has one
        // element, which is how "GPIO[1]" and a one-wide "GPIO" coincide.
        auto elements = arrayElements(base);
        if (elements.size() == 1) return &pins[elements[0]];
    }
    return nullptr;
}

const ComponentPin* PartInfo::findMember(SymbolId base, SymbolId member) const {
    if (const std::uint32_t* i = byKey.find(PinKey{base, 0, member})) return &pins[*i];
    return nullptr;
}

std::vector<std::uint32_t> PartInfo::arrayElements(SymbolId base) const {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < pins.size(); ++i) {
        if (pins[i].base == base) out.push_back(i);
    }
    return out;
}

bool PartInfo::hasBase(SymbolId base) const {
    for (const ComponentPin& p : pins) {
        if (p.base == base) return true;
    }
    return false;
}

namespace {

// Applies one pin map line's directives to every pin it produced.
void applyDirectives(std::span<ComponentPin> pins, const PinMap* line, std::uint32_t lineIndex,
                     StringInterner& interner, DiagEngine& diags) {
    bool sawExplicitSwap = false;
    bool casual = false;

    for (const Directive* d : line->directives) {
        if (!valid(d->name.symbol)) continue;
        std::string_view name = interner.text(d->name.symbol);

        if (name == "CASUAL") {
            casual = true;
            continue;
        }
        if (name == "TYPE" && d->value && d->value->kind == ValueKind::Identifier) {
            PinType t{};
            bool caseError = false;
            if (lookupPinType(interner.text(d->value->text), t, caseError)) {
                for (ComponentPin& p : pins) {
                    p.type = t;
                    p.typeExplicit = true;
                }
            }
            continue;
        }
        if (name == "SWAP" && d->value) {
            sawExplicitSwap = true;
            SymbolId group = d->value->kind == ValueKind::Identifier ? d->value->text
                                                                    : SymbolId::kInvalid;
            for (ComponentPin& p : pins) {
                p.swapGroup = group;
                p.swapGroupImplicit = false;
            }
            continue;
        }
        if (name == "PINDELAY" && d->value && d->value->kind == ValueKind::Dimensioned) {
            for (ComponentPin& p : pins) {
                p.hasPinDelay = true;
                p.pinDelay = d->value->num;
            }
            continue;
        }
        // &NET is a default net, resolved at instantiation rather than here;
        // the elaborator reads it straight off the declaration.
        if (name == "NET") continue;

        diags.report(DiagId::E13, d->name.span, name);
    }

    if (casual) {
        for (ComponentPin& p : pins) p.casual = true;
        if (!sawExplicitSwap) {
            // Spec 11.7: "&CASUAL ... joins a weak implicit group '&~SWAP=CASUAL'.
            // Being weak, the implicit group is replaced by any &SWAP written on
            // the same line, and it is scoped per declaration line, so two
            // independent elements in one part do not become mutually
            // swappable." The line index is what makes the scoping work.
            SymbolId group = interner.intern(std::format("CASUAL#{}", lineIndex));
            for (ComponentPin& p : pins) {
                p.swapGroup = group;
                p.swapGroupImplicit = true;
            }
        }
    }
}

// Spec 11.6: with no &TYPE, a pin is PASSIVE when it has no arrow and SIGNAL
// when it has one.
PinType defaultType(PortDir dir) {
    return dir == PortDir::None ? PinType::Passive : PinType::Signal;
}

}  // namespace

PartInfo buildPartInfo(const Item* part, std::uint32_t objectIndex, StringInterner& interner,
                       DiagEngine& diags) {
    PartInfo info;
    info.decl = part;
    info.objectIndex = objectIndex;
    if (!part) return info;

    std::int32_t order = 0;
    std::uint32_t lineIndex = 0;

    for (const BodyEntry& entry : part->body) {
        if (entry.kind == BodyKind::Field) {
            info.fields.push_back(entry.field);
            continue;
        }
        if (entry.kind != BodyKind::PinMap) continue;

        const PinMap* line = entry.pin;
        ++lineIndex;

        // Physical pins run low to high or high to low; spec 8.1 makes range
        // order significant, and the same applies to the package side.
        std::int64_t step = line->physLo <= line->physHi ? 1 : -1;
        auto count = static_cast<std::size_t>(
            (line->physLo <= line->physHi ? line->physHi - line->physLo
                                          : line->physLo - line->physHi) + 1);

        std::size_t firstPin = info.pins.size();

        for (std::size_t k = 0; k < count; ++k) {
            std::int64_t physical = line->physLo + static_cast<std::int64_t>(k) * step;

            ComponentPin pin;
            pin.physical = std::to_string(physical);
            pin.base = line->logical.symbol;
            pin.direction = line->arrow.dir;
            pin.type = defaultType(line->arrow.dir);
            pin.declOrder = order++;
            pin.span = line->span;

            std::string baseText =
                valid(line->logical.symbol) ? std::string(interner.text(line->logical.symbol)) : "";

            if (line->hasMemberList) {
                // "[12:13] = USB.[+,-]": members in the order written (spec 12.2).
                if (k < line->memberList.size()) {
                    pin.member = line->memberList[k].symbol;
                    pin.logical = baseText + "." + std::string(interner.text(pin.member));
                } else {
                    pin.logical = baseText;
                }
            } else if (line->logicalRange.present) {
                std::int64_t lo = line->logicalRange.lo.literal;
                std::int64_t hi = line->logicalRange.hi.literal;
                std::int64_t lstep = lo <= hi ? 1 : -1;
                pin.index = lo + static_cast<std::int64_t>(k) * lstep;
                pin.isArrayElement = true;
                pin.logical = std::format("{}[{}]", baseText, pin.index);
            } else {
                pin.logical = baseText;
            }

            info.pins.push_back(std::move(pin));
        }

        applyDirectives(std::span<ComponentPin>(info.pins).subspan(firstPin), line, lineIndex,
                        interner, diags);

        // A '#' field on a pin map line applies to every pin the line produced,
        // exactly as its directives do -- so a 48-pin bus states a value once.
        for (std::size_t i = firstPin; i < info.pins.size(); ++i) {
            for (const FieldDecl* f : line->fields) {
                applyPinField(info.pins[i], f, interner, diags);
            }
        }
    }

    // Index after the fact, so the spans handed to applyDirectives stay valid
    // while info.pins is still growing.
    for (std::uint32_t i = 0; i < info.pins.size(); ++i) {
        const ComponentPin& p = info.pins[i];
        PinKey key{p.base, p.isArrayElement ? p.index : 0, p.member};
        auto [slot, inserted] = info.byKey.insert(key, i);
        if (!inserted) {
            diags.report(DiagId::E12, p.span, p.logical, "an earlier pin", "this pin")
                .note(info.pins[*slot].span, "first declared here");
        }
    }

    return info;
}

}  // namespace manta
