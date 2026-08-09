// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fmt/formatter.h"

#include <algorithm>
#include <format>
#include <vector>

namespace manta {

namespace {

class Printer {
public:
    Printer(const TokenStream& tokens, const SourceFile& file, const StringInterner& interner,
            const FormatOptions& options)
        : tokens_(tokens), file_(file), in_(interner), opt_(options) {}

    std::string run(const SourceUnit& unit) {
        for (std::size_t i = 0; i < unit.items.size(); ++i) {
            if (i > 0) blankLine();
            flushCommentsBefore(unit.items[i]->span.offset);
            item(unit.items[i]);
        }
        flushCommentsBefore(UINT32_MAX);
        // Exactly one trailing newline.
        while (out_.size() >= 2 && out_[out_.size() - 1] == '\n' && out_[out_.size() - 2] == '\n') {
            out_.pop_back();
        }
        if (out_.empty() || out_.back() != '\n') out_ += '\n';

        // Everything from the end-of-content marker on is reproduced byte for
        // byte, marker included. It is not manta -- it could be Markdown, a
        // table with deliberate trailing spaces, or a base64 blob -- so it is
        // not normalised, not reindented, and above all not dropped.
        if (tokens_.hasEndMarker()) {
            out_ += '\n';
            out_ += file_.text().substr(tokens_.contentEnd);
        }
        return std::move(out_);
    }

private:
    // ---- output primitives ------------------------------------------------
    void indent() { line_.append(depth_ * opt_.indentWidth, ' '); }

    void emit(std::string_view text) { line_ += text; }

    void endLine() {
        // Trailing whitespace never survives a format.
        while (!line_.empty() && line_.back() == ' ') line_.pop_back();
        out_ += line_;
        out_ += '\n';
        line_.clear();
    }

    void blankLine() {
        if (!out_.empty() && !(out_.size() >= 2 && out_[out_.size() - 2] == '\n')) out_ += '\n';
    }

    // ---- comments ---------------------------------------------------------
    // Spec 17: the formatter "does not alter ... comments". They are reproduced
    // verbatim; only their surrounding whitespace is normalised.
    void flushCommentsBefore(std::uint32_t offset) {
        while (commentIndex_ < tokens_.comments.size() &&
               tokens_.comments[commentIndex_].offset < offset) {
            const Comment& c = tokens_.comments[commentIndex_++];
            std::string_view text = file_.text().substr(c.offset, c.length);

            if (!c.ownLine && !out_.empty()) {
                // A comment that sat beside code stays beside it.
                if (!line_.empty()) {
                    emit("  ");
                    emit(text);
                    continue;
                }
                if (out_.back() == '\n') {
                    out_.pop_back();
                    out_ += "  ";
                    out_ += text;
                    out_ += '\n';
                    continue;
                }
            }

            if (c.blankLineBefore) blankLine();
            indent();
            emit(text);
            endLine();
        }
    }

    [[nodiscard]] std::string_view text(SymbolId s) const { return in_.text(s); }

    // ---- names, values, expressions ---------------------------------------
    std::string name(const Name& n) const {
        if (n.isInterpolated()) return interp(n.interp);
        return valid(n.symbol) ? std::string(in_.text(n.symbol)) : std::string{};
    }

    std::string interp(const InterpText* t) const {
        std::string out;
        for (const InterpChunk& c : t->chunks) {
            if (c.isExpr) {
                out += '$';
                out += expr(c.expr, 0);
                out += '$';
            } else {
                out += in_.text(c.literal);
            }
        }
        return out;
    }

    // Emits an expression with the minimum parentheses its precedence needs.
    std::string expr(const Expr* e, int parentPrecedence) const {
        if (!e) return {};
        switch (e->kind) {
            case ExprKind::IntLit: return std::to_string(e->intVal);
            case ExprKind::BoolLit: return e->boolVal ? "true" : "false";
            case ExprKind::FieldRef: {
                std::string_view n = in_.text(e->field);
                // Spec 14.5: a hyphenated field name is referenced by quoting
                // it, because '-' inside "$...$" is always subtraction.
                if (n.find('-') != std::string_view::npos) {
                    return "\"" + std::string(n) + "\"";
                }
                return std::string(n);
            }
            case ExprKind::Unary:
                return (e->unOp == UnOp::Neg ? "-" : "!") + expr(e->lhs, 9);
            case ExprKind::Binary: {
                int prec = precedenceOf(e->binOp);
                std::string body = expr(e->lhs, prec) + " " +
                                   std::string(binOpText(e->binOp)) + " " +
                                   expr(e->rhs, prec + (e->binOp == BinOp::Pow ? -1 : 1));
                return prec < parentPrecedence ? "(" + body + ")" : body;
            }
        }
        return {};
    }

    static int precedenceOf(BinOp op) {
        switch (op) {
            case BinOp::Pow: return 8;
            case BinOp::Mul:
            case BinOp::Div: return 7;
            case BinOp::Add:
            case BinOp::Sub: return 6;
            case BinOp::Lt:
            case BinOp::Gt:
            case BinOp::Le:
            case BinOp::Ge: return 5;
            case BinOp::Eq:
            case BinOp::Ne: return 4;
            case BinOp::And: return 3;
            case BinOp::Xor: return 2;
            case BinOp::Or: return 1;
        }
        return 1;
    }

    std::string value(const Value* v) const {
        if (!v) return {};
        switch (v->kind) {
            case ValueKind::Integer:
            case ValueKind::Decimal:
            case ValueKind::Dimensioned:
            case ValueKind::Percentage:
                // Spec 17: "emits SI-substituted values".
                return v->num.canonical();
            case ValueKind::Tolerance:
                return "\xC2\xB1" + v->num.canonical();
            case ValueKind::String: {
                std::string out = "\"";
                for (char c : in_.text(v->text)) {
                    switch (c) {
                        case '"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\n': out += "\\n"; break;
                        case '\t': out += "\\t"; break;
                        default: out += c;
                    }
                }
                return out + "\"";
            }
            case ValueKind::Identifier:
                // Spec 17: "does not alter case", so the lexeme is reproduced.
                return std::string(in_.text(v->text));
            case ValueKind::Version: {
                // Rendered from the constraint, not the lexeme: spec 4.3's
                // "1.2+" lexes as the word "1.2" followed by a separate '+',
                // so reproducing the word alone would silently drop the bound.
                const VersionConstraint& c = v->version;
                auto rev = [](std::uint32_t major, std::uint32_t minor) {
                    return std::to_string(major) + "." + std::to_string(minor);
                };
                if (c.hasLo && c.hasHi) {
                    if (c.loMajor == c.hiMajor && c.loMinor == c.hiMinor) {
                        return rev(c.loMajor, c.loMinor);
                    }
                    return rev(c.loMajor, c.loMinor) + "-" + rev(c.hiMajor, c.hiMinor);
                }
                if (c.hasLo) return rev(c.loMajor, c.loMinor) + "+";
                if (c.hasHi) return rev(c.hiMajor, c.hiMinor) + "-";
                return std::string(in_.text(v->text));
            }
            case ValueKind::Boolean:
                return v->upperCaseSpelling ? (v->boolean ? "TRUE" : "FALSE")
                                            : (v->boolean ? "true" : "false");
            case ValueKind::Range:
                return std::format("{}:{}", v->rangeLo, v->rangeHi);
            case ValueKind::List: {
                std::string out = "[";
                for (std::size_t i = 0; i < v->list.size(); ++i) {
                    if (i) out += ", ";
                    out += value(v->list[i]);
                }
                return out + "]";
            }
            case ValueKind::Repeat:
                return "(" + value(v->inner) + ")*" + std::to_string(v->count);
            case ValueKind::Interp:
                return interp(v->interp);
            case ValueKind::Unbind:
                return "?";
        }
        return {};
    }

    std::string index(const Index& i) const {
        if (i.isExpr) return "$" + expr(i.expr, 0) + "$";
        return std::to_string(i.literal);
    }

    std::string range(const Range& r) const {
        if (!r.present) return {};
        if (r.single) return "[" + index(r.lo) + "]";
        return "[" + index(r.lo) + ":" + index(r.hi) + "]";
    }

    // ---- fields and directives --------------------------------------------
    static std::string_view strengthSigil(Strength s) {
        switch (s) {
            case Strength::Weak: return "~";
            case Strength::Locked: return "!";
            case Strength::Normal: return "";
        }
        return "";
    }

    // Spec 9.4: "Canonical sigil order is direction, sigil, strength for an
    // import ('>#~'), and sigil, strength, direction for an export ('#!>').
    // The formatter rewrites any other ordering."
    std::string fieldHead(const FieldDecl* f) const {
        std::string_view sigil = f->ns == FieldNamespace::System ? "@" : "#";
        std::string out;
        if (f->direction == FieldDirection::Import) out += '>';
        out += sigil;
        out += strengthSigil(f->strength);
        if (f->direction == FieldDirection::Export) out += '>';
        out += name(f->name);
        return out;
    }

    std::string field(const FieldDecl* f) const {
        return fieldHead(f) + " = " + value(f->value);
    }

    std::string directive(const Directive* d) const {
        std::string out = "&";
        out += strengthSigil(d->strength);
        out += name(d->name);
        if (d->matchRef) {
            out += "={";
            out += name(d->matchRef->group);
            if (!d->matchRef->overrides.empty()) {
                out += ": ";
                for (std::size_t i = 0; i < d->matchRef->overrides.size(); ++i) {
                    if (i) out += "; ";
                    out += field(d->matchRef->overrides[i]);
                }
            }
            out += "}";
        } else if (d->value) {
            out += "=";
            out += value(d->value);
        }
        return out;
    }

    std::string directives(std::span<Directive* const> ds) const {
        std::string out;
        for (const Directive* d : ds) {
            out += ' ';
            out += directive(d);
        }
        return out;
    }

    // '#' fields on a pin. Emitted after the directives and, like them, without
    // spaces around the '=' -- they sit inline on a pin map line rather than
    // standing alone as a statement, so "#VOH=2V4" reads beside "&TYPE=POWER".
    std::string pinFields(std::span<FieldDecl* const> fs) const {
        std::string out;
        for (const FieldDecl* f : fs) {
            out += ' ';
            out += fieldHead(f);
            out += '=';
            out += value(f->value);
        }
        return out;
    }

    // ---- ports and nets ---------------------------------------------------
    // Spec 10.1: "Canonical form, which the formatter emits: leading '>' at the
    // start of a statement, trailing '>' at the end, and 'pin=NET>' in a
    // binding."
    static std::string_view arrow(const PortSpec& p) {
        if (!p.present()) return "";
        if (p.global) return p.leading ? ">>" : ">>";
        switch (p.dir) {
            case PortDir::In: return p.leading ? ">" : "<";
            case PortDir::Out: return p.leading ? "<" : ">";
            case PortDir::Bidir: return "<>";
            case PortDir::None: return "";
        }
        return "";
    }

    std::string net(const NetExpr* n) const {
        std::string out;
        out += arrow(n->leading);
        if (n->perCopy) out += '%';

        if (n->hasPerCopyList) {
            out += '[';
            for (std::size_t i = 0; i < n->perCopyList.size(); ++i) {
                if (i) out += ", ";
                out += value(n->perCopyList[i]);
            }
            out += ']';
            return out;
        }

        for (std::size_t i = 0; i < n->path.size(); ++i) {
            if (i) out += '.';
            out += name(n->path[i]);
        }
        if (n->hasMemberList) {
            out += ".[";
            for (std::size_t i = 0; i < n->memberList.size(); ++i) {
                if (i) out += ",";
                out += name(n->memberList[i]);
            }
            out += ']';
        }
        out += range(n->range);
        out += arrow(n->trailing);
        return out;
    }

    // ---- devices ----------------------------------------------------------
    std::string terminal(const Terminal& t) const {
        if (t.dot) return ".";
        return name(t.name) + range(t.range);
    }

    std::string designator(const Designator& d) const {
        std::string out = name(d.prefix);
        switch (d.kind) {
            case DesignatorKind::Unassigned:
                out += '?';
                break;
            case DesignatorKind::Numbered:
                out += std::to_string(d.number);
                break;
            case DesignatorKind::Range:
                out += "%[";
                for (std::size_t i = 0; i < d.parts.size(); ++i) {
                    if (i) out += ',';
                    out += std::to_string(d.parts[i].lo);
                    if (d.parts[i].hi != d.parts[i].lo) {
                        out += ':';
                        out += std::to_string(d.parts[i].hi);
                    }
                }
                out += ']';
                break;
        }
        return out;
    }

    std::string binding(const Binding* b) const {
        switch (b->kind) {
            case BindingKind::Field: return field(b->field);
            case BindingKind::Directive: return directive(b->directive);
            case BindingKind::PinNet: break;
        }
        std::string out = b->pinIsDot ? "." : name(b->pin) + range(b->pinRange);
        if (b->unbind) {
            out += " = ?";
        } else if (b->net) {
            out += " = ";
            out += net(b->net);
        }
        out += directives(b->pinDirectives);
        out += pinFields(b->pinFields);
        return out;
    }

    // The '=' column a binding block aligns on (spec 17).
    static std::size_t bindingKeyWidth(const Binding* b, const Printer& p) {
        if (b->kind != BindingKind::PinNet) return 0;
        if (b->unbind || b->net) {
            return (b->pinIsDot ? std::string(".") : p.name(b->pin) + p.range(b->pinRange)).size();
        }
        return 0;
    }

    std::string device(const Device* d) const {
        std::string out;
        if (d->hasEntry) out += terminal(d->entry);
        out += '{';
        out += instance(d->instance, /*multiline=*/false);
        out += '}';
        if (d->hasExit) out += terminal(d->exit);
        return out;
    }

    std::string instance(const Instance* inst, bool multiline) const {
        std::string out;
        if (inst->dnp) out += '!';
        out += designator(inst->designator);
        if (inst->declares) {
            out += '~';
            out += name(inst->partOrBlock);
        }
        if (inst->bindings.empty()) return out;

        // Align the '=' within the binding block (spec 17).
        std::size_t width = 0;
        for (const Binding* b : inst->bindings) {
            width = std::max(width, bindingKeyWidth(b, *this));
        }

        out += ": ";
        for (const Binding* b : inst->bindings) {
            if (multiline) {
                out += '\n';
                out.append((depth_ + 1) * opt_.indentWidth, ' ');
            }
            std::string one = binding(b);
            std::size_t key = bindingKeyWidth(b, *this);
            if (key > 0 && key < width) one.insert(key, std::string(width - key, ' '));
            out += one;
            // Spec 17: "emits a trailing ';' in binding lists".
            out += "; ";
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    // ---- chains -----------------------------------------------------------
    static std::string_view connector(Connector c) {
        switch (c) {
            case Connector::Advance: return "=";
            case Connector::Same: return "==";
            case Connector::Gather: return "=*";
            case Connector::Broadcast: return "*=";
        }
        return "=";
    }

    std::string element(const Element* e) const {
        switch (e->kind) {
            case ElementKind::Net: return net(e->net);
            case ElementKind::Device: return device(e->device);
            case ElementKind::Group: {
                std::string out = "(" + segment(e->group->body) + ")";
                switch (e->group->mult) {
                    case MultKind::Series: out += "+" + std::to_string(e->group->count); break;
                    case MultKind::Parallel: out += "|" + std::to_string(e->group->count); break;
                    case MultKind::Node: out += "*" + std::to_string(e->group->count); break;
                    case MultKind::None: break;
                }
                return out;
            }
            case ElementKind::Replication: {
                const Replication* r = e->replication;
                if (r->counted) {
                    return "[" + std::to_string(r->inWidth) + "[ " + segment(r->body) + " ]" +
                           std::to_string(r->outWidth) + "]";
                }
                return "[[" + segment(r->body) + "]]";
            }
        }
        return {};
    }

    std::string segment(const Segment* s) const {
        if (!s) return {};
        std::string out;
        for (std::size_t i = 0; i < s->elements.size(); ++i) {
            if (i) {
                out += ' ';
                out += connector(s->connectors[i - 1]);
                out += ' ';
            }
            out += element(s->elements[i]);
        }
        return out;
    }

    // Spec 2.4: whitespace is insignificant and a statement may span any number
    // of lines, so a long chain is broken at its connectors. Spec 11.2 notes
    // that "reflowing a statement across lines never changes its constraints".
    void chainStatement(const Stmt* stmt) {
        std::vector<std::string> pieces;
        std::vector<std::string> joins;

        for (std::size_t si = 0; si < stmt->chain->segments.size(); ++si) {
            const Segment* seg = stmt->chain->segments[si];
            if (si) joins.push_back("^");
            for (std::size_t i = 0; i < seg->elements.size(); ++i) {
                if (i) joins.push_back(std::string(connector(seg->connectors[i - 1])));
                pieces.push_back(element(seg->elements[i]));
            }
        }

        std::string tail = directives(stmt->directives) + ";";

        std::string single = stmt->isExtern ? "extern " : "";
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if (i) {
                single += ' ';
                single += joins[i - 1];
                single += ' ';
            }
            single += pieces[i];
        }
        single += tail;

        indent();
        if (line_.size() + single.size() <= opt_.lineWidth || pieces.size() == 1) {
            emit(single);
            endLine();
            return;
        }

        // Break before each connector, with the continuation indented so the
        // operators line up under one another.
        std::string head = stmt->isExtern ? "extern " : "";
        emit(head + pieces[0]);
        std::size_t continuation = (depth_ + 1) * opt_.indentWidth;
        for (std::size_t i = 1; i < pieces.size(); ++i) {
            endLine();
            line_.append(continuation, ' ');
            // Right-align the operator so '=' and '==' share a column.
            const std::string& op = joins[i - 1];
            if (op.size() < 2) line_ += ' ';
            emit(op);
            emit(" ");
            emit(pieces[i]);
        }
        emit(tail);
        endLine();
    }

    void statement(const Stmt* stmt) {
        flushCommentsBefore(stmt->span.offset);

        switch (stmt->kind) {
            case StmtKind::Field:
                indent();
                emit(field(stmt->field));
                emit(directives(stmt->directives));
                emit(";");
                endLine();
                return;

            case StmtKind::PortList: {
                indent();
                std::string out = "[";
                for (std::size_t i = 0; i < stmt->ports.size(); ++i) {
                    if (i) out += ", ";
                    out += net(stmt->ports[i]);
                }
                out += "]";
                out += arrow(stmt->listArrow);
                emit(out);
                emit(directives(stmt->directives));
                emit(";");
                endLine();
                return;
            }

            case StmtKind::Chain:
                chainStatement(stmt);
                return;
        }
    }

    // ---- pin maps and members ---------------------------------------------
    std::string pinSpec(const PinMap* p) const {
        if (p->physIsRange) {
            return "[" + std::to_string(p->physLo) + ":" + std::to_string(p->physHi) + "]";
        }
        return std::to_string(p->physLo);
    }

    std::string pinLogical(const PinMap* p) const {
        std::string out = name(p->logical);
        if (p->hasMemberList) {
            out += ".[";
            for (std::size_t i = 0; i < p->memberList.size(); ++i) {
                if (i) out += ",";
                out += name(p->memberList[i]);
            }
            out += ']';
        } else {
            out += range(p->logicalRange);
        }
        out += arrow(p->arrow);
        return out;
    }

    void memberDecl(const MemberDecl* m) {
        flushCommentsBefore(m->span.offset);
        indent();
        emit(name(m->name));
        emit(arrow(m->arrow));
        emit(directives(m->directives));
        emit(";");
        endLine();
    }

    // ---- declarations ------------------------------------------------------
    void item(const Item* it) {
        indent();
        if (it->isStatic) emit("static ");
        switch (it->kind) {
            case ItemKind::Block: emit("block "); break;
            case ItemKind::Part: emit("part "); break;
            case ItemKind::Harness: emit("harness "); break;
            case ItemKind::Netclass: emit("netclass "); break;
            case ItemKind::Match: emit("match "); break;
            case ItemKind::Cable: emit("cable "); break;
        }
        emit(name(it->name));
        emit(" {");
        endLine();
        ++depth_;

        if (it->kind == ItemKind::Part) {
            partBody(it);
        } else {
            genericBody(it);
        }

        --depth_;
        flushCommentsBefore(it->span.end() > 2 ? it->span.end() - 2 : it->span.offset);
        indent();
        emit("};");
        endLine();
    }

    // A part body aligns the '=' of its pin map and of its field declarations
    // (spec 17), each as its own block.
    void partBody(const Item* part) {
        std::size_t fieldWidth = 0;
        std::size_t pinWidth = 0;
        for (const BodyEntry& e : part->body) {
            if (e.kind == BodyKind::Field) {
                fieldWidth = std::max(fieldWidth, fieldHead(e.field).size());
            } else if (e.kind == BodyKind::PinMap) {
                pinWidth = std::max(pinWidth, pinSpec(e.pin).size());
            }
        }

        bool lastWasField = false;
        bool first = true;
        for (const BodyEntry& e : part->body) {
            if (e.kind == BodyKind::Field) {
                flushCommentsBefore(e.field->span.offset);
                indent();
                std::string head = fieldHead(e.field);
                emit(head);
                emit(std::string(fieldWidth - head.size() + 1, ' '));
                emit("= ");
                emit(value(e.field->value));
                emit(";");
                endLine();
                lastWasField = true;
            } else if (e.kind == BodyKind::PinMap) {
                flushCommentsBefore(e.pin->span.offset);
                // A blank line separates the field block from the pin map, the
                // shape every part in spec 20 is written in.
                if (lastWasField && !first) blankLine();
                lastWasField = false;
                indent();
                std::string spec = pinSpec(e.pin);
                emit(spec);
                emit(std::string(pinWidth - spec.size() + 1, ' '));
                emit("= ");
                emit(pinLogical(e.pin));
                emit(directives(e.pin->directives));
                emit(pinFields(e.pin->fields));
                emit(";");
                endLine();
            }
            first = false;
        }
    }

    void genericBody(const Item* it) {
        for (const BodyEntry& e : it->body) {
            switch (e.kind) {
                case BodyKind::Item:
                    flushCommentsBefore(e.item->span.offset);
                    item(e.item);
                    break;
                case BodyKind::Stmt:
                    statement(e.stmt);
                    break;
                case BodyKind::Field:
                    flushCommentsBefore(e.field->span.offset);
                    indent();
                    emit(field(e.field));
                    emit(";");
                    endLine();
                    break;
                case BodyKind::Member:
                    memberDecl(e.member);
                    break;
                case BodyKind::Directive:
                    flushCommentsBefore(e.directive->span.offset);
                    indent();
                    emit(directive(e.directive));
                    emit(";");
                    endLine();
                    break;
                case BodyKind::PinMap:
                    break;
            }
        }
    }

    const TokenStream& tokens_;
    const SourceFile& file_;
    const StringInterner& in_;
    FormatOptions opt_;

    std::string out_;
    std::string line_;
    std::size_t depth_ = 0;
    std::size_t commentIndex_ = 0;
};

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            std::string_view line = text.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back(line);
            start = i + 1;
        }
    }
    if (start < text.size()) lines.push_back(text.substr(start));
    return lines;
}

}  // namespace

std::string formatUnit(const SourceUnit& unit, const TokenStream& tokens, const SourceFile& file,
                       const StringInterner& interner, const FormatOptions& options) {
    Printer printer(tokens, file, interner, options);
    return printer.run(unit);
}

// A unified diff over whole lines. The longest-common-subsequence table is
// bounded so that a pathological input degrades to "replace everything" rather
// than to quadratic memory.
std::string unifiedDiff(std::string_view before, std::string_view after, std::string_view path) {
    auto a = splitLines(before);
    auto b = splitLines(after);

    std::string out;
    if (a == b) return out;

    out += std::format("--- {}\n+++ {}\n", path, path);

    constexpr std::size_t kMaxCells = 4u * 1000u * 1000u;
    if (a.size() * b.size() > kMaxCells) {
        out += std::format("@@ -1,{} +1,{} @@\n", a.size(), b.size());
        for (auto line : a) { out += '-'; out += line; out += '\n'; }
        for (auto line : b) { out += '+'; out += line; out += '\n'; }
        return out;
    }

    // Classic LCS, then walk it back into a hunk list.
    std::vector<std::vector<std::uint32_t>> lcs(a.size() + 1,
                                                std::vector<std::uint32_t>(b.size() + 1, 0));
    for (std::size_t i = a.size(); i-- > 0;) {
        for (std::size_t j = b.size(); j-- > 0;) {
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1
                                     : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }

    struct Edit {
        char kind;  // ' ', '-', '+'
        std::string_view line;
    };
    std::vector<Edit> edits;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            edits.push_back({' ', a[i]});
            ++i;
            ++j;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            edits.push_back({'-', a[i++]});
        } else {
            edits.push_back({'+', b[j++]});
        }
    }
    while (i < a.size()) edits.push_back({'-', a[i++]});
    while (j < b.size()) edits.push_back({'+', b[j++]});

    // Group into hunks with three lines of context.
    constexpr std::size_t kContext = 3;
    std::size_t index = 0;
    while (index < edits.size()) {
        if (edits[index].kind == ' ') {
            ++index;
            continue;
        }
        std::size_t start = index > kContext ? index - kContext : 0;
        std::size_t stop = index;
        while (stop < edits.size()) {
            std::size_t run = 0;
            std::size_t probe = stop;
            while (probe < edits.size() && edits[probe].kind == ' ' && run < kContext * 2) {
                ++probe;
                ++run;
            }
            if (run >= kContext * 2 || probe >= edits.size()) break;
            stop = probe + 1;
        }
        stop = std::min(stop + kContext, edits.size());

        std::size_t aStart = 0, bStart = 0;
        for (std::size_t k = 0; k < start; ++k) {
            if (edits[k].kind != '+') ++aStart;
            if (edits[k].kind != '-') ++bStart;
        }
        std::size_t aCount = 0, bCount = 0;
        for (std::size_t k = start; k < stop; ++k) {
            if (edits[k].kind != '+') ++aCount;
            if (edits[k].kind != '-') ++bCount;
        }

        out += std::format("@@ -{},{} +{},{} @@\n", aStart + 1, aCount, bStart + 1, bCount);
        for (std::size_t k = start; k < stop; ++k) {
            out += edits[k].kind;
            out += edits[k].line;
            out += '\n';
        }
        index = stop;
    }

    return out;
}

}  // namespace manta
