#include "rules/rules_eval.h"

#include <algorithm>
#include <format>

#include "link/fields.h"

namespace manta {

RuleValue RuleValue::ofNumber(Dimensioned d) {
    RuleValue v;
    v.kind = RuleValueKind::Number;
    v.number = d;
    return v;
}

RuleValue RuleValue::ofBoolean(bool b) {
    RuleValue v;
    v.kind = RuleValueKind::Boolean;
    v.boolean = b;
    return v;
}

RuleValue RuleValue::ofText(std::string s) {
    RuleValue v;
    v.kind = RuleValueKind::Text;
    v.text = std::move(s);
    return v;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

LoadedRules loadRules(const RuleFile& file, StringInterner& interner, DiagEngine& diags) {
    LoadedRules out;

    for (const RuleSet* set : file.sets) {
        for (const FieldTypeDecl* f : set->fields) {
            if (!valid(f->name)) continue;
            auto [slot, inserted] = out.fieldTypes.insert(f->name, f);
            if (!inserted && (*slot)->unit != f->unit) {
                diags.report(DiagId::Type, f->span,
                             std::format("'#{}' is declared as {} here and as {} elsewhere",
                                         interner.text(f->name), unitName(f->unit),
                                         unitName((*slot)->unit)));
            }
        }

        for (const RuleCheck* c : set->checks) {
            if (!valid(c->name)) continue;
            std::string_view name = interner.text(c->name);

            // A check's name is its diagnostic code, so it cannot be one
            // already: "-Wno-drive-high" has to mean one thing.
            DiagId existing{};
            if (lookupDiag(name, existing)) {
                diags.report(DiagId::Type, c->nameSpan,
                             std::format("check '{}' collides with the built-in diagnostic '{}'",
                                         name, diagInfo(existing).code));
                continue;
            }
            for (const RuleCheck* seen : out.checks) {
                if (seen->name == c->name) {
                    diags.report(DiagId::Type, c->nameSpan,
                                 std::format("check '{}' is declared twice", name));
                    break;
                }
            }
            out.checks.push_back(c);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

bool RuleEvaluator::truth(const RuleValue& v) const {
    switch (v.kind) {
        case RuleValueKind::Boolean: return v.boolean;
        case RuleValueKind::Absent: return false;
        // Anything that exists is true. A number's magnitude is deliberately
        // not consulted: 0V is a value the design states, not an absence.
        default: return true;
    }
}

std::string RuleEvaluator::render(const RuleValue& v) const {
    switch (v.kind) {
        case RuleValueKind::Absent: return "<absent>";
        case RuleValueKind::Number: return v.number.canonical();
        case RuleValueKind::Boolean: return v.boolean ? "true" : "false";
        case RuleValueKind::Text: return v.text;
        case RuleValueKind::Pin: {
            const Component& c = design_->components[v.pin.component];
            const ComponentPin& p = c.pins[v.pin.pin];
            std::string_view designator =
                c.designator.empty() ? std::string_view(c.identity) : std::string_view(c.designator);
            return std::string(designator) + "." + p.logical;
        }
        case RuleValueKind::Net: return design_->nets[v.index].name;
        case RuleValueKind::Component: {
            const Component& c = design_->components[v.index];
            return c.designator.empty() ? c.identity : c.designator;
        }
        case RuleValueKind::Part: return v.text;
        case RuleValueKind::Collection: {
            std::string out;
            for (std::size_t i = 0; i < v.elements.size(); ++i) {
                if (i) out += ", ";
                out += render(v.elements[i]);
            }
            return out;
        }
    }
    return {};
}

void RuleEvaluator::typeError(Span at, std::string message) {
    if (!rules_.checks.empty()) {
        // Deduplicated by check, so one mistake in a rules file is reported
        // once and not once per net in the design.
        SymbolId key = interner_.intern(message);
        if (!reportedTypeErrors_.insert(key)) return;
    }
    diags_.report(DiagId::Type, at, std::move(message));
}

// ---------------------------------------------------------------------------
// Member access
// ---------------------------------------------------------------------------

namespace {

std::string_view directionName(PortDir d) {
    switch (d) {
        case PortDir::None: return "none";
        case PortDir::In: return "in";
        case PortDir::Out: return "out";
        case PortDir::Bidir: return "bidir";
    }
    return "none";
}

std::string_view pinTypeText(PinType t) {
    switch (t) {
        case PinType::Passive: return "passive";
        case PinType::Signal: return "signal";
        case PinType::Power: return "power";
        case PinType::OpenDrain: return "opendrain";
        case PinType::NC: return "nc";
        case PinType::Ground: return "ground";
    }
    return "passive";
}

// A string that parses as a quantity becomes a number, so a rule can compare it.
RuleValue fromText(std::string text) {
    Dimensioned d;
    if (parseDimensioned(text, d) && d.unit != Unit::None) return RuleValue::ofNumber(d);
    if (text == "TRUE" || text == "true") return RuleValue::ofBoolean(true);
    if (text == "FALSE" || text == "false") return RuleValue::ofBoolean(false);
    return RuleValue::ofText(std::move(text));
}

}  // namespace

RuleValue RuleEvaluator::evalMember(const RuleValue& base, SymbolId name, RuleScope& scope,
                                    Span at) {
    std::string_view field = interner_.text(name);

    switch (base.kind) {
        case RuleValueKind::Pin: {
            const Component& c = design_->components[base.pin.component];
            const ComponentPin& p = c.pins[base.pin.pin];

            if (field == "direction") return RuleValue::ofText(std::string(directionName(p.direction)));
            if (field == "type") return RuleValue::ofText(std::string(pinTypeText(p.type)));
            if (field == "name") return RuleValue::ofText(p.logical);
            if (field == "physical") return RuleValue::ofText(p.physical);
            if (field == "part") return RuleValue::ofText(c.partName);
            if (field == "component") {
                return RuleValue::ofText(c.designator.empty() ? c.identity : c.designator);
            }
            if (const PinAttribute* a = p.attribute(field)) {
                return a->numeric ? RuleValue::ofNumber(a->number) : fromText(a->value);
            }
            return RuleValue::absent();
        }

        case RuleValueKind::Net: {
            const Net& n = design_->nets[base.index];
            if (field == "name") return RuleValue::ofText(n.name);
            if (field == "ground") return RuleValue::ofBoolean(n.ground);
            if (field == "global") return RuleValue::ofBoolean(n.global);
            if (field == "direction") return RuleValue::ofText(std::string(directionName(n.direction)));
            if (field == "pins") {
                RuleValue out;
                out.kind = RuleValueKind::Collection;
                for (const PinRef& r : n.pins) {
                    RuleValue p;
                    p.kind = RuleValueKind::Pin;
                    p.pin = r;
                    out.elements.push_back(std::move(p));
                }
                return out;
            }
            if (const NetDirective* d = n.directives.find(std::string(field))) {
                return fromText(d->value);
            }
            return RuleValue::absent();
        }

        case RuleValueKind::Component: {
            const Component& c = design_->components[base.index];
            if (field == "designator") {
                return RuleValue::ofText(c.designator.empty() ? c.identity : c.designator);
            }
            // "part" names the part a component instantiates; "name" is the
            // same thing, which is what a part-domain check reads.
            if (field == "part" || field == "name") return RuleValue::ofText(c.partName);
            if (field == "footprint") return RuleValue::ofText(c.footprint);
            if (field == "fitted") return RuleValue::ofBoolean(c.fitted);
            if (field == "bom") return RuleValue::ofBoolean(c.bom);
            if (field == "pins") {
                RuleValue out;
                out.kind = RuleValueKind::Collection;
                for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
                    RuleValue p;
                    p.kind = RuleValueKind::Pin;
                    p.pin = PinRef{base.index, i};
                    out.elements.push_back(std::move(p));
                }
                return out;
            }
            for (const auto& [fname, value] : c.fields) {
                if (fname == field) return fromText(value);
            }
            return RuleValue::absent();
        }

        case RuleValueKind::Part: {
            if (field == "name") return RuleValue::ofText(base.text);
            return RuleValue::absent();
        }

        // Member access over a collection projects: "pins.DRAW" is the DRAW of
        // each pin, absences included, so count() and any() see them.
        case RuleValueKind::Collection: {
            RuleValue out;
            out.kind = RuleValueKind::Collection;
            out.elements.reserve(base.elements.size());
            for (const RuleValue& element : base.elements) {
                out.elements.push_back(evalMember(element, name, scope, at));
            }
            return out;
        }

        default:
            return RuleValue::absent();
    }
}

// ---------------------------------------------------------------------------
// Aggregates
// ---------------------------------------------------------------------------

RuleValue RuleEvaluator::evalAggregate(const RuleExpr* e, RuleScope& scope) {
    // "sum(pins.DRAW where direction == in)" filters the *pins*, then projects.
    // That is what the phrase means: sum DRAW over the pins that are inputs.
    std::vector<RuleValue> values;

    // "pins.DRAW" is a projection over a collection. Splitting it out is what
    // lets a filter apply to the *pins* -- "sum DRAW over the pins that are
    // inputs" -- and it is also how an empty sum learns its unit, so the
    // splitting happens whether or not a filter was written.
    const RuleExpr* projection = nullptr;
    const RuleExpr* source = e->lhs;
    if (source && source->kind == RuleExprKind::Member) {
        RuleValue probe = eval(source->lhs, scope);
        if (probe.kind == RuleValueKind::Collection) {
            projection = source;
            source = source->lhs;
        }
    }

    RuleValue base = eval(source, scope);
    if (base.kind != RuleValueKind::Collection) {
        // An aggregate over a single value is that value, which keeps
        // "sum(x)" from being an error nobody meant to write.
        values.push_back(std::move(base));
    } else {
        for (const RuleValue& element : base.elements) {
            if (e->filter) {
                const RuleValue* saved = scope.element;
                scope.element = &element;
                RuleValue keep = eval(e->filter, scope);
                scope.element = saved;
                if (!truth(keep)) continue;
            }
            values.push_back(projection ? evalMember(element, projection->text, scope, e->span)
                                        : element);
        }
    }

    switch (e->aggregate) {
        case Aggregate::Count: {
            std::size_t n = 0;
            for (const RuleValue& v : values) {
                if (v.present()) ++n;
            }
            Dimensioned d;
            d.mantissa = static_cast<std::int64_t>(n);
            return RuleValue::ofNumber(d);
        }
        case Aggregate::Any: {
            for (const RuleValue& v : values) {
                if (v.present() && truth(v)) return RuleValue::ofBoolean(true);
            }
            return RuleValue::ofBoolean(false);
        }
        case Aggregate::All: {
            for (const RuleValue& v : values) {
                if (!v.present() || !truth(v)) return RuleValue::ofBoolean(false);
            }
            return RuleValue::ofBoolean(true);
        }
        case Aggregate::Sum: {
            // An empty sum is zero in the declared unit, so a guard does not
            // have to special-case a net nobody decorated.
            Unit unit = Unit::None;
            if (projection) {
                if (const FieldTypeDecl* const* t = rules_.fieldTypes.find(projection->text)) {
                    unit = (*t)->unit;
                }
            }
            Dimensioned total = zeroOf(unit);
            bool started = false;
            for (const RuleValue& v : values) {
                if (v.kind != RuleValueKind::Number) continue;
                if (!started) {
                    total = v.number;
                    started = true;
                    continue;
                }
                bool ok = false;
                Dimensioned sum = addValues(total, v.number, ok);
                if (!ok) {
                    typeError(e->span,
                              std::format("sum mixes {} with {}", unitName(total.unit),
                                          unitName(v.number.unit)));
                    return RuleValue::absent();
                }
                total = sum;
            }
            return RuleValue::ofNumber(total);
        }
        case Aggregate::Min:
        case Aggregate::Max: {
            // Absent over an empty collection: there is no smallest of nothing,
            // and inventing one would make a guard silently true. A comparison
            // against absent is false, which is the right answer.
            const Dimensioned* best = nullptr;
            for (const RuleValue& v : values) {
                if (v.kind != RuleValueKind::Number) continue;
                if (!best) {
                    best = &v.number;
                    continue;
                }
                if (!unitsCompatible(*best, v.number)) {
                    typeError(e->span,
                              std::format("{} mixes {} with {}", aggregateName(e->aggregate),
                                          unitName(best->unit), unitName(v.number.unit)));
                    return RuleValue::absent();
                }
                int cmp = compareMagnitude(v.number, *best);
                if ((e->aggregate == Aggregate::Min && cmp < 0) ||
                    (e->aggregate == Aggregate::Max && cmp > 0)) {
                    best = &v.number;
                }
            }
            return best ? RuleValue::ofNumber(*best) : RuleValue::absent();
        }
    }
    return RuleValue::absent();
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

RuleValue RuleEvaluator::evalBinary(const RuleExpr* e, RuleScope& scope) {
    // '&' and '|' short-circuit, so "has(x) & x > y" is safe to write.
    if (e->binOp == RuleBinOp::And) {
        return RuleValue::ofBoolean(truth(eval(e->lhs, scope)) && truth(eval(e->rhs, scope)));
    }
    if (e->binOp == RuleBinOp::Or) {
        return RuleValue::ofBoolean(truth(eval(e->lhs, scope)) || truth(eval(e->rhs, scope)));
    }

    RuleValue a = eval(e->lhs, scope);
    RuleValue b = eval(e->rhs, scope);

    switch (e->binOp) {
        case RuleBinOp::Equal:
        case RuleBinOp::NotEqual: {
            bool same = false;
            if (!a.present() || !b.present()) {
                same = !a.present() && !b.present();
            } else if (a.kind == RuleValueKind::Number && b.kind == RuleValueKind::Number) {
                same = unitsCompatible(a.number, b.number) &&
                       compareMagnitude(a.number, b.number) == 0;
            } else if (a.kind == RuleValueKind::Boolean && b.kind == RuleValueKind::Boolean) {
                same = a.boolean == b.boolean;
            } else {
                same = render(a) == render(b);
            }
            return RuleValue::ofBoolean(e->binOp == RuleBinOp::Equal ? same : !same);
        }

        case RuleBinOp::Less:
        case RuleBinOp::LessEqual:
        case RuleBinOp::Greater:
        case RuleBinOp::GreaterEqual: {
            // An absent operand makes a comparison false rather than an error:
            // whether the rule applies at all is what a guard decides.
            if (!a.present() || !b.present()) return RuleValue::ofBoolean(false);
            if (a.kind != RuleValueKind::Number || b.kind != RuleValueKind::Number) {
                typeError(e->span,
                          std::format("'{}' compares quantities, not {}", ruleBinOpText(e->binOp),
                                      a.kind == RuleValueKind::Number ? "text" : "text"));
                return RuleValue::ofBoolean(false);
            }
            // This is what the type declarations are for: comparing a current
            // against a voltage is a mistake in the rules file, not a check
            // that silently always passes.
            if (!unitsCompatible(a.number, b.number)) {
                typeError(e->span, std::format("'{}' compares {} with {}",
                                               ruleBinOpText(e->binOp), unitName(a.number.unit),
                                               unitName(b.number.unit)));
                return RuleValue::ofBoolean(false);
            }
            int cmp = compareMagnitude(a.number, b.number);
            switch (e->binOp) {
                case RuleBinOp::Less: return RuleValue::ofBoolean(cmp < 0);
                case RuleBinOp::LessEqual: return RuleValue::ofBoolean(cmp <= 0);
                case RuleBinOp::Greater: return RuleValue::ofBoolean(cmp > 0);
                default: return RuleValue::ofBoolean(cmp >= 0);
            }
        }

        case RuleBinOp::Add:
        case RuleBinOp::Subtract: {
            if (a.kind != RuleValueKind::Number || b.kind != RuleValueKind::Number) {
                return RuleValue::absent();
            }
            bool ok = false;
            Dimensioned r = e->binOp == RuleBinOp::Add ? addValues(a.number, b.number, ok)
                                                       : subtractValues(a.number, b.number, ok);
            if (!ok) {
                typeError(e->span, std::format("'{}' mixes {} with {}", ruleBinOpText(e->binOp),
                                               unitName(a.number.unit), unitName(b.number.unit)));
                return RuleValue::absent();
            }
            return RuleValue::ofNumber(r);
        }

        case RuleBinOp::Multiply:
        case RuleBinOp::Divide: {
            if (a.kind != RuleValueKind::Number || b.kind != RuleValueKind::Number) {
                return RuleValue::absent();
            }
            // Only scaling by a bare number: a rules language that multiplied
            // volts by amps would need a unit algebra, and none of the checks
            // this is for want one.
            if (b.number.unit != Unit::None) {
                typeError(e->span, std::format("'{}' scales by a bare number, not by {}",
                                               ruleBinOpText(e->binOp),
                                               unitName(b.number.unit)));
                return RuleValue::absent();
            }
            if (e->binOp == RuleBinOp::Multiply) {
                return RuleValue::ofNumber(scaleValue(a.number, b.number.mantissa));
            }
            if (b.number.mantissa == 0) {
                typeError(e->span, std::string("division by zero"));
                return RuleValue::absent();
            }
            Dimensioned r = a.number;
            r.mantissa /= b.number.mantissa;
            return RuleValue::ofNumber(r);
        }

        default:
            return RuleValue::absent();
    }
}

RuleValue RuleEvaluator::eval(const RuleExpr* e, RuleScope& scope) {
    if (!e) return RuleValue::absent();

    switch (e->kind) {
        case RuleExprKind::Number: return RuleValue::ofNumber(e->number);
        case RuleExprKind::String: return RuleValue::ofText(std::string(interner_.text(e->text)));

        case RuleExprKind::Name: {
            // A binding first, then a property of the element under an
            // aggregate's filter, then a bare word standing for itself -- which
            // is what makes "direction == out" work with no enum table.
            if (const RuleValue* bound = scope.bindings.find(e->text)) return *bound;
            if (scope.element) {
                RuleValue v = evalMember(*scope.element, e->text, scope, e->span);
                if (v.present()) return v;
            }
            return RuleValue::ofText(std::string(interner_.text(e->text)));
        }

        case RuleExprKind::Member:
            return evalMember(eval(e->lhs, scope), e->text, scope, e->span);

        case RuleExprKind::Has:
            return RuleValue::ofBoolean(eval(e->lhs, scope).present());

        case RuleExprKind::Aggregate:
            return evalAggregate(e, scope);

        case RuleExprKind::Unary: {
            RuleValue v = eval(e->lhs, scope);
            if (e->unOp == RuleUnOp::Not) return RuleValue::ofBoolean(!truth(v));
            if (v.kind != RuleValueKind::Number) return RuleValue::absent();
            Dimensioned d = v.number;
            d.mantissa = -d.mantissa;
            return RuleValue::ofNumber(d);
        }

        case RuleExprKind::Binary:
            return evalBinary(e, scope);
    }
    return RuleValue::absent();
}

// ---------------------------------------------------------------------------
// Messages and reporting
// ---------------------------------------------------------------------------

std::string RuleEvaluator::buildMessage(const RuleCheck& check, RuleScope& scope) {
    std::string out;
    for (const MessageChunk& chunk : check.message) {
        if (chunk.isExpr) {
            out += render(eval(chunk.expr, scope));
        } else {
            out += interner_.text(chunk.literal);
        }
    }
    return out;
}

void RuleEvaluator::apply(const RuleCheck& check, RuleScope& scope, Span at) {
    for (const RuleExpr* guard : check.guards) {
        if (!truth(eval(guard, scope))) return;
    }
    if (truth(eval(check.requirement, scope))) return;

    Severity severity =
        check.severity == RuleSeverity::Error ? Severity::Error : Severity::Warning;
    diags_.reportUser(std::string(interner_.text(check.name)), severity, at,
                      buildMessage(check, scope));
}

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

void RuleEvaluator::runNetCheck(const RuleCheck& check, const Design& design) {
    SymbolId netName = interner_.intern("net");
    SymbolId pinsName = interner_.intern("pins");

    for (std::uint32_t i = 0; i < design.nets.size(); ++i) {
        RuleScope scope;
        scope.design = &design;
        scope.check = &check;

        RuleValue net;
        net.kind = RuleValueKind::Net;
        net.index = i;
        scope.bindings.set(pinsName, evalMember(net, pinsName, scope, check.span));
        scope.bindings.set(netName, std::move(net));

        apply(check, scope, design.nets[i].firstSeen);
    }
}

void RuleEvaluator::runComponentCheck(const RuleCheck& check, const Design& design) {
    SymbolId componentName = interner_.intern("component");
    SymbolId pinsName = interner_.intern("pins");

    for (std::uint32_t i = 0; i < design.components.size(); ++i) {
        RuleScope scope;
        scope.design = &design;
        scope.check = &check;

        RuleValue component;
        component.kind = RuleValueKind::Component;
        component.index = i;
        scope.bindings.set(pinsName, evalMember(component, pinsName, scope, check.span));
        scope.bindings.set(componentName, std::move(component));

        apply(check, scope, design.components[i].span);
    }
}

bool RuleEvaluator::mentionsOnly(const RuleExpr* e, SymbolId keep, SymbolId other) const {
    if (!e) return true;
    if (e->kind == RuleExprKind::Name && e->text == other) return false;
    (void)keep;
    return mentionsOnly(e->lhs, keep, other) && mentionsOnly(e->rhs, keep, other) &&
           mentionsOnly(e->filter, keep, other);
}

void RuleEvaluator::runPinPairCheck(const RuleCheck& check, const Design& design) {
    SymbolId netName = interner_.intern("net");
    SymbolId pinsName = interner_.intern("pins");
    SymbolId left = check.leftBinding;
    SymbolId right = check.rightBinding;

    // Guards that mention only one binding filter that side once, rather than
    // being re-tested for every pair. Without this a 200-pin power net is
    // 40,000 evaluations per guard.
    std::vector<const RuleExpr*> leftOnly, rightOnly, both;
    for (const RuleExpr* guard : check.guards) {
        bool noRight = mentionsOnly(guard, left, right);
        bool noLeft = mentionsOnly(guard, right, left);
        if (noRight && !noLeft) leftOnly.push_back(guard);
        else if (noLeft && !noRight) rightOnly.push_back(guard);
        else both.push_back(guard);
    }

    for (std::uint32_t n = 0; n < design.nets.size(); ++n) {
        const Net& net = design.nets[n];
        if (net.pins.size() < 2) continue;

        RuleScope base;
        base.design = &design;
        base.check = &check;
        RuleValue netValue;
        netValue.kind = RuleValueKind::Net;
        netValue.index = n;
        RuleValue pins = evalMember(netValue, pinsName, base, check.span);

        auto passes = [&](const std::vector<const RuleExpr*>& guards, SymbolId binding,
                          const RuleValue& pin) {
            RuleScope scope;
            scope.design = &design;
            scope.check = &check;
            scope.bindings.set(netName, netValue);
            scope.bindings.set(pinsName, pins);
            scope.bindings.set(binding, pin);
            for (const RuleExpr* g : guards) {
                if (!truth(eval(g, scope))) return false;
            }
            return true;
        };

        std::vector<RuleValue> lefts, rights;
        for (const RuleValue& pin : pins.elements) {
            if (passes(leftOnly, left, pin)) lefts.push_back(pin);
            if (passes(rightOnly, right, pin)) rights.push_back(pin);
        }
        if (lefts.empty() || rights.empty()) continue;

        for (const RuleValue& a : lefts) {
            for (const RuleValue& b : rights) {
                // A pin is never paired with itself: a driver does not have to
                // clear its own input threshold.
                if (a.pin.component == b.pin.component && a.pin.pin == b.pin.pin) continue;

                RuleScope scope;
                scope.design = &design;
                scope.check = &check;
                scope.bindings.set(netName, netValue);
                scope.bindings.set(pinsName, pins);
                scope.bindings.set(left, a);
                scope.bindings.set(right, b);

                bool guarded = true;
                for (const RuleExpr* g : both) {
                    if (!truth(eval(g, scope))) {
                        guarded = false;
                        break;
                    }
                }
                if (!guarded) continue;
                if (truth(eval(check.requirement, scope))) continue;

                Severity severity =
                    check.severity == RuleSeverity::Error ? Severity::Error : Severity::Warning;
                diags_.reportUser(std::string(interner_.text(check.name)), severity,
                                  design.components[a.pin.component].pins[a.pin.pin].span,
                                  buildMessage(check, scope));
            }
        }
    }
}

void RuleEvaluator::runOnDesign(const Design& design) {
    design_ = &design;
    for (const RuleCheck* check : rules_.checks) {
        switch (check->domain) {
            case RuleDomain::Net: runNetCheck(*check, design); break;
            case RuleDomain::Component: runComponentCheck(*check, design); break;
            case RuleDomain::PinPair: runPinPairCheck(*check, design); break;
            case RuleDomain::Part: break;  // run over declarations, not the design
        }
    }
}

void RuleEvaluator::runOnPart(const PartInfo& part, std::string_view partName) {
    bool wanted = false;
    for (const RuleCheck* check : rules_.checks) {
        if (check->domain == RuleDomain::Part) wanted = true;
    }
    if (!wanted) return;

    // Present the declaration as a design of one component. Member access, pin
    // collections and aggregates then work exactly as they do at link, rather
    // than needing a parallel implementation that could drift from it.
    Design shim;
    Component component;
    component.designator = std::string(partName);
    component.identity = component.designator;
    component.partName = std::string(partName);
    component.pins = part.pins;
    component.span = part.decl ? part.decl->span : Span{};

    // The part's own fields, resolved through the strength ladder exactly as
    // instantiation would. Without this a part rule could not see the very
    // things a part declares -- its footprint, its value, its type.
    FieldEnv env;
    DiagEngine quiet(diags_.sources());  // a field conflict is already reported elsewhere
    for (const FieldDecl* f : part.fields) env.declare(f, interner_, quiet);

    FlatMap<FieldKey, FieldSlot> visible;
    env.collectVisible(visible);
    for (const auto& [key, slot] : visible) {
        if (!slot.value) continue;
        std::string rendered = renderValue(slot.value, interner_);
        if (key.ns == FieldNamespace::System) {
            std::string_view name = interner_.text(key.name);
            if (name == "footprint") component.footprint = rendered;
            else if (name == "fitted") component.fitted = rendered != "FALSE";
            else if (name == "bom") component.bom = rendered != "FALSE";
            continue;
        }
        component.fields.emplace_back(std::string(interner_.text(key.name)),
                                      std::move(rendered));
    }

    shim.components.push_back(std::move(component));

    const Design* savedDesign = design_;
    design_ = &shim;

    SymbolId partBinding = interner_.intern("part");
    SymbolId pinsName = interner_.intern("pins");

    for (const RuleCheck* check : rules_.checks) {
        if (check->domain != RuleDomain::Part) continue;

        RuleScope scope;
        scope.design = &shim;
        scope.check = check;

        RuleValue partValue;
        partValue.kind = RuleValueKind::Component;
        partValue.index = 0;
        scope.bindings.set(pinsName, evalMember(partValue, pinsName, scope, check->span));
        scope.bindings.set(partBinding, std::move(partValue));

        apply(*check, scope, shim.components[0].span);
    }

    design_ = savedDesign;
}

}  // namespace manta
