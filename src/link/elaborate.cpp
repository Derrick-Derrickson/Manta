#include "link/elaborate.h"

#include <algorithm>
#include <format>

#include "sema/registry.h"

namespace manta {

namespace {

constexpr int kMaxInstantiationDepth = 64;

}  // namespace

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

std::uint32_t Elaborator::freshNode(Span at) {
    std::uint32_t id = uf_.add();
    nodeInfo_.push_back(NodeInfo{});
    nodeInfo_[id].firstSeen = at;
    return id;
}

std::uint32_t Elaborator::netNode(Scope& scope, SymbolId name, std::int64_t index, bool indexed,
                                  Span at, bool countReference) {
    NetKey key{scope.id, name, indexed ? index : 0, indexed};
    if (std::uint32_t* existing = netNodes_.find(key)) {
        if (countReference) ++nodeInfo_[*existing].references;
        return *existing;
    }

    std::uint32_t id = freshNode(at);
    nodeInfo_[id].explicitName = true;
    nodeInfo_[id].name = indexed ? std::format("{}[{}]", interner_.text(name), index)
                                 : std::string(interner_.text(name));
    if (countReference) nodeInfo_[id].references = 1;
    netNodes_.insert(key, id);
    return id;
}

void Elaborator::unite(std::uint32_t a, std::uint32_t b) {
    std::uint32_t ra = uf_.find(a);
    std::uint32_t rb = uf_.find(b);
    if (ra == rb) return;

    std::uint32_t root = uf_.unite(a, b);
    std::uint32_t other = root == ra ? rb : ra;

    // Merge the descriptive information onto whichever node became the root.
    NodeInfo& keep = nodeInfo_[root];
    NodeInfo& gone = nodeInfo_[other];
    if (!keep.explicitName && gone.explicitName) {
        keep.explicitName = true;
        keep.name = gone.name;
    }
    if (keep.component < 0 && gone.component >= 0) {
        keep.component = gone.component;
        keep.pin = gone.pin;
    }
    keep.references += gone.references;
    keep.global = keep.global || gone.global;
    if (keep.direction == PortDir::None) keep.direction = gone.direction;
    if (!keep.firstSeen.valid()) keep.firstSeen = gone.firstSeen;
}

void Elaborator::uniteBundles(const Bundle& a, const Bundle& b, Span at) {
    if (a.empty() || b.empty()) return;

    if (a.size() == b.size()) {
        for (std::size_t i = 0; i < a.size(); ++i) unite(a[i], b[i]);
        return;
    }
    // Spec 6.5: "A width mismatch without either operator is error E-04."
    diags_.report(DiagId::E04, at, static_cast<std::int64_t>(a.size()),
                  static_cast<std::int64_t>(b.size()));
}

SymbolId Elaborator::resolve(const Name& n, Scope& scope) {
    if (!n.isInterpolated()) return n.symbol;
    return subst_.resolveName(n, *scope.fields);
}

// ---------------------------------------------------------------------------
// Parts
// ---------------------------------------------------------------------------

const PartInfo* Elaborator::partInfoFor(SymbolId name, std::uint32_t objectIndex, Span at) {
    const Declaration* decl = symbols_.find(name, objectIndex);
    if (!decl || decl->item->kind != ItemKind::Part) return nullptr;

    auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(decl->item));
    if (auto* cached = partCache_.find(key)) return cached->get();

    auto info = std::make_unique<PartInfo>(
        buildPartInfo(decl->item, decl->objectIndex, interner_, diags_));
    PartInfo* raw = info.get();
    partCache_.insert(key, std::move(info));
    (void)at;
    return raw;
}

// ---------------------------------------------------------------------------
// Widths
// ---------------------------------------------------------------------------

std::int64_t Elaborator::terminalWidth(const Terminal& t, const PartInfo* part, Scope& scope) {
    if (t.dot) return 1;
    if (!part) return 1;

    SymbolId base = resolve(t.name, scope);
    if (!valid(base)) return 1;

    if (t.range.present) {
        bool ok = true;
        std::int64_t lo = subst_.resolveIndex(t.range.lo, *scope.fields, ok);
        std::int64_t hi = subst_.resolveIndex(t.range.hi, *scope.fields, ok);
        if (!ok) return 1;
        return (lo <= hi ? hi - lo : lo - hi) + 1;
    }

    // A bare array name in a terminal position is the whole array.
    auto elements = part->arrayElements(base);
    return elements.empty() ? 1 : static_cast<std::int64_t>(elements.size());
}

std::int64_t Elaborator::staticWidth(const Element* el, Scope& scope) {
    switch (el->kind) {
        case ElementKind::Net: {
            const NetExpr* n = el->net;
            if (n->hasMemberList) return static_cast<std::int64_t>(n->memberList.size());
            if (n->hasPerCopyList) return static_cast<std::int64_t>(n->perCopyList.size());
            if (!n->range.present) return -1;  // inferred from the other side (spec 8.1)
            bool ok = true;
            std::int64_t lo = subst_.resolveIndex(n->range.lo, *scope.fields, ok);
            std::int64_t hi = subst_.resolveIndex(n->range.hi, *scope.fields, ok);
            if (!ok) return -1;
            return (lo <= hi ? hi - lo : lo - hi) + 1;
        }
        case ElementKind::Device: {
            const Device* d = el->device;
            if (!d->instance || !d->instance->declares) return -1;
            SymbolId partName = resolve(d->instance->partOrBlock, scope);
            const PartInfo* part = partInfoFor(partName, scope.objectIndex, d->span);
            if (!part) return -1;
            if (d->hasEntry) return terminalWidth(d->entry, part, scope);
            if (d->hasExit) return terminalWidth(d->exit, part, scope);
            return -1;
        }
        case ElementKind::Group:
            return -1;
        case ElementKind::Replication:
            if (el->replication->counted) return el->replication->inWidth;
            return -1;
    }
    return -1;
}

// Propagates widths along a segment until a fixed point. Spec 8.1: "Where one
// side omits its range it is inferred from the other."
void Elaborator::inferWidths(const Segment* seg, Scope& scope, std::vector<ElemWidth>& widths) {
    std::size_t n = seg->elements.size();
    widths.assign(n, ElemWidth{});

    for (std::size_t i = 0; i < n; ++i) {
        std::int64_t w = staticWidth(seg->elements[i], scope);
        if (w > 0) {
            widths[i].in = w;
            widths[i].out = w;
            widths[i].fixed = true;
        }
        if (seg->elements[i]->kind == ElementKind::Replication &&
            seg->elements[i]->replication->counted) {
            widths[i].in = seg->elements[i]->replication->inWidth;
            widths[i].out = seg->elements[i]->replication->outWidth;
            widths[i].fixed = true;
        }
        if (seg->elements[i]->kind == ElementKind::Device) {
            const Device* d = seg->elements[i]->device;
            if (d->instance && d->instance->declares) {
                SymbolId partName = resolve(d->instance->partOrBlock, scope);
                if (const PartInfo* part = partInfoFor(partName, scope.objectIndex, d->span)) {
                    if (d->hasEntry) widths[i].in = terminalWidth(d->entry, part, scope);
                    if (d->hasExit) widths[i].out = terminalWidth(d->exit, part, scope);
                }
            }
        }
    }

    // Two passes each way settles any chain in practice: the only propagation
    // is across adjacent connectors.
    for (int pass = 0; pass < 4; ++pass) {
        for (std::size_t i = 0; i + 1 < n; ++i) {
            Connector c = seg->connectors[i];
            if (c == Connector::Gather) {
                if (widths[i + 1].in < 0) widths[i + 1].in = 1;
                if (widths[i + 1].out < 0) widths[i + 1].out = 1;
                continue;
            }
            if (c == Connector::Broadcast) {
                if (widths[i].out < 0) widths[i].out = 1;
                if (widths[i].in < 0) widths[i].in = 1;
                continue;
            }
            if (widths[i].out > 0 && widths[i + 1].in < 0) widths[i + 1].in = widths[i].out;
            if (widths[i + 1].in > 0 && widths[i].out < 0) widths[i].out = widths[i + 1].in;
        }
        // A bare net has one width, so entry and exit agree.
        for (std::size_t i = 0; i < n; ++i) {
            if (seg->elements[i]->kind != ElementKind::Net &&
                seg->elements[i]->kind != ElementKind::Group) {
                continue;
            }
            if (widths[i].in > 0 && widths[i].out < 0) widths[i].out = widths[i].in;
            if (widths[i].out > 0 && widths[i].in < 0) widths[i].in = widths[i].out;
        }
        for (std::size_t k = n; k-- > 1;) {
            std::size_t i = k - 1;
            Connector c = seg->connectors[i];
            if (c == Connector::Gather || c == Connector::Broadcast) continue;
            if (widths[i + 1].in > 0 && widths[i].out < 0) widths[i].out = widths[i + 1].in;
            if (widths[i].out > 0 && widths[i + 1].in < 0) widths[i + 1].in = widths[i].out;
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        if (widths[i].in < 0) widths[i].in = 1;
        if (widths[i].out < 0) widths[i].out = 1;
    }
}

// ---------------------------------------------------------------------------
// Net expressions
// ---------------------------------------------------------------------------

std::string Elaborator::netDisplayName(const NetExpr* net, Scope& scope) {
    std::string out;
    for (std::size_t i = 0; i < net->path.size(); ++i) {
        if (i) out += '.';
        SymbolId s = resolve(net->path[i], scope);
        if (valid(s)) out += interner_.text(s);
    }
    return out;
}

Elaborator::ElemValue Elaborator::evalNet(const NetExpr* net, Scope& scope,
                                          std::int64_t expectedWidth,
                                          std::vector<std::uint32_t>& touched) {
    ElemValue value;
    value.isBareNet = true;
    value.span = net->span;

    // Spec 8.5: "'%' supplies a distinct value to each copy. %NAME[range] draws
    // the nth element for the nth copy. %[a,b,c] supplies an explicit list,
    // whose length shall equal the copy count."
    if (net->perCopy) {
        if (copyIndex_ < 0) {
            diags_.report(DiagId::Type, net->span,
                          std::string("a per-copy selector '%' is only meaningful inside a "
                                      "replication or a multiplicity group"));
            value.entry.push_back(freshNode(net->span));
            value.exit = value.entry;
            value.hasEntry = value.hasExit = true;
            return value;
        }

        if (net->hasPerCopyList) {
            auto listSize = static_cast<std::int64_t>(net->perCopyList.size());
            if (listSize != copyCount_) {
                diags_.report(DiagId::E04, net->span, copyCount_, listSize);
            }
            SymbolId picked = SymbolId::kInvalid;
            if (copyIndex_ < listSize) {
                const Value* v = subst_.resolveValue(net->perCopyList[
                                                         static_cast<std::size_t>(copyIndex_)],
                                                     *scope.fields, arena_);
                picked = interner_.intern(renderValue(v, interner_));
            }
            std::uint32_t node = valid(picked) ? netNode(scope, picked, 0, false, net->span)
                                               : freshNode(net->span);
            value.entry.push_back(node);
            value.exit = value.entry;
            value.hasEntry = value.hasExit = true;
            touched.push_back(node);
            return value;
        }

        // "%AMP-EN[0:3]": the copyIndex_-th element of the named array.
        SymbolId base = interner_.intern(netDisplayName(net, scope));
        std::int64_t element = copyIndex_;
        if (net->range.present) {
            bool ok = true;
            std::int64_t lo = subst_.resolveIndex(net->range.lo, *scope.fields, ok);
            std::int64_t hi = subst_.resolveIndex(net->range.hi, *scope.fields, ok);
            if (ok) {
                std::int64_t step = lo <= hi ? 1 : -1;
                element = lo + copyIndex_ * step;
                std::int64_t width = (lo <= hi ? hi - lo : lo - hi) + 1;
                if (copyIndex_ >= width) {
                    diags_.report(DiagId::E04, net->span, copyCount_, width);
                    element = lo;
                }
            }
        }
        std::uint32_t node = netNode(scope, base, element, true, net->span);
        value.entry.push_back(node);
        value.exit = value.entry;
        value.hasEntry = value.hasExit = true;
        touched.push_back(node);
        return value;
    }

    // "USB.[+,-]" selects members in the order written (spec 12.2).
    if (net->hasMemberList) {
        std::string base = netDisplayName(net, scope);
        for (const Name& m : net->memberList) {
            SymbolId member = resolve(m, scope);
            SymbolId full = interner_.intern(base + "." + std::string(interner_.text(member)));
            std::uint32_t node = netNode(scope, full, 0, false, net->span);
            value.entry.push_back(node);
        }
        value.exit = value.entry;
        value.hasEntry = value.hasExit = true;
        for (std::uint32_t nd : value.entry) touched.push_back(nd);
        return value;
    }

    // Spec 5.2: "A pin belongs to exactly one net, so 'U1.GPIO1' denotes that
    // net whether read as the pin or as the net at the pin. This holds
    // everywhere, not only for unnamed nodes." So a two-segment path whose head
    // is a designator in scope resolves to that component's pin, joining the
    // reference to the real pin rather than inventing a net that happens to
    // share its spelling.
    //
    // A dotted name that is *not* a designator is a harness member, implied if
    // never declared (spec 12.3), and is simply a net whose name contains a dot.
    if (net->path.size() == 2 && !net->hasMemberList) {
        SymbolId head = resolve(net->path[0], scope);
        SymbolId tail = resolve(net->path[1], scope);
        if (const std::uint32_t* ci = scope.instances.find(head)) {
            Component& c = components_[*ci];
            std::vector<std::uint32_t> matched;
            for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
                if (c.pins[i].base == tail) matched.push_back(i);
            }
            if (matched.empty()) {
                std::string_view wanted = interner_.text(tail);
                for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
                    const ComponentPin& p = c.pins[i];
                    // An element of a pin array is addressed without brackets:
                    // spec 5.2 writes "U1.GPIO1" for the pin the part declares
                    // as "[3:11] = GPIO[1:9]", and spec 20.7 uses "U1.GPIO9".
                    if (p.isArrayElement &&
                        wanted == std::string(interner_.text(p.base)) + std::to_string(p.index)) {
                        matched.push_back(i);
                        break;
                    }
                    // A pin may also be addressed by its physical number, since
                    // spec 2.3 permits an integer as a pin name.
                    if (p.physical == wanted) {
                        matched.push_back(i);
                        break;
                    }
                }
            }
            if (!matched.empty()) {
                for (std::uint32_t i : matched) {
                    value.entry.push_back(c.pins[i].node);
                    c.pins[i].connected = true;
                    ++nodeInfo_[uf_.find(c.pins[i].node)].references;
                    touched.push_back(c.pins[i].node);
                }
                value.exit = value.entry;
                value.hasEntry = value.hasExit = true;
                value.isBareNet = false;  // this names a device terminal
                return value;
            }
        }
    }

    SymbolId name = interner_.intern(netDisplayName(net, scope));
    if (!valid(name)) {
        value.entry.push_back(freshNode(net->span));
        value.exit = value.entry;
        value.hasEntry = value.hasExit = true;
        return value;
    }

    if (net->range.present) {
        bool ok = true;
        std::int64_t lo = subst_.resolveIndex(net->range.lo, *scope.fields, ok);
        std::int64_t hi = subst_.resolveIndex(net->range.hi, *scope.fields, ok);
        if (ok) {
            // Spec 8.1: "Range order is significant and defines wire order."
            std::int64_t step = lo <= hi ? 1 : -1;
            for (std::int64_t k = lo;; k += step) {
                value.entry.push_back(netNode(scope, name, k, true, net->span));
                if (k == hi) break;
            }
        }
    } else if (expectedWidth > 1) {
        // Spec 8.1: "Where one side omits its range it is inferred from the
        // other." The inferred array is zero-based.
        for (std::int64_t k = 0; k < expectedWidth; ++k) {
            value.entry.push_back(netNode(scope, name, k, true, net->span));
        }
    } else {
        value.entry.push_back(netNode(scope, name, 0, false, net->span));
    }

    value.exit = value.entry;
    value.hasEntry = value.hasExit = !value.entry.empty();

    // Ports (spec 10). A leading '>' is an input, a trailing '>' an output.
    PortSpec port = net->leading.present() ? net->leading : net->trailing;
    if (port.present()) {
        for (std::uint32_t nd : value.entry) {
            NodeInfo& info = nodeInfo_[uf_.find(nd)];
            info.direction = port.dir;
            info.global = info.global || port.global;
        }
    }

    for (std::uint32_t nd : value.entry) touched.push_back(nd);
    return value;
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

std::uint32_t Elaborator::instantiatePart(const Instance* inst, const PartInfo& part,
                                          Scope& scope, const FieldEnv& callSiteFields) {
    auto index = static_cast<std::uint32_t>(components_.size());
    components_.emplace_back();
    Component& c = components_.back();

    c.path = scope.path;
    c.span = inst->span;
    c.pins = part.pins;

    SymbolId partName = resolve(inst->partOrBlock, scope);
    c.part = partName;
    c.partName = valid(partName) ? std::string(interner_.text(partName)) : std::string{};

    // The designator, when the author assigned one. An unassigned instance
    // "carries an internal identity derived from its block instance path"
    // (spec 13.1), so an un-annotated design still links and checks.
    std::string prefix = valid(inst->designator.prefix.symbol)
                             ? std::string(interner_.text(inst->designator.prefix.symbol))
                             : std::string{};
    if (inst->designator.kind == DesignatorKind::Numbered) {
        c.designator = prefix + std::to_string(inst->designator.number);
    } else if (inst->designator.kind == DesignatorKind::Range) {
        // Spec 13.3: "Where one statement instantiates a part or block N times,
        // the annotator writes a range designator: one token carrying N
        // designators." The copies take them in order, and the range may be
        // non-contiguous so that a group which grows never forces the
        // renumbering of designators already assigned.
        std::vector<std::int64_t> numbers;
        for (const DesigPart& range : inst->designator.parts) {
            for (std::int64_t n = range.lo; n <= range.hi; ++n) numbers.push_back(n);
        }
        auto which = static_cast<std::size_t>(copyIndex_ < 0 ? 0 : copyIndex_);
        if (which < numbers.size()) {
            c.designator = prefix + std::to_string(numbers[which]);
        } else {
            diags_.report(DiagId::Type, inst->designator.span,
                          std::format("designator range carries {} designator{} but this "
                                      "statement instantiates at least {}",
                                      numbers.size(), numbers.size() == 1 ? "" : "s",
                                      which + 1));
        }
    }
    // Spec 13.1: "Unassigned instances carry internal identities derived from
    // their block instance path." Path first, so the identity reads as a
    // location; the index disambiguates two unassigned instances of the same
    // prefix in one scope.
    c.identity = (scope.path.empty() ? "" : flattenPath(scope.path) + ".") + prefix + "?" +
                 std::to_string(index);
    c.path.push_back(c.designator.empty() ? c.identity : c.designator);

    // Fields: the part's own declarations, then the call site's overrides.
    FieldEnv env(&callSiteFields);
    for (const FieldDecl* f : part.fields) {
        env.declare(f, interner_, diags_);
        // Spec 16.2 W-06: a '~'-weak field that is never overridden anywhere in
        // the design was written as a suggestion nobody took, which usually
        // means the author expected a call site to supply it.
        if (f->strength == Strength::Weak && valid(f->name.symbol)) {
            weakFields_.insert(
                hashCombine(mix64(raw(f->name.symbol)),
                            mix64(reinterpret_cast<std::uintptr_t>(f))),
                WeakField{f->name.symbol, f->ns, f->span, part.decl->name.symbol});
        }
    }
    for (const Binding* b : inst->bindings) {
        if (b->kind == BindingKind::Field) {
            env.declare(b->field, interner_, diags_);
            if (valid(b->field->name.symbol)) {
                overriddenFields_.insert(FieldKey{b->field->name.symbol, b->field->ns});
            }
        }
    }

    // Spec 7.5: "A '!' prefix on the designator marks the instance as not
    // fitted. It is exact sugar for @fitted=FALSE."
    FieldKey fittedKey{interner_.intern("fitted"), FieldNamespace::System};
    FieldKey bomKey{interner_.intern("bom"), FieldNamespace::System};
    FieldKey footprintKey{interner_.intern("footprint"), FieldNamespace::System};

    c.fitted = true;
    if (const FieldSlot* s = env.lookup(fittedKey); s && s->value) {
        c.fitted = s->value->kind == ValueKind::Boolean ? s->value->boolean : true;
    }
    if (inst->dnp) c.fitted = false;

    c.bom = true;
    if (const FieldSlot* s = env.lookup(bomKey); s && s->value) {
        c.bom = s->value->kind == ValueKind::Boolean ? s->value->boolean : true;
    }
    if (const FieldSlot* s = env.lookup(footprintKey); s && s->value) {
        c.footprint = renderValue(subst_.resolveValue(s->value, env, arena_), interner_);
    }

    // What the part is. Unstated means an ordinary part on the board, which is
    // what almost everything is; the structural roles are what a cable and the
    // mating checks are built on.
    FieldKey typeKey{interner_.intern("type"), FieldNamespace::System};
    c.type = "board_part";
    c.partType = PartType::BoardPart;
    if (const FieldSlot* s = env.lookup(typeKey); s && s->value) {
        c.type = renderValue(subst_.resolveValue(s->value, env, arena_), interner_);
        bool nearMiss = false;
        std::string_view suggestion;
        c.partType = lookupPartType(c.type, nearMiss, suggestion);
        if (nearMiss) {
            diags_.report(DiagId::PartTypeNearMiss, s->declaredAt, c.type, suggestion);
        }
    }

    // User fields travel to the BOM untouched (spec 9.1).
    FlatMap<FieldKey, FieldSlot> visible;
    env.collectVisible(visible);
    for (const auto& [key, slot] : visible) {
        if (key.ns != FieldNamespace::User || !slot.value) continue;
        const Value* resolved = subst_.resolveValue(slot.value, env, arena_);
        c.fields.emplace_back(std::string(interner_.text(key.name)),
                              renderValue(resolved, interner_));
    }

    // Give every pin its own node up front; connections merge them afterwards.
    for (std::uint32_t p = 0; p < c.pins.size(); ++p) {
        std::uint32_t node = freshNode(c.pins[p].span);
        c.pins[p].node = node;
        nodeInfo_[node].component = static_cast<std::int32_t>(index);
        nodeInfo_[node].pin = static_cast<std::int32_t>(p);
        // Spec 5.2: an unnamed node "takes the name of the first pin connected
        // to it, written DESIGNATOR.PIN".
        nodeInfo_[node].name =
            (c.designator.empty() ? c.identity : c.designator) + "." + c.pins[p].logical;
    }

    if (valid(inst->designator.prefix.symbol) &&
        inst->designator.kind == DesignatorKind::Numbered) {
        SymbolId designator = interner_.intern(c.designator);
        if (scope.harnessTypes.contains(designator)) {
            diags_.report(DiagId::E21, inst->span, c.designator);
        }
        scope.instances.insert(designator, index);
    }

    return index;
}

void Elaborator::applyDefaultNets(Component& component, const PartInfo& part, Scope& scope) {
    // Spec 11.6: "&NET names the net a pin joins when nothing binds it."
    for (const BodyEntry& entry : part.decl->body) {
        if (entry.kind != BodyKind::PinMap) continue;
        const PinMap* line = entry.pin;

        for (const Directive* d : line->directives) {
            if (!valid(d->name.symbol)) continue;
            if (interner_.text(d->name.symbol) != "NET") continue;
            if (!d->value) continue;

            // "&NET=?" unbinds: no net is created, so no &STUB is required.
            bool unbind = d->value->kind == ValueKind::Unbind;
            // &NET names a net, so the lexeme is what counts: "&~NET=3V3"
            // means the rail called 3V3, not the quantity 3.3 volts.
            SymbolId target = unbind ? SymbolId::kInvalid : d->value->text;

            SymbolId base = line->logical.symbol;
            for (std::uint32_t i = 0; i < component.pins.size(); ++i) {
                ComponentPin& pin = component.pins[i];
                if (pin.base != base) continue;
                if (pin.connected) continue;  // a binding at the call site wins
                if (unbind) {
                    pin.unbound = true;
                    continue;
                }
                if (!valid(target)) continue;
                unite(pin.node, netNode(scope, target, 0, false, d->span, false));
                pin.connected = true;
            }
        }
    }
}

void Elaborator::applyBindings(const Instance* inst, Component& component, Scope& scope,
                               std::vector<std::uint32_t>& touched) {
    for (const Binding* b : inst->bindings) {
        if (b->kind != BindingKind::PinNet) continue;

        // Spec 19 gives 'pin_ref = identifier [ "[" range "]" ] | "."', and
        // spec 20.7 writes "{C?~100nF-0603: .=GND}": a '.' binding takes the
        // next unassigned casual pin, exactly as a '.' terminal does. Bindings
        // are resolved before terminals (spec 7.3), so here the shunt's first
        // pin goes to GND and the outer '.' picks up the second.
        std::vector<std::uint32_t> targets;
        SymbolId pinName = SymbolId::kInvalid;

        if (b->pinIsDot) {
            std::uint32_t best = UINT32_MAX;
            std::int32_t bestOrder = INT32_MAX;
            for (std::uint32_t i = 0; i < component.pins.size(); ++i) {
                if (component.pins[i].connected || component.pins[i].unbound) continue;
                if (component.pins[i].declOrder < bestOrder) {
                    bestOrder = component.pins[i].declOrder;
                    best = i;
                }
            }
            if (best == UINT32_MAX) {
                diags_.report(DiagId::Type, b->span,
                              std::format("'.' has no unassigned pin left on '{}'",
                                          component.partName));
                continue;
            }
            // Spec 7.3: "'.' is legal only on pins carrying &CASUAL."
            if (!component.pins[best].casual) {
                diags_.report(DiagId::E23, b->span, component.pins[best].logical,
                              component.partName);
            }
            targets.push_back(best);
        } else {
            pinName = resolve(b->pin, scope);
            if (!valid(pinName)) continue;
        }

        // Collect the pins this binding refers to: a scalar, an explicit range,
        // or a whole array.
        if (!targets.empty()) {
            // already resolved by '.'
        } else if (b->pinRange.present) {
            bool ok = true;
            std::int64_t lo = subst_.resolveIndex(b->pinRange.lo, *scope.fields, ok);
            std::int64_t hi = subst_.resolveIndex(b->pinRange.hi, *scope.fields, ok);
            if (!ok) continue;
            std::int64_t step = lo <= hi ? 1 : -1;
            for (std::int64_t k = lo;; k += step) {
                bool found = false;
                for (std::uint32_t i = 0; i < component.pins.size(); ++i) {
                    if (component.pins[i].base == pinName &&
                        component.pins[i].isArrayElement && component.pins[i].index == k) {
                        targets.push_back(i);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    diags_.report(DiagId::E31, b->span,
                                  std::format("{}[{}]", interner_.text(pinName), k));
                }
                if (k == hi) break;
            }
        } else {
            for (std::uint32_t i = 0; i < component.pins.size(); ++i) {
                if (component.pins[i].base == pinName) targets.push_back(i);
            }
            if (targets.empty()) {
                diags_.report(DiagId::E31, b->pin.span, interner_.text(pinName));
                continue;
            }
        }

        // A '#' field written against a pin at a call site overrides what the
        // part declared for it: "{U1~mcu: IO[3] #VOH=3V0; }".
        for (const FieldDecl* f : b->pinFields) {
            for (std::uint32_t i : targets) {
                applyPinField(component.pins[i], f, interner_, diags_);
            }
        }

        for (const Directive* d : b->pinDirectives) {
            if (!valid(d->name.symbol)) continue;
            std::string_view dn = interner_.text(d->name.symbol);
            if (dn == "PINDELAY" && d->value && d->value->kind == ValueKind::Dimensioned) {
                for (std::uint32_t i : targets) {
                    component.pins[i].hasPinDelay = true;
                    component.pins[i].pinDelay = d->value->num;
                }
            } else if (dn == "NET" && d->value) {
                // Spec 11.6: "A binding is a pin-scoped &NET; 'GND=AGND' and
                // 'GND &NET=AGND' are one mechanism."
                if (d->value->kind == ValueKind::Unbind) {
                    for (std::uint32_t i : targets) component.pins[i].unbound = true;
                } else if (valid(d->value->text)) {
                    std::uint32_t node =
                        netNode(scope, d->value->text, 0, false, d->span);
                    for (std::uint32_t i : targets) {
                        unite(component.pins[i].node, node);
                        component.pins[i].connected = true;
                    }
                    touched.push_back(node);
                }
            }
        }

        if (b->unbind) {
            for (std::uint32_t i : targets) component.pins[i].unbound = true;
            continue;
        }
        if (!b->net) continue;

        std::vector<std::uint32_t> dummy;  // bindings are outside the directive scope
        ElemValue net = evalNet(b->net, scope, static_cast<std::int64_t>(targets.size()), dummy);

        if (net.entry.size() == 1 && targets.size() > 1) {
            // Spec 8.4: "connections to a scalar net broadcast to every copy."
            for (std::uint32_t i : targets) {
                unite(component.pins[i].node, net.entry[0]);
                component.pins[i].connected = true;
            }
        } else if (net.entry.size() == targets.size()) {
            for (std::size_t k = 0; k < targets.size(); ++k) {
                unite(component.pins[targets[k]].node, net.entry[k]);
                component.pins[targets[k]].connected = true;
            }
        } else {
            diags_.report(DiagId::E04, b->span, static_cast<std::int64_t>(targets.size()),
                          static_cast<std::int64_t>(net.entry.size()));
        }
    }
}

Elaborator::ElemValue Elaborator::evalDevice(const Device* dev, Scope& scope,
                                             std::vector<std::uint32_t>& touched) {
    ElemValue value;
    value.span = dev->span;
    const Instance* inst = dev->instance;
    if (!inst) return value;

    // Spec 7.2: without '~' this references an instance declared elsewhere.
    if (!inst->declares) {
        SymbolId designator = SymbolId::kInvalid;
        if (inst->designator.kind == DesignatorKind::Numbered) {
            designator = interner_.intern(
                std::string(interner_.text(inst->designator.prefix.symbol)) +
                std::to_string(inst->designator.number));
        }
        std::uint32_t* found = valid(designator) ? scope.instances.find(designator) : nullptr;
        if (!found) {
            // Spec 7.2: "Referencing a designator that is never declared is
            // error E-07."
            diags_.report(DiagId::E07, inst->span,
                          valid(designator) ? interner_.text(designator) : "?");
            return value;
        }

        Component& c = components_[*found];
        const PartInfo* part = partInfoFor(c.part, scope.objectIndex, dev->span);
        if (!part) return value;

        // A reference such as "{U1}GPIO[1]" selects one element, not the whole
        // array, so the range has to be honoured here just as it is on a
        // declaring instance.
        auto bindTerminal = [&](const Terminal& t, Bundle& out) {
            if (t.dot) return;
            SymbolId base = resolve(t.name, scope);
            if (t.range.present) {
                bool ok = true;
                std::int64_t lo = subst_.resolveIndex(t.range.lo, *scope.fields, ok);
                std::int64_t hi = subst_.resolveIndex(t.range.hi, *scope.fields, ok);
                if (!ok) return;
                std::int64_t step = lo <= hi ? 1 : -1;
                for (std::int64_t k = lo;; k += step) {
                    for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
                        if (c.pins[i].base == base && c.pins[i].isArrayElement &&
                            c.pins[i].index == k) {
                            out.push_back(c.pins[i].node);
                            c.pins[i].connected = true;
                            break;
                        }
                    }
                    if (k == hi) break;
                }
                return;
            }
            for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
                if (c.pins[i].base != base) continue;
                out.push_back(c.pins[i].node);
                c.pins[i].connected = true;
            }
        };
        if (dev->hasEntry) {
            bindTerminal(dev->entry, value.entry);
            value.hasEntry = true;
        }
        if (dev->hasExit) {
            bindTerminal(dev->exit, value.exit);
            value.hasExit = true;
        }
        value.component = static_cast<std::int32_t>(*found);
        return value;
    }

    SymbolId targetName = resolve(inst->partOrBlock, scope);
    if (!valid(targetName)) return value;

    const Declaration* decl = symbols_.find(targetName, scope.objectIndex);
    if (!decl) {
        // Spec 4.1: "A name referenced but never declared is error E-31."
        diags_.report(DiagId::E31, inst->partOrBlock.span, interner_.text(targetName));
        return value;
    }

    // ---- a block instance -------------------------------------------------
    if (decl->item->kind == ItemKind::Block) {
        std::unique_ptr<Scope> child = instantiateBlock(inst, decl->item, scope);
        if (!child) return value;

        // Spec 4.6: "A block's ports are its pins." A terminal names a port.
        auto bindPort = [&](const Terminal& t, Bundle& out) {
            if (t.dot) {
                diags_.report(DiagId::E23, t.span, ".", interner_.text(targetName));
                return;
            }
            SymbolId portName = resolve(t.name, scope);
            NetKey key{child->id, portName, 0, false};
            if (std::uint32_t* node = netNodes_.find(key)) {
                out.push_back(*node);
                return;
            }
            // Try an indexed port array.
            bool any = false;
            for (std::int64_t k = 0;; ++k) {
                NetKey ik{child->id, portName, k, true};
                std::uint32_t* n = netNodes_.find(ik);
                if (!n) break;
                out.push_back(*n);
                any = true;
            }
            if (!any) {
                diags_.report(DiagId::E31, t.span,
                              std::format("{}.{}", interner_.text(targetName),
                                          interner_.text(portName)));
            }
        };

        if (dev->hasEntry) {
            bindPort(dev->entry, value.entry);
            value.hasEntry = true;
        }
        if (dev->hasExit) {
            bindPort(dev->exit, value.exit);
            value.hasExit = true;
        }

        // Bindings on a block instance connect its ports by name.
        for (const Binding* b : inst->bindings) {
            if (b->kind != BindingKind::PinNet || !b->net) continue;
            SymbolId portName = resolve(b->pin, scope);
            NetKey key{child->id, portName, 0, false};
            std::uint32_t* node = netNodes_.find(key);
            if (!node) {
                diags_.report(DiagId::E31, b->pin.span, interner_.text(portName));
                continue;
            }
            std::vector<std::uint32_t> dummy;
            ElemValue outer = evalNet(b->net, scope, 1, dummy);
            if (!outer.entry.empty()) unite(*node, outer.entry[0]);
        }
        return value;
    }

    // ---- a part instance --------------------------------------------------
    if (decl->item->kind != ItemKind::Part) {
        diags_.report(DiagId::Type, inst->partOrBlock.span,
                      std::format("'{}' is not a part or a block", interner_.text(targetName)));
        return value;
    }

    const PartInfo* part = partInfoFor(targetName, scope.objectIndex, dev->span);
    if (!part) return value;

    std::uint32_t index = instantiatePart(inst, *part, scope, *scope.fields);
    Component& c = components_[index];
    value.component = static_cast<std::int32_t>(index);

    applyBindings(inst, c, scope, touched);

    // Spec 7.3, resolution order: bindings consume their named pins, then
    // explicitly named terminals consume theirs, then each '.' takes the next
    // unconsumed pin in part-declaration order.
    auto resolveTerminal = [&](const Terminal& t, Bundle& out) {
        if (!t.dot) {
            SymbolId base = resolve(t.name, scope);
            std::vector<std::uint32_t> matched;
            if (t.range.present) {
                bool ok = true;
                std::int64_t lo = subst_.resolveIndex(t.range.lo, *scope.fields, ok);
                std::int64_t hi = subst_.resolveIndex(t.range.hi, *scope.fields, ok);
                if (ok) {
                    std::int64_t step = lo <= hi ? 1 : -1;
                    for (std::int64_t k = lo;; k += step) {
                        for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
                            if (c.pins[i].base == base && c.pins[i].isArrayElement &&
                                c.pins[i].index == k) {
                                matched.push_back(i);
                                break;
                            }
                        }
                        if (k == hi) break;
                    }
                }
            } else {
                for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
                    if (c.pins[i].base == base) matched.push_back(i);
                }
            }
            if (matched.empty()) {
                // Spec 7.3: "A terminal name shall be a declared pin of the
                // instance's part."
                diags_.report(DiagId::E31, t.span,
                              std::format("{}.{}", c.partName, interner_.text(base)));
                return;
            }
            for (std::uint32_t i : matched) {
                out.push_back(c.pins[i].node);
                c.pins[i].connected = true;
            }
            return;
        }

        // The '.' terminal: the next unconsumed pin in declaration order.
        std::uint32_t best = UINT32_MAX;
        std::int32_t bestOrder = INT32_MAX;
        for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
            if (c.pins[i].connected || c.pins[i].unbound) continue;
            if (c.pins[i].declOrder < bestOrder) {
                bestOrder = c.pins[i].declOrder;
                best = i;
            }
        }
        if (best == UINT32_MAX) {
            diags_.report(DiagId::Type, t.span,
                          std::format("'.' has no unassigned pin left on '{}'", c.partName));
            return;
        }
        // Spec 7.3: "'.' is legal only on pins carrying &CASUAL. Otherwise it
        // is error E-23."
        if (!c.pins[best].casual) {
            diags_.report(DiagId::E23, t.span, c.pins[best].logical, c.partName);
        }
        out.push_back(c.pins[best].node);
        c.pins[best].connected = true;
    };

    if (dev->hasEntry) {
        resolveTerminal(dev->entry, value.entry);
        value.hasEntry = true;
    }
    if (dev->hasExit) {
        resolveTerminal(dev->exit, value.exit);
        value.hasExit = true;
    }

    applyDefaultNets(c, *part, scope);
    return value;
}

// ---------------------------------------------------------------------------
// Groups and replication
// ---------------------------------------------------------------------------

Elaborator::ElemValue Elaborator::evalGroup(const Group* g, Scope& scope,
                                            std::int64_t expectedIn,
                                            std::vector<std::uint32_t>& touched) {
    ElemValue value;
    value.span = g->span;
    if (!g->body) return value;

    if (g->mult == MultKind::None) {
        return elaborateSegment(g->body, scope, expectedIn, -1, touched);
    }

    std::int64_t copies = std::max<std::int64_t>(g->count, 1);

    // Each copy is elaborated afresh, so each declares its own instances.
    std::vector<ElemValue> made;
    made.reserve(static_cast<std::size_t>(copies));

    std::int64_t savedIndex = copyIndex_;
    std::int64_t savedCount = copyCount_;
    copyCount_ = copies;
    for (std::int64_t k = 0; k < copies; ++k) {
        copyIndex_ = k;
        made.push_back(elaborateSegment(g->body, scope, expectedIn, -1, touched));
    }
    copyIndex_ = savedIndex;
    copyCount_ = savedCount;

    switch (g->mult) {
        case MultKind::Series:
            // Spec 8.6: "N copies in series along the chain."
            for (std::size_t k = 0; k + 1 < made.size(); ++k) {
                uniteBundles(made[k].exit, made[k + 1].entry, g->span);
            }
            value.entry = made.front().entry;
            value.exit = made.back().exit;
            break;

        case MultKind::Parallel:
            // "N copies in parallel: entry terminals common, exit terminals
            // common." The chain passes through the group.
            for (std::size_t k = 1; k < made.size(); ++k) {
                uniteBundles(made[0].entry, made[k].entry, g->span);
                uniteBundles(made[0].exit, made[k].exit, g->span);
            }
            value.entry = made.front().entry;
            value.exit = made.front().exit;
            break;

        case MultKind::Node:
            // "N copies hanging off the current node; topology unasserted."
            // Each copy's remaining terminals are settled by its own bindings
            // and may differ per copy, so only the entries join.
            for (std::size_t k = 1; k < made.size(); ++k) {
                uniteBundles(made[0].entry, made[k].entry, g->span);
            }
            value.entry = made.front().entry;
            // The node is the same either side of a shunt group, which is what
            // lets a chain continue past it with '=='.
            value.exit = made.front().entry;
            break;

        case MultKind::None:
            break;
    }

    value.hasEntry = !value.entry.empty();
    value.hasExit = !value.exit.empty();
    return value;
}

Elaborator::ElemValue Elaborator::evalReplication(const Replication* r, Scope& scope,
                                                  std::int64_t expectedIn,
                                                  std::int64_t expectedOut,
                                                  std::vector<std::uint32_t>& touched) {
    ElemValue value;
    value.span = r->span;
    if (!r->body) return value;

    // The unit's arity comes from the body itself: a part's arity is "derived
    // from the pins used as chain terminals", a block's is its declared port
    // counts (spec 8.3). Probing it costs one throwaway elaboration, so the
    // widths are measured on a scratch pass whose nodes are simply not joined
    // to anything.
    std::vector<ElemWidth> bodyWidths;
    inferWidths(r->body, scope, bodyWidths);
    std::int64_t inArity = bodyWidths.empty() ? 1 : bodyWidths.front().in;
    std::int64_t outArity = bodyWidths.empty() ? 1 : bodyWidths.back().out;
    if (inArity <= 0) inArity = 1;
    if (outArity <= 0) outArity = 1;

    std::int64_t copies = 1;
    if (r->counted) {
        // "[N[ chain ]M]": the bracketed widths are repeated on the closing
        // delimiter so a mismatched pair is caught by eye.
        if (r->inWidth % inArity != 0) {
            diags_.report(DiagId::E05, r->span, r->inWidth, inArity);
            return value;
        }
        copies = r->inWidth / inArity;
        if (outArity != 0 && r->outWidth % outArity == 0) {
            std::int64_t outCopies = r->outWidth / outArity;
            if (outCopies != copies) {
                // Spec 8.3: "where the two quotients disagree, error E-06."
                diags_.report(DiagId::E06, r->span, r->inWidth, r->outWidth, copies, outCopies);
                return value;
            }
        } else {
            diags_.report(DiagId::E06, r->span, r->inWidth, r->outWidth, copies,
                          outArity == 0 ? 0 : r->outWidth / outArity);
            return value;
        }
    } else {
        std::int64_t incoming = expectedIn > 0 ? expectedIn : expectedOut;
        if (incoming <= 0) incoming = inArity;
        if (incoming % inArity != 0) {
            // Spec 8.3: "Where the widths do not divide, that is error E-05."
            diags_.report(DiagId::E05, r->span, incoming, inArity);
            return value;
        }
        copies = incoming / inArity;
    }

    std::int64_t savedIndex = copyIndex_;
    std::int64_t savedCount = copyCount_;
    copyCount_ = copies;

    for (std::int64_t k = 0; k < copies; ++k) {
        copyIndex_ = k;
        ElemValue copy = elaborateSegment(r->body, scope, inArity, outArity, touched);
        for (std::uint32_t n : copy.entry) value.entry.push_back(n);
        for (std::uint32_t n : copy.exit) value.exit.push_back(n);
    }

    copyIndex_ = savedIndex;
    copyCount_ = savedCount;

    value.hasEntry = !value.entry.empty();
    value.hasExit = !value.exit.empty();
    return value;
}

Elaborator::ElemValue Elaborator::evalElement(const Element* el, Scope& scope,
                                              std::int64_t expectedIn, std::int64_t expectedOut,
                                              std::vector<std::uint32_t>& touched) {
    switch (el->kind) {
        case ElementKind::Net: return evalNet(el->net, scope, expectedIn, touched);
        case ElementKind::Device: return evalDevice(el->device, scope, touched);
        case ElementKind::Group: return evalGroup(el->group, scope, expectedIn, touched);
        case ElementKind::Replication:
            return evalReplication(el->replication, scope, expectedIn, expectedOut, touched);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Segments, chains and statements
// ---------------------------------------------------------------------------

Elaborator::ElemValue Elaborator::elaborateSegment(const Segment* seg, Scope& scope,
                                                   std::int64_t expectedIn,
                                                   std::int64_t expectedOut,
                                                   std::vector<std::uint32_t>& touched) {
    ElemValue result;
    if (!seg || seg->elements.empty()) return result;
    result.span = seg->span;

    std::vector<ElemWidth> widths;
    inferWidths(seg, scope, widths);
    if (expectedIn > 0 && !widths.empty() && !widths.front().fixed) widths.front().in = expectedIn;
    if (expectedOut > 0 && !widths.empty() && !widths.back().fixed) widths.back().out = expectedOut;

    std::vector<ElemValue> values;
    values.reserve(seg->elements.size());
    for (std::size_t i = 0; i < seg->elements.size(); ++i) {
        values.push_back(
            evalElement(seg->elements[i], scope, widths[i].in, widths[i].out, touched));
    }

    // Apply the connectors of spec 6.
    for (std::size_t i = 0; i + 1 < seg->elements.size(); ++i) {
        Connector c = seg->connectors[i];
        const Bundle& lhs = values[i].exit;
        const Bundle& rhs = values[i + 1].entry;
        Span at = values[i].span.merge(values[i + 1].span);

        switch (c) {
            case Connector::Advance:
                // Spec 6.2: "'=' shall have a device, group or replication on at
                // least one side." A dotted reference names a device terminal
                // (spec 5.2) and so is not bare; a harness identifier stands for
                // its members and is not bare either (spec 12.1).
                if (values[i].isBareNet && values[i + 1].isBareNet) {
                    const Element* le = seg->elements[i];
                    const Element* re = seg->elements[i + 1];
                    auto plainNet = [&](const Element* e) {
                        if (e->kind != ElementKind::Net) return false;
                        if (e->net->path.size() != 1 || e->net->hasMemberList) return false;
                        SymbolId n = resolve(e->net->path[0], scope);
                        return !scope.harnessTypes.contains(n);
                    };
                    if (plainNet(le) && plainNet(re)) diags_.report(DiagId::E22, at);
                }
                uniteBundles(lhs, rhs, at);
                break;

            case Connector::Same:
                uniteBundles(lhs, rhs, at);
                // Spec 6.3: "Every element in a run of consecutive '==' lies on
                // one net", so an element with '==' on both sides has its own
                // two terminals joined -- which is what shorts a two-terminal
                // device and raises W-02.
                if (i + 1 < seg->connectors.size() && seg->connectors[i + 1] == Connector::Same) {
                    uniteBundles(values[i + 1].entry, values[i + 1].exit, at);
                    // A two-terminal device with '==' on both sides has its
                    // pads bridged. Spec 6.3 says this "is legal and generates
                    // warning W-02"; only elaboration can see the run, so the
                    // component is recorded here and reported by ERC.
                    std::int32_t shortedIndex = values[i + 1].component;
                    if (shortedIndex >= 0 &&
                        components_[static_cast<std::size_t>(shortedIndex)].pins.size() == 2 &&
                        !values[i + 1].entry.empty() && !values[i + 1].exit.empty()) {
                        shorted_.push_back(static_cast<std::uint32_t>(shortedIndex));
                    }
                }
                break;

            case Connector::Gather:
                // Spec 6.5: "an array on the left is shorted to one net on the
                // right."
                if (!rhs.empty()) {
                    for (std::uint32_t n : lhs) unite(n, rhs[0]);
                }
                break;

            case Connector::Broadcast:
                // "one net on the left connects to every element of the array
                // on the right."
                if (!lhs.empty()) {
                    for (std::uint32_t n : rhs) unite(lhs[0], n);
                }
                break;
        }
    }

    result.entry = values.front().entry;
    result.exit = values.back().exit;
    // A shunt has no exit terminal, so the node is the same either side of it
    // (spec 6.3), which is what lets a chain continue past one.
    if (result.exit.empty()) result.exit = values.back().entry;
    result.hasEntry = !result.entry.empty();
    result.hasExit = !result.exit.empty();
    return result;
}

void Elaborator::elaborateChain(const Chain* chain, const Stmt* stmt, Scope& scope) {
    std::vector<std::uint32_t> touched;
    // Spec 6.4: '^' partitions the statement into independent segments that
    // still share one directive scope (spec 11.2).
    for (const Segment* seg : chain->segments) elaborateSegment(seg, scope, -1, -1, touched);
    if (stmt) applyStatementDirectives(stmt, touched, scope);
}

void Elaborator::elaborateStatement(const Stmt* stmt, Scope& scope) {
    switch (stmt->kind) {
        case StmtKind::Field:
            scope.fields->declare(stmt->field, interner_, diags_);
            return;

        case StmtKind::PortList: {
            std::vector<std::uint32_t> touched;
            for (const NetExpr* n : stmt->ports) {
                ElemValue v = evalNet(n, scope, 1, touched);
                for (std::uint32_t nd : v.entry) {
                    NodeInfo& info = nodeInfo_[uf_.find(nd)];
                    info.global = info.global || stmt->listArrow.global;
                    if (info.direction == PortDir::None) info.direction = stmt->listArrow.dir;
                }
            }
            applyStatementDirectives(stmt, touched, scope);
            return;
        }

        case StmtKind::Chain:
            elaborateChain(stmt->chain, stmt, scope);
            return;
    }
}

void Elaborator::applyStatementDirectives(const Stmt* stmt, std::span<const std::uint32_t> nodes,
                                          Scope& scope) {
    for (const Directive* d : stmt->directives) {
        SymbolId name = resolve(d->name, scope);
        if (!valid(name)) continue;
        std::string_view text = interner_.text(name);

        // Spec 12.1: a harness type is assigned with &HARNESS.
        if (text == "HARNESS" && d->value && d->value->kind == ValueKind::Identifier) {
            if (!stmt->chain || stmt->chain->segments.empty()) continue;
            const Segment* seg = stmt->chain->segments[0];
            if (seg->elements.empty() || seg->elements[0]->kind != ElementKind::Net) continue;
            SymbolId ident = interner_.intern(netDisplayName(seg->elements[0]->net, scope));
            // Spec 5.2: "A harness name shall not collide with a designator, or
            // member access is ambiguous. That is error E-21." Both live in the
            // scope, so the clash is visible right here.
            if (scope.instances.contains(ident)) {
                diags_.report(DiagId::E21, stmt->span, interner_.text(ident));
            }
            scope.harnessTypes.set(ident, d->value->text);
            // Spec 12.4: a 'diff' harness carries two members, '+' and '-'. An
            // '&IMP' applied to one shall carry the 'D' suffix.
            if (interner_.text(d->value->text) == "diff") diffHarnesses_.insert(ident);
            for (std::uint32_t n : nodes) {
                pendingDirectives_.push_back(PendingDirective{
                    n, "HARNESS", std::string(interner_.text(d->value->text)), d->strength,
                    d->span, false});
            }
            continue;
        }

        const Value* resolved = subst_.resolveValue(d->value, *scope.fields, arena_);
        std::string value = resolved ? renderValue(resolved, interner_) : std::string{};

        // Spec 12.4: "'&IMP' applied to a 'diff' shall carry the 'D' suffix; a
        // single-ended impedance on a differential pair is error E-14."
        if (text == "IMP" && resolved && resolved->kind == ValueKind::Dimensioned &&
            !resolved->num.differential && stmt->chain) {
            for (const Segment* seg : stmt->chain->segments) {
                for (const Element* el : seg->elements) {
                    if (el->kind != ElementKind::Net) continue;
                    const NetExpr* n = el->net;
                    // The pair shows either as the harness itself or as one of
                    // its members, "USB.+" and "USB.-".
                    SymbolId head = n->path.empty() ? SymbolId::kInvalid
                                                    : resolve(n->path[0], scope);
                    if (valid(head) && diffHarnesses_.contains(head)) {
                        diags_.report(DiagId::E14, d->span, value, interner_.text(head));
                        break;
                    }
                }
            }
        }

        for (std::uint32_t n : nodes) {
            pendingDirectives_.push_back(
                PendingDirective{n, std::string(text), value, d->strength, d->span, false});
        }

        if (text == "MATCH") {
            SymbolId group = SymbolId::kInvalid;
            std::string offset, tolerance;
            if (d->matchRef) {
                group = d->matchRef->group.symbol;
                for (const FieldDecl* f : d->matchRef->overrides) {
                    std::string_view fname = interner_.text(f->name.symbol);
                    std::string rendered = renderValue(f->value, interner_);
                    if (fname == "offset") offset = rendered;
                    if (fname == "tolerance") tolerance = rendered;
                }
            } else if (resolved && resolved->kind == ValueKind::Identifier) {
                group = resolved->text;
            }
            if (valid(group)) {
                for (std::uint32_t n : nodes) {
                    pendingMatches_.push_back(
                        PendingMatchUse{n, group, offset, tolerance, d->span});
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

std::unique_ptr<Elaborator::Scope> Elaborator::instantiateBlock(const Instance* inst,
                                                                const Item* block,
                                                                Scope& parent) {
    if (depth_ >= kMaxInstantiationDepth) {
        diags_.report(DiagId::Internal, inst->span,
                      std::string("block instantiation nested more than 64 deep; "
                                  "a block probably instantiates itself"));
        return nullptr;
    }

    auto child = std::make_unique<Scope>();
    child->id = nextScopeId_++;
    child->parent = &parent;
    child->objectIndex = parent.objectIndex;
    child->path = parent.path;
    child->fields = std::make_unique<FieldEnv>(parent.fields.get());

    std::string prefix = valid(inst->designator.prefix.symbol)
                             ? std::string(interner_.text(inst->designator.prefix.symbol))
                             : std::string{};

    // A block instance names a level of the hierarchy, so this label ends up in
    // the path of every component beneath it and, through those, in the netlist
    // and the BOM. It has to follow the same rules as a device designator.
    std::string label;
    switch (inst->designator.kind) {
        case DesignatorKind::Numbered:
            label = prefix + std::to_string(inst->designator.number);
            break;
        case DesignatorKind::Range: {
            // Spec 13.3: one token carrying N designators, taken in order by the
            // copies. This is the *annotated* form -- 'BLK%[1:2]' is what the
            // annotator writes for 'BLK?' under a x2 replication -- so reading
            // it as unassigned would report an annotated design as un-annotated.
            std::vector<std::int64_t> numbers;
            for (const DesigPart& range : inst->designator.parts) {
                for (std::int64_t n = range.lo; n <= range.hi; ++n) numbers.push_back(n);
            }
            auto which = static_cast<std::size_t>(copyIndex_ < 0 ? 0 : copyIndex_);
            if (which < numbers.size()) {
                label = prefix + std::to_string(numbers[which]);
            } else {
                diags_.report(DiagId::Type, inst->designator.span,
                              std::format("designator range carries {} designator{} but this "
                                          "statement instantiates at least {}",
                                          numbers.size(), numbers.size() == 1 ? "" : "s",
                                          which + 1));
                label = prefix + "?" + std::to_string(child->id);
            }
            break;
        }
        case DesignatorKind::Unassigned:
            // Spec 13.1: an un-annotated design still elaborates, carrying an
            // internal identity, so that 'manta annotate' has a netlist to read.
            // Recorded and reported as E-UNANNOTATED once the netlist is built,
            // alongside the unassigned devices.
            label = prefix + "?" + std::to_string(child->id);
            unannotatedBlocks_.push_back(UnannotatedBlock{
                (parent.path.empty() ? "" : flattenPath(parent.path) + ".") + label,
                inst->designator.span});
            break;
    }
    child->path.push_back(label);

    // Spec 14.1: "A block instantiated twice with different parameters produces
    // two elaborations from one source." The overrides are applied to the child
    // environment before the body is walked, so every substitution inside sees
    // them.
    for (const Binding* b : inst->bindings) {
        if (b->kind == BindingKind::Field) child->fields->declare(b->field, interner_, diags_);
    }
    if (inst->dnp) {
        // Spec 7.5: "!BLK?~audio-stage cascades recursively to every part within."
        auto* falseValue = arena_.make<Value>();
        falseValue->kind = ValueKind::Boolean;
        falseValue->boolean = false;
        falseValue->upperCaseSpelling = true;
        child->fields->set(FieldKey{interner_.intern("fitted"), FieldNamespace::System},
                           falseValue, Strength::Locked, inst->span);
    }

    ++depth_;
    elaborateBlock(block, *child);
    --depth_;

    return child;
}

void Elaborator::elaborateBlock(const Item* block, Scope& scope) {
    // Fields first, so that a substitution anywhere in the body sees every
    // field the block declares regardless of where it was written.
    for (const BodyEntry& e : block->body) {
        if (e.kind == BodyKind::Field) scope.fields->declare(e.field, interner_, diags_);
        if (e.kind == BodyKind::Stmt && e.stmt->kind == StmtKind::Field) {
            scope.fields->declare(e.stmt->field, interner_, diags_);
        }
    }

    // Spec 14.1: "Every substitution error is reported at link, alongside ERC."
    // A field is evaluated where it is declared, so a bad expression is
    // diagnosed even when nothing goes on to read the field.
    for (const BodyEntry& e : block->body) {
        const FieldDecl* f = nullptr;
        if (e.kind == BodyKind::Field) f = e.field;
        if (e.kind == BodyKind::Stmt && e.stmt->kind == StmtKind::Field) f = e.stmt->field;
        if (f && f->value && f->value->kind == ValueKind::Interp) {
            (void)subst_.resolveValue(f->value, *scope.fields, arena_);
        }
    }

    // Spec 13.4: the flattening template, inherited by nested scopes.
    FieldKey flat{interner_.intern("FLATFORMAT"), FieldNamespace::System};
    if (const FieldSlot* s = scope.fields->lookup(flat); s && s->value) {
        scope.flatFormat = renderValue(s->value, interner_);
    }

    for (const BodyEntry& e : block->body) {
        switch (e.kind) {
            case BodyKind::Stmt:
                if (e.stmt->kind != StmtKind::Field) elaborateStatement(e.stmt, scope);
                break;
            case BodyKind::Item:
                if (e.item->kind == ItemKind::Netclass) {
                    std::vector<const Directive*> ds;
                    for (const BodyEntry& d : e.item->body) {
                        if (d.kind == BodyKind::Directive) ds.push_back(d.directive);
                    }
                    netclasses_.set(e.item->name.symbol, std::move(ds));
                }
                break;
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Finishing
// ---------------------------------------------------------------------------

void Elaborator::collectNetclasses() {
    for (const auto& [key, decl] : symbols_.all()) {
        if (decl.item->kind != ItemKind::Netclass) continue;
        std::vector<const Directive*> ds;
        for (const BodyEntry& e : decl.item->body) {
            if (e.kind == BodyKind::Directive) ds.push_back(e.directive);
        }
        netclasses_.set(decl.item->name.symbol, std::move(ds));
    }
}

void Elaborator::collectMatchGroups(Design& design) {
    for (const auto& [key, decl] : symbols_.all()) {
        if (decl.item->kind != ItemKind::Match) continue;

        MatchGroup group;
        group.name = std::string(interner_.text(decl.item->name.symbol));
        group.span = decl.item->span;

        for (const BodyEntry& e : decl.item->body) {
            if (e.kind == BodyKind::Item && e.item->kind == ItemKind::Match) {
                group.nested.push_back(std::string(interner_.text(e.item->name.symbol)));
                continue;
            }
            if (e.kind != BodyKind::Field || !e.field->value) continue;
            std::string_view name = interner_.text(e.field->name.symbol);
            const Value* v = e.field->value;

            if (name == "src") {
                group.src = renderValue(v, interner_);
            } else if (name == "dest") {
                if (v->kind == ValueKind::List) {
                    for (const Value* item : v->list) {
                        group.dest.push_back(renderValue(item, interner_));
                    }
                } else {
                    group.dest.push_back(renderValue(v, interner_));
                }
            } else if (name == "tolerance") {
                group.tolerance = renderValue(v, interner_);
            } else if (name == "offset") {
                group.offset = renderValue(v, interner_);
            }
        }
        design.matches.push_back(std::move(group));
    }

    // Attach the uses collected during elaboration.
    for (const PendingMatchUse& use : pendingMatches_) {
        std::uint32_t root = uf_.find(use.node);
        std::string_view groupName = interner_.text(use.group);
        for (MatchGroup& g : design.matches) {
            if (g.name != groupName) continue;
            MatchMember member;
            member.net = nodeInfo_[root].name;
            member.at = use.at;
            member.offset = use.offset;
            member.tolerance = use.tolerance;
            g.members.push_back(std::move(member));
            break;
        }
    }
}

void Elaborator::buildNets(Design& design) {
    // Group nodes by union-find root, in the order the roots were first
    // created, so the emitted net order follows source order (spec 15.8).
    FlatMap<std::uint32_t, std::uint32_t> rootToNet;

    // A pin left deliberately floating by "&NET=?" joins no net at all: spec
    // 11.6 says "No net is created, so no &STUB is required". Its node would
    // otherwise surface as an empty net and trip E-26.
    std::vector<bool> unboundOnly(nodeInfo_.size(), true);
    for (const Component& c : components_) {
        for (const ComponentPin& p : c.pins) {
            if (!p.unbound) unboundOnly[uf_.find(p.node)] = false;
        }
    }
    for (std::uint32_t node = 0; node < nodeInfo_.size(); ++node) {
        if (nodeInfo_[node].component < 0) unboundOnly[uf_.find(node)] = false;
    }

    for (std::uint32_t node = 0; node < nodeInfo_.size(); ++node) {
        std::uint32_t root = uf_.find(node);
        if (unboundOnly[root]) continue;
        if (rootToNet.contains(root)) continue;

        auto index = static_cast<std::uint32_t>(design.nets.size());
        rootToNet.insert(root, index);
        design.nets.emplace_back();
        Net& net = design.nets.back();
        net.firstSeen = nodeInfo_[root].firstSeen;
    }

    // Name each net. Spec 5.2: "Where a net is also named explicitly the two
    // are aliases, and the explicit name is used for display, netlist output
    // and BOM."
    for (std::uint32_t node = 0; node < nodeInfo_.size(); ++node) {
        std::uint32_t root = uf_.find(node);
        std::uint32_t* slot = rootToNet.find(root);
        if (!slot) continue;
        Net& net = design.nets[*slot];
        const NodeInfo& info = nodeInfo_[node];

        if (info.explicitName && net.name.empty()) net.name = info.name;
        if (!info.explicitName && net.name.empty()) net.name = info.name;
        net.references += info.references;
        net.global = net.global || info.global;
        if (net.direction == PortDir::None) net.direction = info.direction;
        if (!net.firstSeen.valid()) net.firstSeen = info.firstSeen;
    }
    // A second pass so an explicit name always beats a pin-derived one.
    for (std::uint32_t node = 0; node < nodeInfo_.size(); ++node) {
        const NodeInfo& info = nodeInfo_[node];
        if (!info.explicitName) continue;
        std::uint32_t* slot = rootToNet.find(uf_.find(node));
        if (!slot) continue;
        design.nets[*slot].name = info.name;
    }

    // Attach pins.
    for (std::uint32_t ci = 0; ci < components_.size(); ++ci) {
        Component& c = components_[ci];
        for (std::uint32_t pi = 0; pi < c.pins.size(); ++pi) {
            if (c.pins[pi].unbound) continue;
            std::uint32_t root = uf_.find(c.pins[pi].node);
            std::uint32_t* slot = rootToNet.find(root);
            if (!slot) continue;
            auto netIndex = *slot;
            c.pins[pi].net = static_cast<std::int32_t>(netIndex);
            design.nets[netIndex].pins.push_back(PinRef{ci, pi});
        }
    }

    // Directives, applying the strength ladder per key (spec 11.1: "Directives
    // accumulate per key ... only same-key collisions at equal strength are
    // conflicts").
    auto applyDirective = [&](Net& net, const std::string& name, const std::string& value,
                              Strength strength, Span at, bool fromClass) {
        NetDirective incoming{value, strength, at, fromClass};
        auto [slot, inserted] = net.directives.insert(name, incoming);
        if (inserted) return;
        if (slot->strength > strength) return;
        if (slot->strength < strength) {
            *slot = incoming;
            return;
        }
        // Equal strength. A class directive loses to a per-net one, which is
        // spec 11.9: "Per-net directives override class directives at equal
        // strength."
        if (slot->fromClass && !fromClass) {
            *slot = incoming;
            return;
        }
        if (!slot->fromClass && fromClass) return;
        if (slot->value != value) {
            diags_.report(DiagId::E12, at, name, slot->value, value)
                .note(slot->declaredAt, "first applied here");
        }
    };

    for (const PendingDirective& d : pendingDirectives_) {
        std::uint32_t* slot = rootToNet.find(uf_.find(d.node));
        if (!slot) continue;
        Net& net = design.nets[*slot];
        applyDirective(net, d.name, d.value, d.strength, d.at, d.fromClass);

        if (d.name == "TYPE" && d.value == "GROUND") net.ground = true;
        if (d.name == "STUB") net.stub = true;
        if (d.name == "HARNESS") net.harness = true;
    }

    // Spec 11.9: a net class carries directives for many nets at once.
    for (Net& net : design.nets) {
        const NetDirective* cls = net.directives.find("CLASS");
        if (!cls) continue;
        SymbolId className = interner_.intern(cls->value);
        auto* directives = netclasses_.find(className);
        if (!directives) {
            diags_.report(DiagId::E31, cls->declaredAt, cls->value);
            continue;
        }
        for (const Directive* d : *directives) {
            if (!valid(d->name.symbol)) continue;
            applyDirective(net, std::string(interner_.text(d->name.symbol)),
                           renderValue(d->value, interner_), d->strength, d->span, true);
        }
    }

    design.components = std::move(components_);
    design.shorted = shorted_;
    design.unannotatedBlocks = std::move(unannotatedBlocks_);
}

Design Elaborator::run(SymbolId topName, Span at) {
    Design design;
    design.top = std::string(interner_.text(topName));

    const Declaration* decl = symbols_.find(topName, 0);
    if (!decl) {
        diags_.report(DiagId::E31, at, interner_.text(topName));
        return design;
    }
    if (decl->item->kind != ItemKind::Block) {
        diags_.report(DiagId::Type, at,
                      std::format("'{}' is not a block", interner_.text(topName)));
        return design;
    }

    collectNetclasses();

    Scope root;
    root.id = nextScopeId_++;
    root.objectIndex = decl->objectIndex;
    root.fields = std::make_unique<FieldEnv>();

    elaborateBlock(decl->item, root);

    buildNets(design);
    collectMatchGroups(design);

    for (const auto& [key, w] : weakFields_) {
        if (overriddenFields_.contains(FieldKey{w.name, w.ns})) continue;
        diags_.report(DiagId::W06, w.declaredAt,
                      std::format("{}{}", w.ns == FieldNamespace::System ? "@" : "#",
                                  interner_.text(w.name)),
                      interner_.text(w.owner));
    }

    for (const Component& c : design.components) {
        design.elaborationMap.emplace_back(flattenPath(c.path),
                                           c.designator.empty() ? c.identity : c.designator);
    }

    return design;
}

}  // namespace manta
