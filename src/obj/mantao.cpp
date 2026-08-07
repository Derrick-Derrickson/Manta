#include "obj/mantao.h"

#include <format>
#include <vector>

namespace manta {

// Compares two "major.minor" revision strings. A malformed string is treated as
// newer than anything, so it is rejected rather than silently accepted.
bool revisionAtMost(std::string_view object, std::string_view toolchain) {
    auto split = [](std::string_view s, std::uint32_t& major, std::uint32_t& minor) {
        std::size_t dot = s.find('.');
        if (dot == std::string_view::npos) return false;
        major = minor = 0;
        for (char c : s.substr(0, dot)) {
            if (c < '0' || c > '9') return false;
            major = major * 10 + static_cast<std::uint32_t>(c - '0');
        }
        for (char c : s.substr(dot + 1)) {
            if (c < '0' || c > '9') return false;
            minor = minor * 10 + static_cast<std::uint32_t>(c - '0');
        }
        return true;
    };

    std::uint32_t oMajor = 0, oMinor = 0, tMajor = 0, tMinor = 0;
    if (!split(object, oMajor, oMinor)) return false;
    if (!split(toolchain, tMajor, tMinor)) return false;
    return oMajor != tMajor ? oMajor < tMajor : oMinor <= tMinor;
}

// ===========================================================================
// Writing
// ===========================================================================

namespace {

class ObjectWriter {
public:
    ObjectWriter(JsonWriter& w, const StringInterner& interner) : w_(w), in_(interner) {}

    void writeUnit(const SourceUnit& unit, std::string_view sourcePath) {
        w_.beginObject();
        w_.field("version", kLanguageVersion);
        w_.field("kind", "mantaO");
        w_.field("source", sourcePath);
        w_.key("declarations");
        w_.beginArray();
        for (const Item* item : unit.items) writeItem(item);
        w_.endArray();
        w_.endObject();
    }

private:
    // Spans are two integers rather than an object: they appear on every node,
    // and the array form keeps objects readable.
    void span(Span s) {
        w_.key("at");
        w_.beginArray();
        w_.value(static_cast<std::int64_t>(s.offset));
        w_.value(static_cast<std::int64_t>(s.length));
        w_.endArray();
    }

    void name(std::string_view key, const Name& n) {
        w_.key(key);
        if (n.isInterpolated()) {
            w_.beginObject();
            w_.key("interp");
            writeInterp(n.interp);
            span(n.span);
            w_.endObject();
        } else if (valid(n.symbol)) {
            w_.value(in_.text(n.symbol));
        } else {
            w_.null();
        }
    }

    void writeInterp(const InterpText* t) {
        w_.beginArray();
        for (const InterpChunk& c : t->chunks) {
            w_.beginObject();
            if (c.isExpr) {
                w_.key("expr");
                writeExpr(c.expr);
            } else {
                w_.field("text", in_.text(c.literal));
            }
            span(c.span);
            w_.endObject();
        }
        w_.endArray();
    }

    void writeExpr(const Expr* e) {
        if (!e) {
            w_.null();
            return;
        }
        w_.beginObject();
        switch (e->kind) {
            case ExprKind::IntLit:
                w_.field("int", e->intVal);
                break;
            case ExprKind::BoolLit:
                w_.field("bool", e->boolVal);
                break;
            case ExprKind::FieldRef:
                w_.field("field", in_.text(e->field));
                break;
            case ExprKind::Unary:
                w_.field("unary", e->unOp == UnOp::Neg ? "-" : "!");
                w_.key("operand");
                writeExpr(e->lhs);
                break;
            case ExprKind::Binary:
                w_.field("op", binOpText(e->binOp));
                w_.key("lhs");
                writeExpr(e->lhs);
                w_.key("rhs");
                writeExpr(e->rhs);
                break;
        }
        span(e->span);
        w_.endObject();
    }

    void writeValue(const Value* v) {
        if (!v) {
            w_.null();
            return;
        }
        w_.beginObject();
        w_.field("v", valueKindName(v->kind));
        switch (v->kind) {
            case ValueKind::Integer:
            case ValueKind::Decimal:
            case ValueKind::Dimensioned:
            case ValueKind::Percentage:
            case ValueKind::Tolerance:
                // The canonical text is the value: exact, and already the
                // SI-substituted form the formatter emits (spec 3.2, 17).
                w_.field("num", v->num.canonical());
                // The lexeme as written, because a net-name context reads the
                // same word differently (spec 11.6).
                if (valid(v->text)) w_.field("text", in_.text(v->text));
                break;
            case ValueKind::String:
            case ValueKind::Identifier:
                w_.field("text", in_.text(v->text));
                break;
            case ValueKind::Boolean:
                w_.field("bool", v->boolean);
                w_.field("upper", v->upperCaseSpelling);
                break;
            case ValueKind::List:
                w_.key("items");
                w_.beginArray();
                for (const Value* item : v->list) writeValue(item);
                w_.endArray();
                break;
            case ValueKind::Repeat:
                w_.key("inner");
                writeValue(v->inner);
                w_.field("count", v->count);
                break;
            case ValueKind::Interp:
                w_.key("interp");
                writeInterp(v->interp);
                break;
            case ValueKind::Version:
                w_.field("text", in_.text(v->text));
                w_.key("range");
                w_.beginArray();
                w_.value(static_cast<std::int64_t>(v->version.hasLo ? 1 : 0));
                w_.value(static_cast<std::int64_t>(v->version.loMajor));
                w_.value(static_cast<std::int64_t>(v->version.loMinor));
                w_.value(static_cast<std::int64_t>(v->version.hasHi ? 1 : 0));
                w_.value(static_cast<std::int64_t>(v->version.hiMajor));
                w_.value(static_cast<std::int64_t>(v->version.hiMinor));
                w_.endArray();
                break;
            case ValueKind::Unbind:
                break;
        }
        span(v->span);
        w_.endObject();
    }

    void writeIndex(const Index& i) {
        w_.beginObject();
        if (i.isExpr) {
            w_.key("expr");
            writeExpr(i.expr);
        } else {
            w_.field("n", i.literal);
        }
        span(i.span);
        w_.endObject();
    }

    void writeRange(std::string_view key, const Range& r) {
        if (!r.present) return;
        w_.key(key);
        w_.beginObject();
        w_.field("single", r.single);
        w_.key("lo");
        writeIndex(r.lo);
        w_.key("hi");
        writeIndex(r.hi);
        span(r.span);
        w_.endObject();
    }

    void writePort(std::string_view key, const PortSpec& p) {
        if (!p.present()) return;
        w_.key(key);
        w_.beginObject();
        w_.field("dir", portDirName(p.dir));
        w_.field("global", p.global);
        w_.field("leading", p.leading);
        span(p.span);
        w_.endObject();
    }

    void writeNames(std::string_view key, std::span<const Name> names) {
        if (names.empty()) return;
        w_.key(key);
        w_.beginArray();
        for (const Name& n : names) {
            w_.beginObject();
            name("n", n);
            w_.endObject();
        }
        w_.endArray();
    }

    void writeNet(const NetExpr* n) {
        if (!n) {
            w_.null();
            return;
        }
        w_.beginObject();
        writePort("lead", n->leading);
        writePort("trail", n->trailing);
        if (n->perCopy) w_.field("perCopy", true);
        writeNames("path", n->path);
        if (n->hasMemberList) writeNames("members", n->memberList);
        writeRange("range", n->range);
        if (n->hasPerCopyList) {
            w_.key("perCopyList");
            w_.beginArray();
            for (const Value* v : n->perCopyList) writeValue(v);
            w_.endArray();
        }
        span(n->span);
        w_.endObject();
    }

    void writeDirective(const Directive* d) {
        w_.beginObject();
        w_.field("strength", strengthName(d->strength));
        name("name", d->name);
        if (d->value) {
            w_.key("value");
            writeValue(d->value);
        }
        if (d->matchRef) {
            w_.key("match");
            w_.beginObject();
            name("group", d->matchRef->group);
            w_.key("overrides");
            w_.beginArray();
            for (const FieldDecl* f : d->matchRef->overrides) writeField(f);
            w_.endArray();
            span(d->matchRef->span);
            w_.endObject();
        }
        span(d->span);
        w_.endObject();
    }

    void writeDirectives(std::string_view key, std::span<Directive* const> ds) {
        if (ds.empty()) return;
        w_.key(key);
        w_.beginArray();
        for (const Directive* d : ds) writeDirective(d);
        w_.endArray();
    }

    void writeFields(std::string_view key, std::span<FieldDecl* const> fs) {
        if (fs.empty()) return;
        w_.key(key);
        w_.beginArray();
        for (const FieldDecl* f : fs) writeField(f);
        w_.endArray();
    }

    void writeField(const FieldDecl* f) {
        w_.beginObject();
        w_.field("ns", f->ns == FieldNamespace::System ? "@" : "#");
        w_.field("strength", strengthName(f->strength));
        w_.field("dir", fieldDirName(f->direction));
        name("name", f->name);
        w_.key("value");
        writeValue(f->value);
        span(f->span);
        w_.endObject();
    }

    void writeTerminal(std::string_view key, const Terminal& t) {
        w_.key(key);
        w_.beginObject();
        if (t.dot) w_.field("dot", true);
        else name("name", t.name);
        writeRange("range", t.range);
        span(t.span);
        w_.endObject();
    }

    void writeDesignator(const Designator& d) {
        w_.key("designator");
        w_.beginObject();
        name("prefix", d.prefix);
        w_.field("kind", desigKindName(d.kind));
        if (d.kind == DesignatorKind::Numbered) w_.field("number", d.number);
        if (d.kind == DesignatorKind::Range) {
            w_.key("parts");
            w_.beginArray();
            for (const DesigPart& p : d.parts) {
                w_.beginArray();
                w_.value(p.lo);
                w_.value(p.hi);
                w_.endArray();
            }
            w_.endArray();
        }
        // The exact byte range "manta annotate" rewrites (spec 13.7).
        w_.key("assignAt");
        w_.beginArray();
        w_.value(static_cast<std::int64_t>(d.assignmentSpan.offset));
        w_.value(static_cast<std::int64_t>(d.assignmentSpan.length));
        w_.endArray();
        span(d.span);
        w_.endObject();
    }

    void writeInstance(const Instance* i) {
        w_.key("instance");
        w_.beginObject();
        if (i->dnp) w_.field("dnp", true);
        writeDesignator(i->designator);
        w_.field("declares", i->declares);
        if (i->declares) name("part", i->partOrBlock);
        if (!i->bindings.empty()) {
            w_.key("bindings");
            w_.beginArray();
            for (const Binding* b : i->bindings) {
                w_.beginObject();
                w_.field("b", bindingKindName(b->kind));
                switch (b->kind) {
                    case BindingKind::PinNet:
                        if (b->pinIsDot) w_.field("dot", true);
                        else name("pin", b->pin);
                        writeRange("range", b->pinRange);
                        if (b->unbind) w_.field("unbind", true);
                        if (b->net) {
                            w_.key("net");
                            writeNet(b->net);
                        }
                        writeDirectives("directives", b->pinDirectives);
                        writeFields("fields", b->pinFields);
                        break;
                    case BindingKind::Field:
                        w_.key("field");
                        writeField(b->field);
                        break;
                    case BindingKind::Directive:
                        w_.key("directive");
                        writeDirective(b->directive);
                        break;
                }
                span(b->span);
                w_.endObject();
            }
            w_.endArray();
        }
        span(i->span);
        w_.endObject();
    }

    void writeDevice(const Device* d) {
        w_.beginObject();
        if (d->hasEntry) writeTerminal("entry", d->entry);
        writeInstance(d->instance);
        if (d->hasExit) writeTerminal("exit", d->exit);
        span(d->span);
        w_.endObject();
    }

    void writeElement(const Element* e) {
        w_.beginObject();
        w_.field("e", elementKindName(e->kind));
        switch (e->kind) {
            case ElementKind::Net:
                w_.key("net");
                writeNet(e->net);
                break;
            case ElementKind::Device:
                w_.key("device");
                writeDevice(e->device);
                break;
            case ElementKind::Group:
                w_.key("body");
                writeSegment(e->group->body);
                w_.field("mult", multKindName(e->group->mult));
                if (e->group->mult != MultKind::None) w_.field("count", e->group->count);
                break;
            case ElementKind::Replication:
                w_.key("body");
                writeSegment(e->replication->body);
                w_.field("counted", e->replication->counted);
                if (e->replication->counted) {
                    w_.field("in", e->replication->inWidth);
                    w_.field("out", e->replication->outWidth);
                }
                break;
        }
        span(e->span);
        w_.endObject();
    }

    void writeSegment(const Segment* s) {
        if (!s) {
            w_.null();
            return;
        }
        w_.beginObject();
        w_.key("elements");
        w_.beginArray();
        for (const Element* e : s->elements) writeElement(e);
        w_.endArray();
        w_.key("connectors");
        w_.beginArray();
        for (Connector c : s->connectors) w_.value(connectorName(c));
        w_.endArray();
        span(s->span);
        w_.endObject();
    }

    void writeStmt(const Stmt* s) {
        w_.beginObject();
        w_.field("s", stmtKindName(s->kind));
        if (s->isExtern) w_.field("extern", true);
        switch (s->kind) {
            case StmtKind::Chain:
                w_.key("segments");
                w_.beginArray();
                for (const Segment* seg : s->chain->segments) writeSegment(seg);
                w_.endArray();
                break;
            case StmtKind::Field:
                w_.key("field");
                writeField(s->field);
                break;
            case StmtKind::PortList:
                w_.key("ports");
                w_.beginArray();
                for (const NetExpr* n : s->ports) writeNet(n);
                w_.endArray();
                writePort("arrow", s->listArrow);
                break;
        }
        writeDirectives("directives", s->directives);
        span(s->span);
        w_.endObject();
    }

    void writePinMap(const PinMap* p) {
        w_.beginObject();
        w_.field("lo", p->physLo);
        w_.field("hi", p->physHi);
        w_.field("physRange", p->physIsRange);
        name("logical", p->logical);
        if (p->hasMemberList) writeNames("members", p->memberList);
        writeRange("range", p->logicalRange);
        writePort("arrow", p->arrow);
        writeDirectives("directives", p->directives);
        writeFields("fields", p->fields);
        span(p->span);
        w_.endObject();
    }

    void writeMember(const MemberDecl* m) {
        w_.beginObject();
        name("name", m->name);
        writePort("arrow", m->arrow);
        writeDirectives("directives", m->directives);
        span(m->span);
        w_.endObject();
    }

    void writeItem(const Item* item) {
        w_.beginObject();
        w_.field("kind", itemKindName(item->kind));
        name("name", item->name);
        w_.field("static", item->isStatic);
        w_.key("body");
        w_.beginArray();
        for (const BodyEntry& e : item->body) {
            w_.beginObject();
            switch (e.kind) {
                case BodyKind::Item:
                    w_.key("item");
                    writeItem(e.item);
                    break;
                case BodyKind::Stmt:
                    w_.key("stmt");
                    writeStmt(e.stmt);
                    break;
                case BodyKind::Field:
                    w_.key("field");
                    writeField(e.field);
                    break;
                case BodyKind::PinMap:
                    w_.key("pin");
                    writePinMap(e.pin);
                    break;
                case BodyKind::Member:
                    w_.key("member");
                    writeMember(e.member);
                    break;
                case BodyKind::Directive:
                    w_.key("directive");
                    writeDirective(e.directive);
                    break;
            }
            w_.endObject();
        }
        w_.endArray();
        span(item->span);
        w_.endObject();
    }

    JsonWriter& w_;
    const StringInterner& in_;

public:
    // ---- enum spellings, shared with the reader --------------------------
    static std::string_view itemKindName(ItemKind k) {
        switch (k) {
            case ItemKind::Block: return "block";
            case ItemKind::Part: return "part";
            case ItemKind::Harness: return "harness";
            case ItemKind::Netclass: return "netclass";
            case ItemKind::Match: return "match";
        }
        return "block";
    }
    static std::string_view stmtKindName(StmtKind k) {
        switch (k) {
            case StmtKind::Chain: return "chain";
            case StmtKind::Field: return "field";
            case StmtKind::PortList: return "portlist";
        }
        return "chain";
    }
    static std::string_view elementKindName(ElementKind k) {
        switch (k) {
            case ElementKind::Net: return "net";
            case ElementKind::Device: return "device";
            case ElementKind::Group: return "group";
            case ElementKind::Replication: return "rep";
        }
        return "net";
    }
    static std::string_view connectorName(Connector c) {
        switch (c) {
            case Connector::Advance: return "=";
            case Connector::Same: return "==";
            case Connector::Gather: return "=*";
            case Connector::Broadcast: return "*=";
        }
        return "=";
    }
    static std::string_view multKindName(MultKind m) {
        switch (m) {
            case MultKind::None: return "none";
            case MultKind::Series: return "+";
            case MultKind::Parallel: return "|";
            case MultKind::Node: return "*";
        }
        return "none";
    }
    static std::string_view strengthName(Strength s) {
        switch (s) {
            case Strength::Weak: return "weak";
            case Strength::Normal: return "normal";
            case Strength::Locked: return "locked";
        }
        return "normal";
    }
    static std::string_view fieldDirName(FieldDirection d) {
        switch (d) {
            case FieldDirection::Local: return "local";
            case FieldDirection::Import: return "import";
            case FieldDirection::Export: return "export";
        }
        return "local";
    }
    static std::string_view portDirName(PortDir d) {
        switch (d) {
            case PortDir::None: return "none";
            case PortDir::In: return "in";
            case PortDir::Out: return "out";
            case PortDir::Bidir: return "bidir";
        }
        return "none";
    }
    static std::string_view desigKindName(DesignatorKind k) {
        switch (k) {
            case DesignatorKind::Unassigned: return "?";
            case DesignatorKind::Numbered: return "n";
            case DesignatorKind::Range: return "%";
        }
        return "?";
    }
    static std::string_view bindingKindName(BindingKind k) {
        switch (k) {
            case BindingKind::PinNet: return "pin";
            case BindingKind::Field: return "field";
            case BindingKind::Directive: return "directive";
        }
        return "pin";
    }
    static std::string_view valueKindName(ValueKind k) {
        switch (k) {
            case ValueKind::Integer: return "int";
            case ValueKind::Decimal: return "dec";
            case ValueKind::Dimensioned: return "dim";
            case ValueKind::Percentage: return "pct";
            case ValueKind::Tolerance: return "tol";
            case ValueKind::String: return "str";
            case ValueKind::Boolean: return "bool";
            case ValueKind::Identifier: return "id";
            case ValueKind::List: return "list";
            case ValueKind::Repeat: return "repeat";
            case ValueKind::Interp: return "interp";
            case ValueKind::Version: return "version";
            case ValueKind::Unbind: return "unbind";
        }
        return "int";
    }
};

}  // namespace

void writeObject(const SourceUnit& unit, const StringInterner& interner,
                 std::string_view sourcePath, std::string& out) {
    JsonWriter w(out, /*pretty=*/true);
    ObjectWriter ow(w, interner);
    ow.writeUnit(unit, sourcePath);
    out += '\n';
}

// ===========================================================================
// Reading
// ===========================================================================

namespace {

class ObjectReader {
public:
    ObjectReader(Arena& arena, StringInterner& interner, FileId file, DiagEngine& diags,
                 Span errorSpan)
        : arena_(arena), in_(interner), file_(file), diags_(diags), errorSpan_(errorSpan) {}

    [[nodiscard]] bool ok() const { return ok_; }

    SourceUnit readUnit(const JsonValue& root) {
        SourceUnit unit;
        unit.file = file_;
        const JsonValue* decls = root.arr("declarations");
        if (!decls) {
            fail("object has no 'declarations' array");
            return unit;
        }
        std::vector<Item*> items;
        for (const JsonPtr& d : decls->array) items.push_back(readItem(*d));
        unit.items = commit(items);
        return unit;
    }

private:
    void fail(std::string message) {
        if (ok_) {
            ok_ = false;
            diags_.report(DiagId::Io, errorSpan_, std::move(message));
        }
    }

    template <typename T>
    std::span<T> commit(std::vector<T>& v) {
        auto dst = arena_.makeArray<T>(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) dst[i] = v[i];
        return dst;
    }

    Span readSpan(const JsonValue& o) {
        const JsonValue* a = o.arr("at");
        if (!a || a->array.size() != 2) return Span{file_, 0, 0};
        return Span{file_, static_cast<std::uint32_t>(a->array[0]->asInteger()),
                    static_cast<std::uint32_t>(a->array[1]->asInteger())};
    }

    Name readName(const JsonValue* v) {
        Name n;
        if (!v || v->kind == JsonKind::Null) return n;
        if (v->kind == JsonKind::String) {
            n.symbol = in_.intern(v->text);
            return n;
        }
        if (v->kind == JsonKind::Object) {
            const JsonValue* interp = v->arr("interp");
            if (interp) n.interp = readInterp(*interp);
            n.span = readSpan(*v);
        }
        return n;
    }

    InterpText* readInterp(const JsonValue& arr) {
        auto* t = arena_.make<InterpText>();
        std::vector<InterpChunk> chunks;
        for (const JsonPtr& c : arr.array) {
            InterpChunk chunk;
            if (const JsonValue* e = c->find("expr")) {
                chunk.isExpr = true;
                chunk.expr = readExpr(*e);
            } else {
                chunk.literal = in_.intern(c->str("text"));
            }
            chunk.span = readSpan(*c);
            t->span = t->span.merge(chunk.span);
            chunks.push_back(chunk);
        }
        t->chunks = commit(chunks);
        return t;
    }

    Expr* readExpr(const JsonValue& o) {
        if (o.kind != JsonKind::Object) return nullptr;
        auto* e = arena_.make<Expr>();
        e->span = readSpan(o);
        if (const JsonValue* v = o.find("int")) {
            e->kind = ExprKind::IntLit;
            e->intVal = v->asInteger();
        } else if (const JsonValue* b = o.find("bool")) {
            e->kind = ExprKind::BoolLit;
            e->boolVal = b->boolean;
        } else if (const JsonValue* f = o.find("field")) {
            e->kind = ExprKind::FieldRef;
            e->field = in_.intern(f->text);
        } else if (const JsonValue* u = o.find("unary")) {
            e->kind = ExprKind::Unary;
            e->unOp = u->text == "-" ? UnOp::Neg : UnOp::Not;
            if (const JsonValue* operand = o.find("operand")) e->lhs = readExpr(*operand);
        } else if (const JsonValue* op = o.find("op")) {
            e->kind = ExprKind::Binary;
            e->binOp = binOpFromText(op->text);
            if (const JsonValue* l = o.find("lhs")) e->lhs = readExpr(*l);
            if (const JsonValue* r = o.find("rhs")) e->rhs = readExpr(*r);
        } else {
            fail("malformed expression in object");
        }
        return e;
    }

    static BinOp binOpFromText(std::string_view t) {
        if (t == "^") return BinOp::Pow;
        if (t == "*") return BinOp::Mul;
        if (t == "/") return BinOp::Div;
        if (t == "+") return BinOp::Add;
        if (t == "-") return BinOp::Sub;
        if (t == "<") return BinOp::Lt;
        if (t == ">") return BinOp::Gt;
        if (t == "<=") return BinOp::Le;
        if (t == ">=") return BinOp::Ge;
        if (t == "=") return BinOp::Eq;
        if (t == "!=") return BinOp::Ne;
        if (t == "&") return BinOp::And;
        if (t == "~") return BinOp::Xor;
        return BinOp::Or;
    }

    Value* readValue(const JsonValue* o) {
        if (!o || o->kind != JsonKind::Object) return nullptr;
        auto* v = arena_.make<Value>();
        v->span = readSpan(*o);
        std::string_view k = o->str("v");

        if (k == "int") v->kind = ValueKind::Integer;
        else if (k == "dec") v->kind = ValueKind::Decimal;
        else if (k == "dim") v->kind = ValueKind::Dimensioned;
        else if (k == "pct") v->kind = ValueKind::Percentage;
        else if (k == "tol") v->kind = ValueKind::Tolerance;
        else if (k == "str") v->kind = ValueKind::String;
        else if (k == "bool") v->kind = ValueKind::Boolean;
        else if (k == "id") v->kind = ValueKind::Identifier;
        else if (k == "list") v->kind = ValueKind::List;
        else if (k == "repeat") v->kind = ValueKind::Repeat;
        else if (k == "interp") v->kind = ValueKind::Interp;
        else if (k == "version") v->kind = ValueKind::Version;
        else if (k == "unbind") v->kind = ValueKind::Unbind;
        else fail(std::format("unknown value kind '{}' in object", k));

        switch (v->kind) {
            case ValueKind::Integer:
            case ValueKind::Decimal:
            case ValueKind::Dimensioned:
            case ValueKind::Percentage:
            case ValueKind::Tolerance:
                if (!parseDimensioned(o->str("num"), v->num)) {
                    fail(std::format("malformed numeric value '{}' in object", o->str("num")));
                }
                if (const JsonValue* t = o->find("text")) v->text = in_.intern(t->text);
                break;
            case ValueKind::String:
            case ValueKind::Identifier:
                v->text = in_.intern(o->str("text"));
                break;
            case ValueKind::Boolean:
                v->boolean = o->boolean_("bool");
                v->upperCaseSpelling = o->boolean_("upper");
                break;
            case ValueKind::List: {
                std::vector<Value*> items;
                if (const JsonValue* a = o->arr("items")) {
                    for (const JsonPtr& item : a->array) items.push_back(readValue(item.get()));
                }
                v->list = commit(items);
                break;
            }
            case ValueKind::Repeat:
                v->inner = readValue(o->find("inner"));
                v->count = o->integer("count");
                break;
            case ValueKind::Interp:
                if (const JsonValue* a = o->arr("interp")) v->interp = readInterp(*a);
                break;
            case ValueKind::Version: {
                v->text = in_.intern(o->str("text"));
                if (const JsonValue* r = o->arr("range"); r && r->array.size() == 6) {
                    v->version.hasLo = r->array[0]->asInteger() != 0;
                    v->version.loMajor = static_cast<std::uint32_t>(r->array[1]->asInteger());
                    v->version.loMinor = static_cast<std::uint32_t>(r->array[2]->asInteger());
                    v->version.hasHi = r->array[3]->asInteger() != 0;
                    v->version.hiMajor = static_cast<std::uint32_t>(r->array[4]->asInteger());
                    v->version.hiMinor = static_cast<std::uint32_t>(r->array[5]->asInteger());
                }
                break;
            }
            case ValueKind::Unbind:
                break;
        }
        return v;
    }

    Index readIndex(const JsonValue* o) {
        Index i;
        if (!o) return i;
        i.span = readSpan(*o);
        if (const JsonValue* e = o->find("expr")) {
            i.isExpr = true;
            i.expr = readExpr(*e);
        } else {
            i.literal = o->integer("n");
        }
        return i;
    }

    Range readRange(const JsonValue* o) {
        Range r;
        if (!o || o->kind != JsonKind::Object) return r;
        r.present = true;
        r.single = o->boolean_("single");
        r.lo = readIndex(o->find("lo"));
        r.hi = readIndex(o->find("hi"));
        r.span = readSpan(*o);
        return r;
    }

    PortSpec readPort(const JsonValue* o) {
        PortSpec p;
        if (!o || o->kind != JsonKind::Object) return p;
        std::string_view d = o->str("dir");
        if (d == "in") p.dir = PortDir::In;
        else if (d == "out") p.dir = PortDir::Out;
        else if (d == "bidir") p.dir = PortDir::Bidir;
        p.global = o->boolean_("global");
        p.leading = o->boolean_("leading");
        p.span = readSpan(*o);
        return p;
    }

    std::span<Name> readNames(const JsonValue* a) {
        std::vector<Name> names;
        if (a) {
            for (const JsonPtr& n : a->array) names.push_back(readName(n->find("n")));
        }
        return commit(names);
    }

    NetExpr* readNet(const JsonValue* o) {
        if (!o || o->kind != JsonKind::Object) return nullptr;
        auto* n = arena_.make<NetExpr>();
        n->leading = readPort(o->find("lead"));
        n->trailing = readPort(o->find("trail"));
        n->perCopy = o->boolean_("perCopy");
        n->path = readNames(o->arr("path"));
        if (const JsonValue* m = o->arr("members")) {
            n->memberList = readNames(m);
            n->hasMemberList = true;
        }
        n->range = readRange(o->find("range"));
        if (const JsonValue* pc = o->arr("perCopyList")) {
            std::vector<Value*> items;
            for (const JsonPtr& v : pc->array) items.push_back(readValue(v.get()));
            n->perCopyList = commit(items);
            n->hasPerCopyList = true;
        }
        n->span = readSpan(*o);
        return n;
    }

    static Strength readStrength(std::string_view s) {
        if (s == "weak") return Strength::Weak;
        if (s == "locked") return Strength::Locked;
        return Strength::Normal;
    }

    FieldDecl* readField(const JsonValue* o) {
        if (!o || o->kind != JsonKind::Object) return nullptr;
        auto* f = arena_.make<FieldDecl>();
        f->ns = o->str("ns") == "@" ? FieldNamespace::System : FieldNamespace::User;
        f->strength = readStrength(o->str("strength"));
        std::string_view d = o->str("dir");
        f->direction = d == "import"   ? FieldDirection::Import
                       : d == "export" ? FieldDirection::Export
                                       : FieldDirection::Local;
        f->name = readName(o->find("name"));
        f->value = readValue(o->find("value"));
        f->span = readSpan(*o);
        return f;
    }

    Directive* readDirective(const JsonValue* o) {
        if (!o || o->kind != JsonKind::Object) return nullptr;
        auto* d = arena_.make<Directive>();
        d->strength = readStrength(o->str("strength"));
        d->name = readName(o->find("name"));
        d->value = readValue(o->find("value"));
        if (const JsonValue* m = o->find("match"); m && m->kind == JsonKind::Object) {
            auto* ref = arena_.make<MatchRef>();
            ref->group = readName(m->find("group"));
            std::vector<FieldDecl*> overrides;
            if (const JsonValue* ov = m->arr("overrides")) {
                for (const JsonPtr& f : ov->array) overrides.push_back(readField(f.get()));
            }
            ref->overrides = commit(overrides);
            ref->span = readSpan(*m);
            d->matchRef = ref;
        }
        d->span = readSpan(*o);
        return d;
    }

    std::span<Directive*> readDirectives(const JsonValue* a) {
        std::vector<Directive*> ds;
        if (a) {
            for (const JsonPtr& d : a->array) ds.push_back(readDirective(d.get()));
        }
        return commit(ds);
    }

    std::span<FieldDecl*> readFields(const JsonValue* a) {
        std::vector<FieldDecl*> fs;
        if (a) {
            for (const JsonPtr& f : a->array) fs.push_back(readField(f.get()));
        }
        return commit(fs);
    }

    Terminal readTerminal(const JsonValue* o) {
        Terminal t;
        if (!o) return t;
        t.dot = o->boolean_("dot");
        if (!t.dot) t.name = readName(o->find("name"));
        t.range = readRange(o->find("range"));
        t.span = readSpan(*o);
        return t;
    }

    Designator readDesignator(const JsonValue* o) {
        Designator d;
        if (!o) return d;
        d.prefix = readName(o->find("prefix"));
        std::string_view k = o->str("kind");
        d.kind = k == "n" ? DesignatorKind::Numbered
                 : k == "%" ? DesignatorKind::Range
                            : DesignatorKind::Unassigned;
        d.number = o->integer("number");
        if (const JsonValue* parts = o->arr("parts")) {
            std::vector<DesigPart> ps;
            for (const JsonPtr& p : parts->array) {
                DesigPart part;
                if (p->array.size() == 2) {
                    part.lo = p->array[0]->asInteger();
                    part.hi = p->array[1]->asInteger();
                }
                ps.push_back(part);
            }
            d.parts = commit(ps);
        }
        if (const JsonValue* a = o->arr("assignAt"); a && a->array.size() == 2) {
            d.assignmentSpan = Span{file_, static_cast<std::uint32_t>(a->array[0]->asInteger()),
                                    static_cast<std::uint32_t>(a->array[1]->asInteger())};
        }
        d.span = readSpan(*o);
        return d;
    }

    Instance* readInstance(const JsonValue* o) {
        if (!o) return nullptr;
        auto* i = arena_.make<Instance>();
        i->dnp = o->boolean_("dnp");
        i->designator = readDesignator(o->find("designator"));
        i->declares = o->boolean_("declares");
        if (i->declares) i->partOrBlock = readName(o->find("part"));
        if (const JsonValue* bs = o->arr("bindings")) {
            std::vector<Binding*> bindings;
            for (const JsonPtr& b : bs->array) {
                auto* binding = arena_.make<Binding>();
                std::string_view k = b->str("b");
                binding->kind = k == "field"       ? BindingKind::Field
                                : k == "directive" ? BindingKind::Directive
                                                   : BindingKind::PinNet;
                switch (binding->kind) {
                    case BindingKind::PinNet:
                        binding->pinIsDot = b->boolean_("dot");
                        if (!binding->pinIsDot) binding->pin = readName(b->find("pin"));
                        binding->pinRange = readRange(b->find("range"));
                        binding->unbind = b->boolean_("unbind");
                        binding->net = readNet(b->find("net"));
                        binding->pinDirectives = readDirectives(b->arr("directives"));
                        binding->pinFields = readFields(b->arr("fields"));
                        break;
                    case BindingKind::Field:
                        binding->field = readField(b->find("field"));
                        break;
                    case BindingKind::Directive:
                        binding->directive = readDirective(b->find("directive"));
                        break;
                }
                binding->span = readSpan(*b);
                bindings.push_back(binding);
            }
            i->bindings = commit(bindings);
        }
        i->span = readSpan(*o);
        return i;
    }

    Device* readDevice(const JsonValue* o) {
        if (!o) return nullptr;
        auto* d = arena_.make<Device>();
        if (const JsonValue* e = o->find("entry")) {
            d->entry = readTerminal(e);
            d->hasEntry = true;
        }
        d->instance = readInstance(o->find("instance"));
        if (const JsonValue* e = o->find("exit")) {
            d->exit = readTerminal(e);
            d->hasExit = true;
        }
        d->span = readSpan(*o);
        return d;
    }

    Element* readElement(const JsonValue& o) {
        auto* e = arena_.make<Element>();
        std::string_view k = o.str("e");
        e->kind = k == "device" ? ElementKind::Device
                  : k == "group" ? ElementKind::Group
                  : k == "rep"   ? ElementKind::Replication
                                 : ElementKind::Net;
        switch (e->kind) {
            case ElementKind::Net:
                e->net = readNet(o.find("net"));
                break;
            case ElementKind::Device:
                e->device = readDevice(o.find("device"));
                break;
            case ElementKind::Group: {
                auto* g = arena_.make<Group>();
                g->body = readSegment(o.find("body"));
                std::string_view m = o.str("mult");
                g->mult = m == "+"   ? MultKind::Series
                          : m == "|" ? MultKind::Parallel
                          : m == "*" ? MultKind::Node
                                     : MultKind::None;
                g->count = o.integer("count");
                g->span = readSpan(o);
                e->group = g;
                break;
            }
            case ElementKind::Replication: {
                auto* r = arena_.make<Replication>();
                r->body = readSegment(o.find("body"));
                r->counted = o.boolean_("counted");
                r->inWidth = o.integer("in");
                r->outWidth = o.integer("out");
                r->span = readSpan(o);
                e->replication = r;
                break;
            }
        }
        e->span = readSpan(o);
        return e;
    }

    Segment* readSegment(const JsonValue* o) {
        if (!o || o->kind != JsonKind::Object) return nullptr;
        auto* s = arena_.make<Segment>();
        std::vector<Element*> elements;
        if (const JsonValue* a = o->arr("elements")) {
            for (const JsonPtr& e : a->array) elements.push_back(readElement(*e));
        }
        s->elements = commit(elements);
        std::vector<Connector> connectors;
        if (const JsonValue* a = o->arr("connectors")) {
            for (const JsonPtr& c : a->array) {
                connectors.push_back(c->text == "==" ? Connector::Same
                                     : c->text == "=*" ? Connector::Gather
                                     : c->text == "*=" ? Connector::Broadcast
                                                       : Connector::Advance);
            }
        }
        s->connectors = commit(connectors);
        s->span = readSpan(*o);
        return s;
    }

    Stmt* readStmt(const JsonValue* o) {
        if (!o) return nullptr;
        auto* s = arena_.make<Stmt>();
        std::string_view k = o->str("s");
        s->kind = k == "field" ? StmtKind::Field
                  : k == "portlist" ? StmtKind::PortList
                                    : StmtKind::Chain;
        s->isExtern = o->boolean_("extern");
        switch (s->kind) {
            case StmtKind::Chain: {
                auto* c = arena_.make<Chain>();
                std::vector<Segment*> segs;
                if (const JsonValue* a = o->arr("segments")) {
                    for (const JsonPtr& seg : a->array) segs.push_back(readSegment(seg.get()));
                }
                c->segments = commit(segs);
                s->chain = c;
                break;
            }
            case StmtKind::Field:
                s->field = readField(o->find("field"));
                break;
            case StmtKind::PortList: {
                std::vector<NetExpr*> ports;
                if (const JsonValue* a = o->arr("ports")) {
                    for (const JsonPtr& p : a->array) ports.push_back(readNet(p.get()));
                }
                s->ports = commit(ports);
                s->listArrow = readPort(o->find("arrow"));
                break;
            }
        }
        s->directives = readDirectives(o->arr("directives"));
        s->span = readSpan(*o);
        return s;
    }

    PinMap* readPinMap(const JsonValue* o) {
        if (!o) return nullptr;
        auto* p = arena_.make<PinMap>();
        p->physLo = o->integer("lo");
        p->physHi = o->integer("hi");
        p->physIsRange = o->boolean_("physRange");
        p->logical = readName(o->find("logical"));
        if (const JsonValue* m = o->arr("members")) {
            p->memberList = readNames(m);
            p->hasMemberList = true;
        }
        p->logicalRange = readRange(o->find("range"));
        p->arrow = readPort(o->find("arrow"));
        p->directives = readDirectives(o->arr("directives"));
        p->fields = readFields(o->arr("fields"));
        p->span = readSpan(*o);
        return p;
    }

    MemberDecl* readMember(const JsonValue* o) {
        if (!o) return nullptr;
        auto* m = arena_.make<MemberDecl>();
        m->name = readName(o->find("name"));
        m->arrow = readPort(o->find("arrow"));
        m->directives = readDirectives(o->arr("directives"));
        m->span = readSpan(*o);
        return m;
    }

    Item* readItem(const JsonValue& o) {
        auto* item = arena_.make<Item>();
        std::string_view k = o.str("kind");
        item->kind = k == "part"       ? ItemKind::Part
                     : k == "harness"  ? ItemKind::Harness
                     : k == "netclass" ? ItemKind::Netclass
                     : k == "match"    ? ItemKind::Match
                                       : ItemKind::Block;
        item->name = readName(o.find("name"));
        item->nameSpan = item->name.span;
        item->isStatic = o.boolean_("static");

        std::vector<BodyEntry> body;
        if (const JsonValue* a = o.arr("body")) {
            for (const JsonPtr& e : a->array) {
                BodyEntry entry;
                if (const JsonValue* v = e->find("item")) {
                    entry.kind = BodyKind::Item;
                    entry.item = readItem(*v);
                } else if (const JsonValue* v2 = e->find("stmt")) {
                    entry.kind = BodyKind::Stmt;
                    entry.stmt = readStmt(v2);
                } else if (const JsonValue* v3 = e->find("field")) {
                    entry.kind = BodyKind::Field;
                    entry.field = readField(v3);
                } else if (const JsonValue* v4 = e->find("pin")) {
                    entry.kind = BodyKind::PinMap;
                    entry.pin = readPinMap(v4);
                } else if (const JsonValue* v5 = e->find("member")) {
                    entry.kind = BodyKind::Member;
                    entry.member = readMember(v5);
                } else if (const JsonValue* v6 = e->find("directive")) {
                    entry.kind = BodyKind::Directive;
                    entry.directive = readDirective(v6);
                } else {
                    fail("unknown body entry in object");
                    continue;
                }
                body.push_back(entry);
            }
        }
        item->body = commit(body);
        item->span = readSpan(o);
        return item;
    }

    Arena& arena_;
    StringInterner& in_;
    FileId file_;
    DiagEngine& diags_;
    Span errorSpan_;
    bool ok_ = true;
};

}  // namespace

bool readObject(const JsonValue& root, Arena& arena, StringInterner& interner, FileId file,
                DiagEngine& diags, Span errorSpan, ObjectFile& out) {
    if (root.kind != JsonKind::Object) {
        diags.report(DiagId::Io, errorSpan, std::string("object file is not a JSON object"));
        return false;
    }
    std::string_view kind = root.str("kind");
    if (kind != "mantaO") {
        diags.report(DiagId::Io, errorSpan,
                     std::format("expected a '.mantaO' object, found kind '{}'", kind));
        return false;
    }
    out.version = std::string(root.str("version"));
    // An object may target an *older* revision than the toolchain: 1.1 only
    // added a lexical marker, so a 1.0 object is perfectly good input. What
    // cannot be read is an object from a newer revision, whose constructs this
    // toolchain does not know.
    if (!revisionAtMost(out.version, kLanguageVersion)) {
        diags.report(DiagId::Io, errorSpan,
                     std::format("object targets language revision {}, this toolchain is {}",
                                 out.version, kLanguageVersion));
        return false;
    }
    out.sourcePath = std::string(root.str("source"));

    ObjectReader reader(arena, interner, file, diags, errorSpan);
    out.unit = reader.readUnit(root);
    return reader.ok();
}

}  // namespace manta
