// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sema/local_check.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "sema/registry.h"

namespace manta {

std::string_view LocalChecker::text(const Name& n) const {
    if (n.isInterpolated() || !valid(n.symbol)) return {};
    return interner_.text(n.symbol);
}

std::int64_t LocalChecker::rangeWidth(const Range& r) const {
    if (!r.present) return 1;
    if (r.lo.isExpr || r.hi.isExpr) return -1;  // width known only after substitution
    std::int64_t lo = r.lo.literal;
    std::int64_t hi = r.hi.literal;
    return (lo <= hi ? hi - lo : lo - hi) + 1;
}

// ---------------------------------------------------------------------------
// Directives
// ---------------------------------------------------------------------------

void LocalChecker::checkValueType(const Directive* d, ValueType expected) {
    const Value* v = d->value;
    if (!v) return;
    // A substituted value is not known until link.
    if (v->kind == ValueKind::Interp) return;

    std::string_view name = text(d->name);

    switch (expected) {
        case ValueType::Resistance:
        case ValueType::Current:
        case ValueType::Voltage:
        case ValueType::Time: {
            Unit want = expectedUnit(expected);
            if (v->kind != ValueKind::Dimensioned) {
                diags_.report(DiagId::Type, v->span,
                              std::format("'&{}' takes a value in '{}'", name, unitSuffix(want)));
                return;
            }
            if (v->num.unit != want) {
                // Spec 11.4 singles this one out: "Tolerance shall be a time,
                // never a length ... A tolerance given as a length is error
                // E-18." The same mistake on &MAXDELAY gets the general code.
                if (expected == ValueType::Time && isLengthUnit(v->num.unit)) {
                    diags_.report(DiagId::E18, v->span, v->num.canonical());
                } else {
                    diags_.report(DiagId::Type, v->span,
                                  std::format("'&{}' takes a value in '{}', not '{}'", name,
                                              unitSuffix(want), unitSuffix(v->num.unit)));
                }
            }
            return;
        }

        case ValueType::PinType: {
            if (v->kind != ValueKind::Identifier) {
                diags_.report(DiagId::Type, v->span,
                              std::format("'&{}' takes a type name", name));
                return;
            }
            PinType t{};
            bool caseError = false;
            std::string_view spelled = interner_.text(v->text);
            if (!lookupPinType(spelled, t, caseError)) {
                diags_.report(DiagId::Type, v->span,
                              std::format("'{}' is not a pin type", spelled));
                return;
            }
            if (caseError) {
                // Spec 2.6: "&TYPE=power" is E-34; "#status = power" is fine,
                // because a user field value is unconstrained.
                diags_.report(DiagId::E34, v->span, spelled, pinTypeName(t));
            }
            return;
        }

        case ValueType::RenderMode: {
            if (v->kind != ValueKind::Identifier) {
                diags_.report(DiagId::Type, v->span,
                              std::format("'&{}' takes WIRE or LABEL", name));
                return;
            }
            NetRenderMode mode{};
            bool caseError = false;
            std::string_view spelled = interner_.text(v->text);
            if (!lookupRenderMode(spelled, mode, caseError)) {
                diags_.report(DiagId::Type, v->span,
                              std::format("'{}' is not a render mode; write WIRE or LABEL",
                                          spelled));
                return;
            }
            if (caseError) {
                // Spec 2.6, exactly as "&TYPE=power": a fixed-set directive
                // value shall be upper case.
                diags_.report(DiagId::E34, v->span, spelled, renderModeName(mode));
            }
            return;
        }
        case ValueType::Edge: {
            if (v->kind != ValueKind::Identifier) {
                diags_.report(DiagId::Type, v->span,
                              std::format("'&{}' takes a sheet edge", name));
                return;
            }
            EdgeSide side{};
            bool caseError = false;
            std::string_view spelled = interner_.text(v->text);
            if (!lookupEdgeSide(spelled, side, caseError)) {
                diags_.report(DiagId::Type, v->span,
                              std::format("'{}' is not a sheet edge; write LEFT, RIGHT, "
                                          "TOP or BOTTOM",
                                          spelled));
                return;
            }
            if (caseError) {
                // Spec 2.6, exactly as "&TYPE=power": a fixed-set directive
                // value shall be upper case.
                diags_.report(DiagId::E34, v->span, spelled, edgeSideName(side));
            }
            return;
        }

        case ValueType::None:
            if (v) {
                diags_.report(DiagId::Type, v->span,
                              std::format("'&{}' takes no value", name));
            }
            return;

        default:
            return;  // identifiers, net names and match groups accept any word
    }
}

void LocalChecker::checkDirective(const Directive* d, std::uint8_t context) {
    std::string_view name = text(d->name);
    if (name.empty()) return;  // interpolated; resolved at link

    const DirectiveInfo* info = lookupDirective(name);
    if (!info) {
        std::string_view suggestion = nearestDirective(name);
        auto builder = diags_.report(DiagId::E13, d->name.span, name);
        if (!suggestion.empty()) {
            builder.note(d->name.span, std::format("did you mean '&{}'?", suggestion));
        }
        return;
    }

    if ((info->contexts & context) == 0) {
        diags_.report(DiagId::E13, d->name.span, name)
            .note(d->name.span,
                  std::format("'&{}' is not permitted in this position", name));
        return;
    }

    if (info->valueRequired && !d->value && !d->matchRef) {
        // Spec 11.1: "&CASUAL and &STUB take no value; every other directive
        // requires one."
        diags_.report(DiagId::Type, d->span,
                      std::format("'&{}' requires a value", name));
        return;
    }

    if (!info->valueRequired && d->value) {
        diags_.report(DiagId::Type, d->value->span,
                      std::format("'&{}' takes no value", name));
        return;
    }

    checkValueType(d, info->type);
}

// ---------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------

void LocalChecker::checkFieldDecl(const FieldDecl* f, bool inPart, bool inMatch) {
    // Spec 9.4: "A part shall not export a field. Export is available to blocks
    // only, and '#!>' inside a part definition is error E-43."
    if (inPart && f->direction == FieldDirection::Export) {
        std::string_view n = text(f->name);
        diags_.report(DiagId::E43, f->span, n.empty() ? std::string_view("<substituted>") : n);
    }

    // Spec 9.4: "Export requires locked strength, so that a global's value
    // cannot be changed by whichever object happens to link last."
    if (f->direction == FieldDirection::Export && f->strength != Strength::Locked) {
        diags_.report(DiagId::Type, f->span,
                      std::format("exporting '{}' requires locked strength; write '{}!>{}'",
                                  text(f->name), f->ns == FieldNamespace::System ? "@" : "#",
                                  text(f->name)));
    }

    if (f->ns != FieldNamespace::System) return;  // '#' is an open namespace

    std::string_view name = text(f->name);
    if (name.empty()) return;

    const SystemFieldInfo* info = lookupSystemField(name);
    if (!info) {
        std::string_view suggestion = nearestSystemField(name);
        auto builder = diags_.report(DiagId::E10, f->name.span, name);
        if (!suggestion.empty()) {
            builder.note(f->name.span, std::format("did you mean '@{}'?", suggestion));
        }
        return;
    }

    if (info->matchOnly && !inMatch) {
        diags_.report(DiagId::E10, f->name.span, name)
            .note(f->name.span,
                  std::format("'@{}' is a match-group field (spec 11.4)", name));
        return;
    }

    if (!f->value) return;

    if (info->type == ValueType::Boolean) {
        if (f->value->kind != ValueKind::Boolean) {
            if (f->value->kind != ValueKind::Interp) {
                diags_.report(DiagId::Type, f->value->span,
                              std::format("'@{}' takes TRUE or FALSE", name));
            }
            return;
        }
        // Spec 3.6: "TRUE and FALSE in system fields."
        if (!f->value->upperCaseSpelling) {
            std::string_view spelled = interner_.text(f->value->text);
            diags_.report(DiagId::E34, f->value->span, spelled,
                          f->value->boolean ? "TRUE" : "FALSE");
        }
        return;
    }

    if (info->type == ValueType::Time && f->value->kind == ValueKind::Dimensioned &&
        isLengthUnit(f->value->num.unit)) {
        // "@tolerance = 5mm" inside a match group.
        diags_.report(DiagId::E18, f->value->span, f->value->num.canonical());
    }
}

// ---------------------------------------------------------------------------
// Chains
// ---------------------------------------------------------------------------

void LocalChecker::checkDevice(const Device* dev) {
    const Instance* inst = dev->instance;
    if (!inst) return;

    // Spec 7.3: "A pin used as a terminal shall not also appear in the binding
    // list. That is error E-08."
    //
    // "The same pin" means the same pin, not merely the same array. "IO[1]" as
    // a terminal and "IO[3]" in the binding list are two different pins and are
    // perfectly well formed; comparing base names alone would reject them.
    struct PinRefSpan {
        std::string_view base;
        Range range;
        Span at;
    };

    std::vector<PinRefSpan> terminals;
    if (dev->hasEntry && !dev->entry.dot) {
        std::string_view n = text(dev->entry.name);
        if (!n.empty()) terminals.push_back(PinRefSpan{n, dev->entry.range, dev->entry.span});
    }
    if (dev->hasExit && !dev->exit.dot) {
        std::string_view n = text(dev->exit.name);
        if (!n.empty()) terminals.push_back(PinRefSpan{n, dev->exit.range, dev->exit.span});
    }

    // Two references to one array overlap unless both name index ranges that do
    // not intersect. A bare name means the whole array, so it overlaps anything.
    auto overlaps = [](const Range& a, const Range& b) {
        if (!a.present || !b.present) return true;
        if (a.lo.isExpr || a.hi.isExpr || b.lo.isExpr || b.hi.isExpr) {
            return true;  // an index that needs substitution is settled at link
        }
        std::int64_t aLo = std::min(a.lo.literal, a.hi.literal);
        std::int64_t aHi = std::max(a.lo.literal, a.hi.literal);
        std::int64_t bLo = std::min(b.lo.literal, b.hi.literal);
        std::int64_t bHi = std::max(b.lo.literal, b.hi.literal);
        return aLo <= bHi && bLo <= aHi;
    };

    for (const Binding* b : inst->bindings) {
        if (b->kind == BindingKind::Field) {
            checkFieldDecl(b->field, false, false);
            continue;
        }
        if (b->kind == BindingKind::Directive) {
            // Revision 1.5: a directive written bare in a binding list -- no
            // pin in front of it -- annotates the *instance*, so it is checked
            // against the Instance context. Before 1.5 such a binding was
            // checked as if it sat on a pin and then applied to nothing at
            // all; see docs/assumptions.md, F2.
            checkDirective(b->directive, DirCtx::Instance);
            continue;
        }
        for (const Directive* d : b->pinDirectives) checkDirective(d, DirCtx::Pin);
        for (const FieldDecl* f : b->pinFields) checkFieldDecl(f, false, false);

        // Spec 7.4: a binding's right-hand side is "an ordinary segment (19),
        // with all of 6's connectors and all of 8's grouping and replication
        // available", so every check a segment gets applies inside one too --
        // including the devices it declares and their own binding lists.
        if (b->rhs) checkSegment(b->rhs);

        if (b->pinIsDot) continue;
        std::string_view pin = text(b->pin);
        if (pin.empty()) continue;

        // A binding that only carries directives or fields annotates the pin
        // rather than connecting it, so it is not a second connection to a pin
        // the chain already passes through. A binding that carries a chain does
        // connect it (spec 7.4), so E-08 applies to it exactly as it does to
        // "PIN = NET".
        bool connects = b->net != nullptr || b->rhs != nullptr || b->unbind;
        if (!connects) continue;

        for (const PinRefSpan& t : terminals) {
            if (t.base == pin && overlaps(t.range, b->pinRange)) {
                diags_.report(DiagId::E08, b->span, pin)
                    .note(t.at, "used as a chain terminal here");
            }
        }
    }
}

void LocalChecker::checkElement(const Element* el) {
    switch (el->kind) {
        case ElementKind::Device:
            checkDevice(el->device);
            return;
        case ElementKind::Group:
            if (el->group->body) checkSegment(el->group->body);
            return;
        case ElementKind::Replication:
            if (el->replication->body) checkSegment(el->replication->body);
            return;
        case ElementKind::Net:
            return;
    }
}

void LocalChecker::checkSegment(const Segment* seg) {
    // Revision 1.6, spec 6.2: '=' is the plain join, two bare net names
    // included -- "A = B;" puts them on one node. E-22 is retired; the '=='
    // pairing rule that replaced it is enforced where the brackets are read,
    // in the parser (E-49).
    for (const Element* el : seg->elements) checkElement(el);
}

void LocalChecker::checkChain(const Chain* chain) {
    for (const Segment* seg : chain->segments) checkSegment(seg);
}

void LocalChecker::checkStatement(const Stmt* stmt) {
    for (const Directive* d : stmt->directives) checkDirective(d, DirCtx::Net);

    switch (stmt->kind) {
        case StmtKind::Field:
            checkFieldDecl(stmt->field, false, false);
            return;

        case StmtKind::PortList:
            return;

        case StmtKind::Chain: {
            checkChain(stmt->chain);

            // Spec 4.4: "A block's interface is the set of nets in its body
            // carrying a direction arrow. The arrow is mandatory on a block
            // port ... A port with no arrow is error E-32."
            //
            // A statement that is one bare net name, with no connection and no
            // directive, declares nothing electrically -- spec 5.1 is explicit
            // that "a net exists because it is named" -- so it can only have
            // been meant as a port declaration (spec 10.2).
            const Chain* c = stmt->chain;
            if (c->segments.size() != 1) return;
            const Segment* seg = c->segments[0];
            if (seg->elements.size() != 1) return;
            const Element* el = seg->elements[0];
            if (el->kind != ElementKind::Net) return;
            if (!stmt->directives.empty()) return;

            const NetExpr* net = el->net;
            if (net->leading.present() || net->trailing.present()) return;
            if (net->perCopy || net->hasMemberList) return;

            std::string_view n = net->path.empty() ? std::string_view{} : text(net->path[0]);
            diags_.report(DiagId::E32, net->span,
                          n.empty() ? std::string_view("<substituted>") : n);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Pin maps
// ---------------------------------------------------------------------------

void LocalChecker::checkPinMap(const PinMap* pin) {
    for (const Directive* d : pin->directives) checkDirective(d, DirCtx::Pin);

    // Spec 11.6 (revision 2.0): a part declares its pins; where they connect
    // is the design's decision. '&NET' in a pin declaration -- the old weak
    // default net -- is error E-50. '&NET' at an instance is untouched.
    for (const Directive* d : pin->directives) {
        if (text(d->name) == "NET") {
            std::string_view n = text(pin->logical);
            diags_.report(DiagId::E50, d->span,
                          n.empty() ? std::string_view("<substituted>") : n);
        }
    }

    std::int64_t physWidth =
        (pin->physLo <= pin->physHi ? pin->physHi - pin->physLo : pin->physLo - pin->physHi) + 1;

    if (pin->hasMemberList) {
        // Spec 12.2: "'NAME.[member, ...]' selects members in the order
        // written. Its length shall equal the width of the pin range."
        auto members = static_cast<std::int64_t>(pin->memberList.size());
        if (members != physWidth) {
            diags_.report(DiagId::E38, pin->span,
                          std::format("member list has {} member{} but the pin range is {} wide",
                                      members, members == 1 ? "" : "s", physWidth));
        }
        return;
    }

    // Spec 8.2: "Within a part, a contiguous run of physical pins maps to an
    // array. Widths shall match."
    std::int64_t logicalWidth = rangeWidth(pin->logicalRange);
    if (logicalWidth < 0) return;  // depends on a substitution
    if (logicalWidth != physWidth) {
        diags_.report(DiagId::E04, pin->span, physWidth, logicalWidth);
    }
}

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

void LocalChecker::checkBlockBody(const Item* block) {
    for (const BodyEntry& e : block->body) {
        switch (e.kind) {
            case BodyKind::Item: checkItem(e.item); break;
            case BodyKind::Stmt: checkStatement(e.stmt); break;
            default: break;
        }
    }
}

void LocalChecker::checkPartBody(const Item* part) {
    for (const BodyEntry& e : part->body) {
        switch (e.kind) {
            case BodyKind::Field: checkFieldDecl(e.field, /*inPart=*/true, false); break;
            case BodyKind::PinMap: checkPinMap(e.pin); break;
            default: break;
        }
    }
}

void LocalChecker::checkHarnessBody(const Item* harness) {
    for (const BodyEntry& e : harness->body) {
        switch (e.kind) {
            case BodyKind::Directive: checkDirective(e.directive, DirCtx::Harness); break;
            case BodyKind::Member:
                for (const Directive* d : e.member->directives) {
                    checkDirective(d, DirCtx::Harness | DirCtx::Pin);
                }
                break;
            default: break;
        }
    }
}

void LocalChecker::checkNetclassBody(const Item* netclass) {
    for (const BodyEntry& e : netclass->body) {
        if (e.kind == BodyKind::Directive) checkDirective(e.directive, DirCtx::Netclass);
    }
}

void LocalChecker::checkMatchBody(const Item* group) {
    for (const BodyEntry& e : group->body) {
        switch (e.kind) {
            case BodyKind::Field: checkFieldDecl(e.field, false, /*inMatch=*/true); break;
            case BodyKind::Item: checkItem(e.item); break;
            default: break;
        }
    }
}

void LocalChecker::checkItem(const Item* item) {
    if (!item) return;
    switch (item->kind) {
        case ItemKind::Block: checkBlockBody(item); break;
        case ItemKind::Part: checkPartBody(item); break;
        case ItemKind::Harness: checkHarnessBody(item); break;
        case ItemKind::Netclass: checkNetclassBody(item); break;
        case ItemKind::Match: checkMatchBody(item); break;
        // A cable body is a chain, so it checks exactly as a block body does.
        // What may be *instantiated* in one is narrower -- only a cable
        // connector, a wire or a crimp -- but that needs each part's '@type',
        // which lives in whichever object declares it. It is a link-time rule
        // (E-44), not one this file can see.
        case ItemKind::Cable: checkBlockBody(item); break;
    }
}

void LocalChecker::run(const SourceUnit& unit) {
    for (const Item* item : unit.items) checkItem(item);
}

}  // namespace manta
