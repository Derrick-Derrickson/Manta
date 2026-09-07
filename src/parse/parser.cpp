// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "parse/parser.h"

#include <format>
#include <string>

#include "lex/dimensioned.h"

namespace manta {

std::string_view binOpText(BinOp op) noexcept {
    switch (op) {
        case BinOp::Pow: return "^";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Lt: return "<";
        case BinOp::Gt: return ">";
        case BinOp::Le: return "<=";
        case BinOp::Ge: return ">=";
        case BinOp::Eq: return "=";
        case BinOp::Ne: return "!=";
        case BinOp::And: return "&";
        case BinOp::Xor: return "~";
        case BinOp::Or: return "|";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Errors and recovery
// ---------------------------------------------------------------------------

void Parser::error(Span at, std::string message) {
    if (errorBudget_-- <= 0) return;
    diags_.report(DiagId::Syntax, at, message);
}

bool Parser::expect(TokenKind k, std::string_view context) {
    if (at(k)) {
        ++pos_;
        return true;
    }
    error(here(), std::format("expected {} {}, found {}", tokenKindName(k), context,
                              tokenKindName(cur().kind)));
    return false;
}

// Skips to just past the next ';' at the current brace depth, so one bad
// statement does not swallow the declarations after it.
void Parser::recoverToStatementEnd() {
    int depth = 0;
    while (!atEnd()) {
        TokenKind k = cur().kind;
        if (k == TokenKind::LBrace || k == TokenKind::LParen || k == TokenKind::LBracket) ++depth;
        if (k == TokenKind::RBrace && depth == 0) return;  // let the body loop close it
        // A section marker starts a fresh section; do not swallow it with the
        // bad statement. The body loop consumes it.
        if (k == TokenKind::SectionMarker && depth <= 0) return;
        if (k == TokenKind::RBrace || k == TokenKind::RParen || k == TokenKind::RBracket) --depth;
        ++pos_;
        if (k == TokenKind::Semi && depth <= 0) return;
    }
}

int Parser::braceDepth() {
    while (depthCursor_ < pos_) {
        TokenKind k = toks_.at(depthCursor_).kind;
        if (k == TokenKind::LBrace) ++depthAtCursor_;
        else if (k == TokenKind::RBrace) --depthAtCursor_;
        ++depthCursor_;
    }
    return depthAtCursor_;
}

void Parser::resyncToDepth(int depth) {
    if (braceDepth() <= depth) return;
    while (!atEnd() && braceDepth() > depth) advance();
    // The '}' just consumed closed the construct the failed entry left open,
    // and the ';' terminating the statement that construct belonged to goes
    // with it. A body loop is entitled to start on a fresh entry.
    accept(TokenKind::Semi);
}

void Parser::recoverToDeclEnd() {
    int depth = 0;
    while (!atEnd()) {
        TokenKind k = advance().kind;
        if (k == TokenKind::LBrace) ++depth;
        if (k == TokenKind::RBrace) {
            --depth;
            if (depth <= 0) {
                accept(TokenKind::Semi);
                return;
            }
        }
    }
}

void Parser::checkTrailingDash(const Token& t) {
    if (t.has(WordFlags::TrailingDash)) {
        // Spec 2.3: "It shall not be the last character." A lexical rule, so a
        // syntax error; before 2.0 it shared E-02 with the no-driver rule.
        diags_.report(DiagId::Syntax, span(t),
                      std::format("identifier '{}' ends in '-'", text(t)));
    }
}

// ---------------------------------------------------------------------------
// Lookahead
// ---------------------------------------------------------------------------

bool Parser::atItemStart() const {
    if (!at(TokenKind::Word)) return false;
    std::string_view t = curText();
    if (t == "static") return true;
    return t == "block" || t == "part" || t == "harness" || t == "netclass" ||
           t == "match" || t == "cable";
}

// "[[ ... ]]" or "[N[ ... ]M]" (spec 8.3).
bool Parser::looksLikeReplication() const {
    if (!at(TokenKind::LBracket)) return false;
    if (ahead(1).kind == TokenKind::LBracket) return true;
    return ahead(1).kind == TokenKind::Word && ahead(1).has(WordFlags::Integer) &&
           ahead(2).kind == TokenKind::LBracket;
}

// "[A,K].{D1}": a pin list attached to a device open. Scans the bracket to
// its match and asks what follows (spec 7.3); the undotted pre-1.6 form is
// still recognised so parseTerminal can say what is missing.
bool Parser::looksLikePinList() const {
    if (!at(TokenKind::LBracket)) return false;
    std::size_t i = pos_ + 1;
    int depth = 1;
    while (depth > 0 && toks_.at(i).kind != TokenKind::Eof) {
        if (toks_.at(i).kind == TokenKind::LBracket) ++depth;
        if (toks_.at(i).kind == TokenKind::RBracket) --depth;
        ++i;
    }
    if (toks_.at(i).kind == TokenKind::Dot) ++i;
    return toks_.at(i).kind == TokenKind::LBrace;
}

// "[3V3, GND]>>;" (spec 10.3). Anything else beginning with '[' inside a block
// body is a replication or a pin-list device.
bool Parser::looksLikePortList() const {
    return at(TokenKind::LBracket) && !looksLikeReplication() && !looksLikePinList();
}

bool Parser::looksLikeFieldDecl() const {
    if (at(TokenKind::Hash) || at(TokenKind::At)) return true;
    // An import is written ">#~source" (spec 9.4); a port is ">SIG-IN".
    return at(TokenKind::Gt) &&
           (ahead(1).kind == TokenKind::Hash || ahead(1).kind == TokenKind::At);
}

// A chain element is a device when a '{' follows an optional terminal
// (spec 7.1). The terminal is '.' or an identifier with an optional index.
bool Parser::looksLikeDevice() const {
    if (at(TokenKind::LBrace)) return true;
    if (at(TokenKind::Dot)) {
        // A run of dots is one terminal (spec 7.3): "..{R1}".
        std::size_t i = pos_;
        while (toks_.at(i).kind == TokenKind::Dot) ++i;
        return toks_.at(i).kind == TokenKind::LBrace;
    }
    if (at(TokenKind::LBracket)) return looksLikePinList();
    if (!at(TokenKind::Word)) return false;

    std::size_t i = pos_ + 1;
    if (toks_.at(i).kind == TokenKind::LBracket) {
        // Skip a bracketed index: "GPIO[1].{...}" is a terminal with a range,
        // so scan to the matching ']'.
        int depth = 1;
        ++i;
        while (depth > 0 && toks_.at(i).kind != TokenKind::Eof) {
            if (toks_.at(i).kind == TokenKind::LBracket) ++depth;
            if (toks_.at(i).kind == TokenKind::RBracket) --depth;
            ++i;
        }
    }
    // The attachment dot of "A.{D1}" (spec 7.3). The undotted pre-1.6 form is
    // still recognised so parseTerminal can say what is missing.
    if (toks_.at(i).kind == TokenKind::Dot) ++i;
    return toks_.at(i).kind == TokenKind::LBrace;
}

// ---------------------------------------------------------------------------
// Names, values and substitution
// ---------------------------------------------------------------------------

// Collects a maximal run of abutting Word and "$...$" tokens into chunks.
// Spec 14.2 places substitution "inside an identifier", so "R?~$val$-0603" is
// one name made of an expression and a literal tail.
InterpText* Parser::parseInterpRun(std::vector<InterpChunk>& chunks, Span start) {
    Span full = start;
    for (;;) {
        if (at(TokenKind::Word)) {
            const Token& t = advance();
            chunks.push_back(InterpChunk{false, intern(text(t)), nullptr, span(t)});
            full = full.merge(span(t));
        } else if (at(TokenKind::Dollar)) {
            Span dollarStart = here();
            Expr* e = parseInterpolation();
            Span whole = dollarStart.merge(toks_.at(pos_ - 1).span(file_.id()));
            chunks.push_back(InterpChunk{true, SymbolId::kInvalid, e, whole});
            full = full.merge(whole);
        } else {
            break;
        }
        // Only abutting tokens continue the run; a space ends the identifier.
        if (pos_ == 0 || !adjacent(pos_ - 1)) break;
        if (!at(TokenKind::Word) && !at(TokenKind::Dollar)) break;
    }

    auto* it = arena_.make<InterpText>();
    it->chunks = commit(chunks);
    it->span = full;
    return it;
}

Name Parser::parseName(bool identifierPosition) {
    Name n;
    n.span = here();

    if (!at(TokenKind::Word) && !at(TokenKind::Dollar)) {
        error(here(), std::format("expected a name, found {}", tokenKindName(cur().kind)));
        return n;
    }

    // The common case by far: a single word with no substitution.
    if (at(TokenKind::Word) &&
        !(adjacent(pos_) && (ahead(1).kind == TokenKind::Dollar))) {
        const Token& t = advance();
        if (identifierPosition) checkTrailingDash(t);
        n.symbol = intern(text(t));
        n.span = span(t);
        return n;
    }

    std::vector<InterpChunk> chunks;
    n.interp = parseInterpRun(chunks, here());
    n.span = n.interp->span;
    return n;
}

Index Parser::parseIndex() {
    Index idx;
    idx.span = here();
    if (at(TokenKind::Dollar)) {
        idx.isExpr = true;
        idx.expr = parseInterpolation();
        idx.span = idx.span.merge(toks_.at(pos_ - 1).span(file_.id()));
        return idx;
    }
    if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
        const Token& t = advance();
        idx.literal = integerOf(t);
        idx.span = span(t);
        return idx;
    }
    error(here(), "expected an array index");
    return idx;
}

Range Parser::parseRange() {
    Range r;
    r.span = here();
    if (!accept(TokenKind::LBracket)) return r;
    r.lo = parseIndex();
    if (accept(TokenKind::Colon)) {
        r.hi = parseIndex();
    } else {
        // A single index selects one wire: "GPIO[1]", "GPIO[$n$]" (spec 14.6).
        r.single = true;
        r.hi = r.lo;
    }
    expect(TokenKind::RBracket, "closing an array range");
    r.present = true;
    r.span = r.span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return r;
}

Value* Parser::parseListValue() {
    auto* v = arena_.make<Value>();
    v->kind = ValueKind::List;
    v->span = here();
    expect(TokenKind::LBracket, "opening a list");
    std::vector<Value*> items;
    if (!at(TokenKind::RBracket)) {
        do {
            Value* item = parseValue();
            // "1:20" inside a list is a range, so a twenty-way pin map is one
            // pair rather than twenty. Only inside a list: elsewhere ':' is the
            // binding separator and has nothing to do with values.
            if (item && item->kind == ValueKind::Integer && at(TokenKind::Colon)) {
                advance();
                Value* hi = parseValue();
                if (hi && hi->kind == ValueKind::Integer) {
                    auto* range = arena_.make<Value>();
                    range->kind = ValueKind::Range;
                    range->rangeLo = item->num.mantissa;
                    range->rangeHi = hi->num.mantissa;
                    range->span = item->span.merge(hi->span);
                    item = range;
                } else {
                    error(hi ? hi->span : here(), "expected a whole number ending a range");
                }
            }
            items.push_back(item);
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RBracket, "closing a list");
    v->list = commit(items);
    v->span = v->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return v;
}

Value* Parser::parseValue() {
    auto* v = arena_.make<Value>();
    v->span = here();

    // "&NET=?" unbinds a pin (spec 11.6).
    if (accept(TokenKind::Question)) {
        v->kind = ValueKind::Unbind;
        return v;
    }

    if (at(TokenKind::LBracket)) return parseListValue();

    // "(5ps)*3": an element-wise tolerance list (spec 11.4).
    if (at(TokenKind::LParen)) {
        advance();
        Value* inner = parseValue();
        expect(TokenKind::RParen, "closing a repeated value");
        v->kind = ValueKind::Repeat;
        v->inner = inner;
        if (accept(TokenKind::Star)) {
            if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
                v->count = integerOf(cur());
                advance();
            } else {
                error(here(), "expected a repeat count after '*'");
            }
        } else {
            error(here(), "expected '*' and a count after a parenthesised value");
        }
        v->span = v->span.merge(toks_.at(pos_ - 1).span(file_.id()));
        return v;
    }

    // "±1%" or "+-1%" (spec 3.3).
    if (accept(TokenKind::PlusMinus)) {
        v->kind = ValueKind::Tolerance;
        if (at(TokenKind::Word)) {
            const Token& t = advance();
            Dimensioned d;
            (void)parseDimensioned(text(t), d);
            v->num = d;
            if (at(TokenKind::Percent) && adjacent(pos_ - 1)) {
                advance();
                v->num.unit = Unit::Percent;
            }
        } else {
            error(here(), "expected a number after '±'");
        }
        v->span = v->span.merge(toks_.at(pos_ - 1).span(file_.id()));
        return v;
    }

    if (at(TokenKind::String)) {
        const Token& t = advance();
        v->kind = ValueKind::String;
        // Strip the quotes and resolve the escapes of spec 3.5.
        std::string_view raw = text(t);
        std::string unescaped;
        if (raw.size() >= 2) {
            std::string_view body = raw.substr(1, raw.size() - 2);
            unescaped.reserve(body.size());
            for (std::size_t i = 0; i < body.size(); ++i) {
                if (body[i] == '\\' && i + 1 < body.size()) {
                    switch (body[i + 1]) {
                        case 'n': unescaped += '\n'; ++i; continue;
                        case 't': unescaped += '\t'; ++i; continue;
                        case '"': unescaped += '"'; ++i; continue;
                        case '\\': unescaped += '\\'; ++i; continue;
                        default: break;
                    }
                }
                unescaped += body[i];
            }
        }
        v->text = intern(unescaped);
        v->span = span(t);
        return v;
    }

    // A run that begins with, or contains, a substitution.
    if (at(TokenKind::Dollar) ||
        (at(TokenKind::Word) && adjacent(pos_) && ahead(1).kind == TokenKind::Dollar)) {
        std::vector<InterpChunk> chunks;
        v->kind = ValueKind::Interp;
        v->interp = parseInterpRun(chunks, here());
        v->span = v->interp->span;
        return v;
    }

    if (!at(TokenKind::Word)) {
        error(here(), std::format("expected a value, found {}", tokenKindName(cur().kind)));
        return v;
    }

    const Token& t = advance();
    std::string_view lex = text(t);
    v->span = span(t);
    v->upperCaseSpelling = t.has(WordFlags::UpperCase);
    // Every word-derived value keeps its lexeme. A directive whose value type is
    // a *net name* -- "&~NET=3V3" (spec 11.6) -- must read "3V3" as the name of
    // a rail, not as 3.3 volts, and only the directive's declared type says
    // which reading applies. Same word, two meanings, resolved by position.
    v->text = intern(lex);

    // Spec 4.3 version constraints. A trailing '-' lives inside the word,
    // whereas a trailing '+' is a separate token.
    if (t.has(WordFlags::VersionSpec)) {
        bool plus = at(TokenKind::Plus) && adjacent(pos_ - 1);
        if (plus) advance();
        auto parseRev = [](std::string_view s, std::uint32_t& maj, std::uint32_t& min) {
            std::size_t dot = s.find('.');
            if (dot == std::string_view::npos) return false;
            maj = 0;
            min = 0;
            for (std::size_t i = 0; i < dot; ++i) maj = maj * 10 + static_cast<std::uint32_t>(s[i] - '0');
            for (std::size_t i = dot + 1; i < s.size(); ++i)
                min = min * 10 + static_cast<std::uint32_t>(s[i] - '0');
            return true;
        };

        VersionConstraint vc;
        std::string_view body = lex;
        if (!body.empty() && body.back() == '-') {
            // "1.2-": revision 1.2 or earlier.
            parseRev(body.substr(0, body.size() - 1), vc.hiMajor, vc.hiMinor);
            vc.hasHi = true;
        } else {
            std::size_t sep = std::string_view::npos;
            for (std::size_t i = 1; i + 1 < body.size(); ++i) {
                if (body[i] == '-') { sep = i; break; }
            }
            if (sep != std::string_view::npos) {
                // "0.2-1.2": an inclusive range.
                parseRev(body.substr(0, sep), vc.loMajor, vc.loMinor);
                parseRev(body.substr(sep + 1), vc.hiMajor, vc.hiMinor);
                vc.hasLo = vc.hasHi = true;
            } else {
                parseRev(body, vc.loMajor, vc.loMinor);
                vc.hasLo = true;
                if (!plus) {
                    // A bare revision pins exactly that one.
                    vc.hiMajor = vc.loMajor;
                    vc.hiMinor = vc.loMinor;
                    vc.hasHi = true;
                }
            }
        }
        // Only treat it as a version when it cannot be an ordinary value; a
        // bare "1.0" is a decimal unless the field wants a constraint. The
        // resolution happens in sema, which knows the field; both readings are
        // recorded here.
        v->kind = ValueKind::Version;
        v->version = vc;
        v->text = intern(lex);
        Dimensioned d;
        if (parseDimensioned(lex, d)) v->num = d;
        v->span = v->span.merge(toks_.at(pos_ - 1).span(file_.id()));
        return v;
    }

    if (t.has(WordFlags::BoolTrue) || t.has(WordFlags::BoolFalse)) {
        v->kind = ValueKind::Boolean;
        v->boolean = t.has(WordFlags::BoolTrue);
        v->text = intern(lex);
        return v;
    }

    if (t.has(WordFlags::Dimensioned)) {
        v->kind = ValueKind::Dimensioned;
        Dimensioned d;
        (void)parseDimensioned(lex, d);
        v->num = d;
        return v;
    }

    if (t.has(WordFlags::Integer) || t.has(WordFlags::Decimal)) {
        Dimensioned d;
        (void)parseDimensioned(lex, d);
        v->num = d;
        v->kind = t.has(WordFlags::Integer) ? ValueKind::Integer : ValueKind::Decimal;
        // A '%' abutting a number is a percentage; a '%' *leading* a token is
        // the per-copy selector, so the two never collide (spec 3.3).
        if (at(TokenKind::Percent) && adjacent(pos_ - 1)) {
            advance();
            v->kind = ValueKind::Percentage;
            v->num.unit = Unit::Percent;
            v->span = v->span.merge(toks_.at(pos_ - 1).span(file_.id()));
        }
        return v;
    }

    // Spec 3.5: "A bare word is accepted where it contains no whitespace and
    // lexes as an identifier."
    v->kind = ValueKind::Identifier;
    v->text = intern(lex);
    // Imperial literals are recognised as such so that E-17 can be reported
    // against them rather than a baffling "not a value" (spec 3.4).
    if (t.has(WordFlags::Imperial)) {
        diags_.report(DiagId::E17, span(t), lex);
    }
    return v;
}

// ---------------------------------------------------------------------------
// Substitution expressions (spec 14.3)
// ---------------------------------------------------------------------------

namespace {

struct OpInfo {
    BinOp op;
    int precedence;  // higher binds tighter
    bool rightAssoc;
};

// Spec 14.3, tightest first: ^ then * / then + - then relations then = != then
// & then ~ then |.
bool binOpFor(TokenKind k, OpInfo& out) {
    switch (k) {
        case TokenKind::Caret: out = {BinOp::Pow, 8, true}; return true;
        case TokenKind::Star: out = {BinOp::Mul, 7, false}; return true;
        case TokenKind::Slash: out = {BinOp::Div, 7, false}; return true;
        case TokenKind::Plus: out = {BinOp::Add, 6, false}; return true;
        case TokenKind::Minus: out = {BinOp::Sub, 6, false}; return true;
        case TokenKind::Lt: out = {BinOp::Lt, 5, false}; return true;
        case TokenKind::Gt: out = {BinOp::Gt, 5, false}; return true;
        case TokenKind::Le: out = {BinOp::Le, 5, false}; return true;
        case TokenKind::Ge: out = {BinOp::Ge, 5, false}; return true;
        case TokenKind::Eq: out = {BinOp::Eq, 4, false}; return true;
        case TokenKind::BangEq: out = {BinOp::Ne, 4, false}; return true;
        case TokenKind::Amp: out = {BinOp::And, 3, false}; return true;
        case TokenKind::Tilde: out = {BinOp::Xor, 2, false}; return true;
        case TokenKind::Pipe: out = {BinOp::Or, 1, false}; return true;
        default: return false;
    }
}

}  // namespace

Expr* Parser::parseAtom() {
    auto* e = arena_.make<Expr>();
    e->span = here();

    if (accept(TokenKind::LParen)) {
        Expr* inner = parseExpr();
        expect(TokenKind::RParen, "closing a substitution group");
        return inner;
    }

    if (at(TokenKind::String)) {
        // Spec 14.5: a field whose name contains a hyphen is referenced by
        // quoting it, because '-' inside "$...$" is always subtraction.
        const Token& t = advance();
        std::string_view raw = text(t);
        e->kind = ExprKind::FieldRef;
        e->field = intern(raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : raw);
        e->span = span(t);
        return e;
    }

    if (at(TokenKind::Word)) {
        const Token& t = advance();
        e->span = span(t);
        if (t.has(WordFlags::BoolTrue) || t.has(WordFlags::BoolFalse)) {
            e->kind = ExprKind::BoolLit;
            e->boolVal = t.has(WordFlags::BoolTrue);
            return e;
        }
        if (t.has(WordFlags::Integer)) {
            e->kind = ExprKind::IntLit;
            e->intVal = integerOf(t);
            return e;
        }
        e->kind = ExprKind::FieldRef;
        e->field = intern(text(t));
        return e;
    }

    error(here(), std::format("expected an operand in a substitution, found {}",
                              tokenKindName(cur().kind)));
    e->kind = ExprKind::IntLit;
    return e;
}

Expr* Parser::parseUnary() {
    if (at(TokenKind::Minus) || at(TokenKind::Bang)) {
        auto* e = arena_.make<Expr>();
        e->kind = ExprKind::Unary;
        e->span = here();
        e->unOp = at(TokenKind::Minus) ? UnOp::Neg : UnOp::Not;
        advance();
        e->lhs = parseUnary();
        e->span = e->span.merge(e->lhs->span);
        return e;
    }
    return parseAtom();
}

Expr* Parser::parseBinary(int minPrecedence) {
    Expr* lhs = parseUnary();
    for (;;) {
        OpInfo info{};
        if (!binOpFor(cur().kind, info)) break;
        if (info.precedence < minPrecedence) break;
        advance();
        Expr* rhs = parseBinary(info.rightAssoc ? info.precedence : info.precedence + 1);
        auto* node = arena_.make<Expr>();
        node->kind = ExprKind::Binary;
        node->binOp = info.op;
        node->lhs = lhs;
        node->rhs = rhs;
        node->span = lhs->span.merge(rhs->span);
        lhs = node;
    }
    return lhs;
}

Expr* Parser::parseExpr() { return parseBinary(1); }

Expr* Parser::parseInterpolation() {
    expect(TokenKind::Dollar, "opening a substitution");
    Expr* e = parseExpr();
    // Spec 14.2: "The closing '$' is required, since substitution is most
    // useful inside an identifier and a bare $NAME cannot be delimited."
    expect(TokenKind::Dollar, "closing a substitution");
    return e;
}

// ---------------------------------------------------------------------------
// Fields and directives
// ---------------------------------------------------------------------------

FieldDecl* Parser::parseFieldDecl() {
    auto* f = arena_.make<FieldDecl>();
    f->span = here();

    // Canonical order is direction, sigil, strength for an import (">#~") and
    // sigil, strength, direction for an export ("#!>"). Spec 9.4 says the
    // formatter rewrites any other ordering, so the parser accepts both.
    bool leadingArrow = accept(TokenKind::Gt);

    if (accept(TokenKind::At)) f->ns = FieldNamespace::System;
    else if (accept(TokenKind::Hash)) f->ns = FieldNamespace::User;
    else error(here(), "expected '#' or '@' to introduce a field");

    if (accept(TokenKind::Tilde)) f->strength = Strength::Weak;
    else if (accept(TokenKind::Bang)) f->strength = Strength::Locked;

    // Canonical export order puts the arrow *before* the name -- "#!>board-rev"
    // -- while an import puts it before the sigil. Both spellings of each are
    // accepted; the formatter emits the canonical one.
    bool exportArrowBeforeName = accept(TokenKind::Gt);

    f->name = parseName(true);

    bool trailingArrow = accept(TokenKind::Gt) || exportArrowBeforeName;

    if (leadingArrow && trailingArrow) {
        error(f->span, "a field is either imported or exported, not both");
        f->direction = FieldDirection::Import;
    } else if (leadingArrow) {
        f->direction = FieldDirection::Import;
    } else if (trailingArrow) {
        f->direction = FieldDirection::Export;
    }

    if (expect(TokenKind::Eq, "in a field declaration")) f->value = parseValue();
    f->span = f->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return f;
}

Directive* Parser::parseDirective() {
    auto* d = arena_.make<Directive>();
    d->span = here();
    expect(TokenKind::Amp, "introducing a directive");

    if (accept(TokenKind::Tilde)) d->strength = Strength::Weak;
    else if (accept(TokenKind::Bang)) d->strength = Strength::Locked;

    d->name = parseName(true);

    if (accept(TokenKind::Eq)) {
        // "&MATCH={ddr-addr: @!offset=10ps}" (spec 11.4).
        if (at(TokenKind::LBrace)) {
            auto* ref = arena_.make<MatchRef>();
            ref->span = here();
            advance();
            ref->group = parseName(true);
            std::vector<FieldDecl*> overrides;
            if (accept(TokenKind::Colon)) {
                while (!at(TokenKind::RBrace) && !atEnd()) {
                    overrides.push_back(parseFieldDecl());
                    if (!accept(TokenKind::Semi)) break;
                }
            }
            expect(TokenKind::RBrace, "closing a match reference");
            ref->overrides = commit(overrides);
            ref->span = ref->span.merge(toks_.at(pos_ - 1).span(file_.id()));
            d->matchRef = ref;
        } else {
            d->value = parseValue();
        }
    }

    d->span = d->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return d;
}

// ---------------------------------------------------------------------------
// Ports and net expressions
// ---------------------------------------------------------------------------

PortSpec Parser::parseLeadingArrow() {
    PortSpec p;
    p.leading = true;
    p.span = here();
    // Spec 10.1: the arrow points toward the identifier for an input and away
    // from it for an output, so a *leading* '>' is an input and '<' an output.
    if (accept(TokenKind::GtGt)) {
        p.dir = PortDir::In;
        p.global = true;
    } else if (accept(TokenKind::LtGt)) {
        p.dir = PortDir::Bidir;
    } else if (accept(TokenKind::Gt)) {
        p.dir = PortDir::In;
    } else if (accept(TokenKind::Lt)) {
        p.dir = PortDir::Out;
    }
    return p;
}

PortSpec Parser::parseTrailingArrow() {
    PortSpec p;
    p.leading = false;
    p.span = here();
    if (accept(TokenKind::GtGt)) {
        p.dir = PortDir::Out;
        p.global = true;
    } else if (accept(TokenKind::LtGt)) {
        p.dir = PortDir::Bidir;
    } else if (accept(TokenKind::Gt)) {
        p.dir = PortDir::Out;
    } else if (accept(TokenKind::Lt)) {
        p.dir = PortDir::In;
    }
    return p;
}

NetExpr* Parser::parseNetExpr() {
    auto* n = arena_.make<NetExpr>();
    n->span = here();
    n->leading = parseLeadingArrow();

    // "%NAME[range]" and "%[a,b,c]" supply a distinct value per copy (spec 8.5).
    if (accept(TokenKind::Percent)) {
        n->perCopy = true;
        if (at(TokenKind::LBracket)) {
            advance();
            std::vector<Value*> items;
            if (!at(TokenKind::RBracket)) {
                do {
                    items.push_back(parseValue());
                } while (accept(TokenKind::Comma));
            }
            expect(TokenKind::RBracket, "closing a per-copy list");
            n->perCopyList = commit(items);
            n->hasPerCopyList = true;
            n->span = n->span.merge(toks_.at(pos_ - 1).span(file_.id()));
            return n;
        }
    }

    std::vector<Name> path;
    path.push_back(parseName(true));

    while (at(TokenKind::Dot)) {
        // "USB.[+,-]" selects members in the order written (spec 12.2).
        if (ahead(1).kind == TokenKind::LBracket) {
            advance();  // '.'
            advance();  // '['
            std::vector<Name> members;
            if (!at(TokenKind::RBracket)) {
                do {
                    Name m;
                    m.span = here();
                    // '+' and '-' are member names in the harness namespace and
                    // do not collide with the leading-hyphen identifier rule
                    // (spec 12.4).
                    if (at(TokenKind::Plus)) { advance(); m.symbol = intern("+"); }
                    else if (at(TokenKind::Minus)) { advance(); m.symbol = intern("-"); }
                    else m = parseName(true);
                    members.push_back(m);
                } while (accept(TokenKind::Comma));
            }
            expect(TokenKind::RBracket, "closing a harness member list");
            n->memberList = commit(members);
            n->hasMemberList = true;
            break;
        }
        advance();  // '.'
        Name m;
        m.span = here();
        if (at(TokenKind::Plus)) { advance(); m.symbol = intern("+"); }
        else if (at(TokenKind::Minus)) { advance(); m.symbol = intern("-"); }
        else m = parseName(true);
        path.push_back(m);
    }

    n->path = commit(path);
    if (at(TokenKind::LBracket)) n->range = parseRange();
    n->trailing = parseTrailingArrow();
    n->span = n->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return n;
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

Terminal Parser::parseTerminal(bool entrySide) {
    Terminal t;
    t.span = here();

    // The exit side leads with the attachment dot: "}.K", "}.[3,4]", "}."
    // (casual), "}.." (a casual run). Spec 7.3, revision 1.6: a terminal
    // always touches its braces through '.'.
    if (!entrySide) {
        expect(TokenKind::Dot, "attaching an exit terminal; a pin of the device is '.PIN'");
        if (at(TokenKind::Dot)) {
            // More dots: a casual run, the first dot included.
            t.dot = true;
            t.dotCount = 1;
            while (accept(TokenKind::Dot)) ++t.dotCount;
        } else if (at(TokenKind::Word) && adjacent(pos_ - 1)) {
            t.name = parseName(true);
            if (at(TokenKind::LBracket)) t.range = parseRange();
        } else if (at(TokenKind::LBracket) && adjacent(pos_ - 1)) {
            advance();
            std::vector<Name> pins;
            do {
                pins.push_back(parseName(true));
            } while (accept(TokenKind::Comma));
            expect(TokenKind::RBracket, "closing a pin list");
            t.list = commit(pins);
            t.hasList = true;
        } else {
            t.dot = true;
            t.dotCount = 1;
        }
        t.span = t.span.merge(toks_.at(pos_ - 1).span(file_.id()));
        return t;
    }

    if (at(TokenKind::Dot)) {
        // Spec 7.3: '.' takes the first unassigned casual pin from those
        // remaining. A run of dots takes that many, all onto one node --
        // "..{R1}" is the written form of a deliberate short.
        t.dot = true;
        while (accept(TokenKind::Dot)) ++t.dotCount;
        t.span = t.span.merge(toks_.at(pos_ - 1).span(file_.id()));
        return t;
    }
    if (at(TokenKind::LBracket)) {
        // Spec 7.3: "[A,K].{D1}" -- a pin list as one terminal. Every listed
        // pin joins the one node; a *range* ("O[0:1]") is a bus and needs a
        // name first, so the bare bracket is unambiguous here.
        advance();
        std::vector<Name> pins;
        do {
            pins.push_back(parseName(true));
        } while (accept(TokenKind::Comma));
        expect(TokenKind::RBracket, "closing a pin list");
        t.list = commit(pins);
        t.hasList = true;
    } else {
        t.name = parseName(true);
        if (at(TokenKind::LBracket)) t.range = parseRange();
    }
    // A named or listed entry terminal attaches through a dot: "A.{D1}".
    expect(TokenKind::Dot, "attaching the terminal to its device; a pin is 'PIN.{...}'");
    t.span = t.span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return t;
}

Designator Parser::parseDesignator() {
    Designator d;
    d.span = here();

    if (!at(TokenKind::Word)) {
        error(here(), "expected a designator");
        return d;
    }

    const Token& t = advance();
    std::string_view lex = text(t);
    d.span = span(t);

    // Split a trailing run of digits: "U7" is prefix "U" and number 7. The
    // assignment span is exactly those digits, because "manta annotate"
    // rewrites precisely that byte range and nothing else (spec 13.7).
    std::size_t digitStart = lex.size();
    while (digitStart > 0 && lex[digitStart - 1] >= '0' && lex[digitStart - 1] <= '9') --digitStart;

    if (digitStart < lex.size() && digitStart > 0) {
        d.prefix.symbol = intern(lex.substr(0, digitStart));
        d.prefix.span = span(t).sub(0, static_cast<std::uint32_t>(digitStart));
        d.kind = DesignatorKind::Numbered;
        std::int64_t n = 0;
        for (std::size_t i = digitStart; i < lex.size(); ++i) n = n * 10 + (lex[i] - '0');
        d.number = n;
        d.assignmentSpan = span(t).sub(static_cast<std::uint32_t>(digitStart),
                                       static_cast<std::uint32_t>(lex.size() - digitStart));
        return d;
    }

    d.prefix.symbol = intern(lex);
    d.prefix.span = span(t);

    if (at(TokenKind::Question)) {
        d.kind = DesignatorKind::Unassigned;
        d.assignmentSpan = here();
        advance();
        d.span = d.span.merge(d.assignmentSpan);
        return d;
    }

    if (at(TokenKind::Percent)) {
        // A range designator: one token carrying N designators (spec 13.3).
        Span start = here();
        advance();
        d.kind = DesignatorKind::Range;
        std::vector<DesigPart> parts;
        if (expect(TokenKind::LBracket, "opening a designator range")) {
            do {
                DesigPart p;
                if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
                    p.lo = p.hi = integerOf(cur());
                    advance();
                } else {
                    error(here(), "expected a number in a designator range");
                }
                if (accept(TokenKind::Colon)) {
                    if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
                        p.hi = integerOf(cur());
                        advance();
                    } else {
                        error(here(), "expected a number after ':' in a designator range");
                    }
                }
                parts.push_back(p);
            } while (accept(TokenKind::Comma));
            expect(TokenKind::RBracket, "closing a designator range");
        }
        d.parts = commit(parts);
        d.assignmentSpan = start.merge(toks_.at(pos_ - 1).span(file_.id()));
        d.span = d.span.merge(d.assignmentSpan);
        return d;
    }

    // No '?', no number, no range. Spec 19 requires one of the three.
    error(span(t), std::format(
        "designator '{}' has no number; write '{}?' to leave it for 'manta annotate'", lex, lex));
    d.kind = DesignatorKind::Unassigned;
    d.assignmentSpan = Span{span(t).file, span(t).end(), 0};
    return d;
}

Binding* Parser::parseBinding() {
    auto* b = arena_.make<Binding>();
    b->span = here();

    if (at(TokenKind::Amp)) {
        b->kind = BindingKind::Directive;
        b->directive = parseDirective();
        b->span = b->directive->span;
        return b;
    }

    if (looksLikeFieldDecl()) {
        b->kind = BindingKind::Field;
        b->field = parseFieldDecl();
        b->span = b->field->span;
        return b;
    }

    b->kind = BindingKind::PinNet;
    // Revision 1.6, spec 7.4: a binding names this instance's pin with a
    // leading '.', the blank left side meaning "this" -- ".VIN = VPOS;" is
    // U's own VIN, never a net. '.' alone stays the casual-pin binding.
    if (at(TokenKind::Dot)) {
        advance();
    } else {
        error(here(), "a pin binding opens with '.'; this instance's pin is '.PIN'");
    }
    if (at(TokenKind::Word)) {
        b->pin = parseName(true);
        if (at(TokenKind::LBracket)) b->pinRange = parseRange();
    } else {
        b->pinIsDot = true;
    }

    // Revision 1.4, spec 7.4: a binding is a chain rooted at a pin of the
    // enclosing instance, so "PIN <connector> <segment>" here means exactly
    // what "<designator>.PIN <connector> <segment>" means in the enclosing
    // body. The connectors are spec 6's four, the same set parseSegment()
    // joins elements with. None of them means the binding has no right-hand
    // side at all: the pin carries only directives or fields (spec 11.5).
    bool connected = true;
    if (at(TokenKind::EqEq)) b->connector = Connector::Same;
    else if (at(TokenKind::EqStar)) b->connector = Connector::Gather;
    else if (at(TokenKind::StarEq)) b->connector = Connector::Broadcast;
    else if (at(TokenKind::Eq)) b->connector = Connector::Advance;
    else connected = false;

    if (connected) {
        bool advancing = at(TokenKind::Eq);
        Span connSpan = here();
        advance();
        if (advancing && at(TokenKind::Question)) {
            // "GNDB=?" leaves the pin deliberately floating (spec 11.6). Only
            // '=' takes it: the other three connectors join runs of elements,
            // and there is no run to join to nothing.
            advance();
            b->unbind = true;
        } else {
            // The opening connector of a binding is part of the segment's
            // bracket accounting (spec 6.3): "FB == .{R3: .=5V; } == ..."
            // opens at the pin and closes after R3.
            Segment* seg = parseSegment(b->connector == Connector::Same, connSpan);
            // A lone net keeps the shape it had before revision 1.4, 'rhs'
            // null included, so every design written against 1.3 travels the
            // path it always travelled.
            if (b->connector == Connector::Advance && seg->elements.size() == 1 &&
                seg->elements[0]->kind == ElementKind::Net) {
                b->net = seg->elements[0]->net;
            } else {
                b->rhs = seg;
            }
        }
    }

    // A pin may carry directives or fields instead of, or as well as, a net:
    // spec 11.5 writes "{U5~ddr-chip: DQ[0] &PINDELAY=18ps; }", and a field
    // overrides what the part declared for that pin.
    std::vector<Directive*> dirs;
    std::vector<FieldDecl*> fields;
    for (;;) {
        if (at(TokenKind::Amp)) {
            dirs.push_back(parseDirective());
        } else if (looksLikeFieldDecl()) {
            fields.push_back(parseFieldDecl());
        } else {
            break;
        }
    }
    b->pinDirectives = commit(dirs);
    b->pinFields = commit(fields);

    b->span = b->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return b;
}

Instance* Parser::parseInstance() {
    auto* inst = arena_.make<Instance>();
    inst->span = here();

    // Spec 7.5: a '!' prefix marks the instance not fitted.
    if (accept(TokenKind::Bang)) inst->dnp = true;

    inst->designator = parseDesignator();

    // Spec 7.2: the presence of '~' distinguishes declaring a new instance from
    // referencing an existing one.
    if (accept(TokenKind::Tilde)) {
        inst->declares = true;
        inst->partOrBlock = parseName(true);
    }

    if (at(TokenKind::Colon)) {
        Span colon = here();
        advance();
        if (at(TokenKind::RBrace)) {
            // Spec 7.4: ".{L1~MT100UFA:}." is E-09.
            diags_.report(DiagId::E09, colon.merge(here()));
        } else {
            std::vector<Binding*> bindings;
            for (;;) {
                bindings.push_back(parseBinding());
                if (!accept(TokenKind::Semi)) break;
                // A trailing ';' before '}' is permitted, and is what the
                // formatter emits (spec 7.4).
                if (at(TokenKind::RBrace)) break;
            }
            inst->bindings = commit(bindings);
        }
    } else if (at(TokenKind::Semi) && ahead(1).kind == TokenKind::RBrace) {
        // ".{L1~MT100UFA;}." is also E-09.
        Span s = here();
        advance();
        diags_.report(DiagId::E09, s);
    }

    inst->span = inst->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return inst;
}

Device* Parser::parseDevice() {
    auto* dev = arena_.make<Device>();
    dev->span = here();

    // The terminals written outside the braces are the pins through which the
    // chain passes: the left is the entry, the right the exit (spec 7.3).
    if (!at(TokenKind::LBrace)) {
        dev->entry = parseTerminal(/*entrySide=*/true);
        dev->hasEntry = true;
    }

    expect(TokenKind::LBrace, "opening a device");
    dev->instance = parseInstance();
    expect(TokenKind::RBrace, "closing a device");

    // An exit terminal leads with the attachment dot -- "}.K" -- so the old
    // whitespace rule ("abuts the closing brace") is only needed to keep a
    // '.' on the next line from being read as this device's exit.
    if (at(TokenKind::Dot) && adjacent(pos_ - 1)) {
        dev->exit = parseTerminal(/*entrySide=*/false);
        dev->hasExit = true;
    } else if ((at(TokenKind::Word) || at(TokenKind::LBracket)) && adjacent(pos_ - 1)) {
        // The pre-1.6 undotted exit, recognised for the error's sake and
        // parsed as written -- no attachment dot to demand a second time.
        error(here(), "an exit terminal attaches through '.'; a pin of the device is '}.PIN'");
        Terminal t;
        t.span = here();
        if (at(TokenKind::LBracket)) {
            advance();
            std::vector<Name> pins;
            do {
                pins.push_back(parseName(true));
            } while (accept(TokenKind::Comma));
            expect(TokenKind::RBracket, "closing a pin list");
            t.list = commit(pins);
            t.hasList = true;
        } else {
            t.name = parseName(true);
            if (at(TokenKind::LBracket)) t.range = parseRange();
        }
        t.span = t.span.merge(toks_.at(pos_ - 1).span(file_.id()));
        dev->exit = t;
        dev->hasExit = true;
    }

    dev->span = dev->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return dev;
}

Group* Parser::parseGroup() {
    auto* g = arena_.make<Group>();
    g->span = here();
    expect(TokenKind::LParen, "opening a group");
    g->body = parseSegment();
    expect(TokenKind::RParen, "closing a group");

    // Multiplicity binds to a parenthesised group (spec 8.6).
    if (at(TokenKind::Plus) || at(TokenKind::Pipe) || at(TokenKind::Star)) {
        g->multSpan = here();
        g->mult = at(TokenKind::Plus)   ? MultKind::Series
                  : at(TokenKind::Pipe) ? MultKind::Parallel
                                        : MultKind::Node;
        advance();
        if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
            g->count = integerOf(cur());
            g->multSpan = g->multSpan.merge(here());
            advance();
        } else {
            error(here(), "expected a count after a multiplicity operator");
        }
    }

    g->span = g->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return g;
}

Replication* Parser::parseReplication() {
    auto* r = arena_.make<Replication>();
    r->span = here();
    expect(TokenKind::LBracket, "opening a replication");

    if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
        // "[N[ chain ]M]": the widths are repeated on the closing delimiter so
        // that a mismatched pair is caught by eye (spec 8.3).
        r->counted = true;
        r->inWidth = integerOf(cur());
        advance();
        expect(TokenKind::LBracket, "opening a counted replication");
        r->body = parseSegment();
        expect(TokenKind::RBracket, "closing a counted replication");
        if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
            r->outWidth = integerOf(cur());
            advance();
        } else {
            error(here(), "expected the output width before the closing ']'");
        }
        expect(TokenKind::RBracket, "closing a counted replication");
    } else {
        expect(TokenKind::LBracket, "opening a replication");
        r->body = parseSegment();
        expect(TokenKind::RBracket, "closing a replication");
        expect(TokenKind::RBracket, "closing a replication");
    }

    r->span = r->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return r;
}

Element* Parser::parseElement() {
    auto* e = arena_.make<Element>();
    e->span = here();

    if (looksLikeReplication()) {
        e->kind = ElementKind::Replication;
        e->replication = parseReplication();
        e->span = e->replication->span;
        // Spec 8.3: multiplicity shall not be applied to a replication.
        if (at(TokenKind::Plus) || at(TokenKind::Pipe) || at(TokenKind::Star)) {
            std::string op(1, at(TokenKind::Plus) ? '+' : at(TokenKind::Pipe) ? '|' : '*');
            diags_.report(DiagId::E37, here(), op + "N");
            advance();
            if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) advance();
        }
        return e;
    }

    if (at(TokenKind::LParen)) {
        e->kind = ElementKind::Group;
        e->group = parseGroup();
        e->span = e->group->span;
        // "([[...]])+2" is E-37 too: a group whose sole content is a
        // replication does not launder the multiplicity (spec 8.3).
        if (e->group->mult != MultKind::None && e->group->body &&
            e->group->body->elements.size() == 1 &&
            e->group->body->elements[0]->kind == ElementKind::Replication) {
            std::string op = e->group->mult == MultKind::Series     ? "+N"
                             : e->group->mult == MultKind::Parallel ? "|N"
                                                                    : "*N";
            diags_.report(DiagId::E37, e->group->multSpan, op);
        }
        return e;
    }

    if (looksLikeDevice()) {
        e->kind = ElementKind::Device;
        e->device = parseDevice();
        e->span = e->device->span;
        return e;
    }

    e->kind = ElementKind::Net;
    e->net = parseNetExpr();
    e->span = e->net->span;
    return e;
}

// Whether an element, as written, offers a far side for the chain to advance
// through (spec 6.2). A net is its own far side; a device passes through when
// an exit terminal is written; a '*N' group hangs on the node; any other group
// or a replication passes through when its unit's last element does.
static bool elementHasExit(const Element* e) {
    switch (e->kind) {
        case ElementKind::Net: return true;
        case ElementKind::Device: return e->device->hasExit;
        case ElementKind::Group: {
            if (e->group->mult == MultKind::Node) return false;
            const Segment* body = e->group->body;
            if (!body || body->elements.empty()) return true;
            return elementHasExit(body->elements.back());
        }
        case ElementKind::Replication: {
            const Segment* body = e->replication->body;
            if (!body || body->elements.empty()) return true;
            return elementHasExit(body->elements.back());
        }
    }
    return true;
}

Segment* Parser::parseSegment(bool openedSame, Span openSpan) {
    auto* s = arena_.make<Segment>();
    s->span = here();

    std::vector<Element*> elements;
    std::vector<Connector> connectors;
    std::vector<Span> connectorSpans;
    bool diagnosed = false;

    elements.push_back(parseElement());
    for (;;) {
        Connector c;
        if (at(TokenKind::EqEq)) c = Connector::Same;
        else if (at(TokenKind::EqStar)) c = Connector::Gather;
        else if (at(TokenKind::StarEq)) c = Connector::Broadcast;
        else if (at(TokenKind::Eq)) c = Connector::Advance;
        else break;
        Span cs = here();
        advance();
        // A '==' with nothing after it continues the node into nowhere.
        if (c == Connector::Same && atSegmentEnd()) {
            diags_.report(DiagId::E49, cs,
                          "a '==' with nothing after it; the node continues only "
                          "into an element");
            diagnosed = true;
            break;
        }
        connectors.push_back(c);
        connectorSpans.push_back(cs);
        elements.push_back(parseElement());
    }

    // Spec 6.2/6.3 (revision 1.6): the connector states whether the chain
    // moved, and it has to be telling the truth. '=' advances through the far
    // side of the element before it, so that element must have one; '=='
    // continues on the near side, legal exactly when the element before it
    // has no far side -- its other pins are spoken for inside its '{}'.
    if (!diagnosed) {
        if (openedSame) {
            // A binding is rooted at a pin, and a pin passes through (spec
            // 7.4), so a binding never opens with '=='.
            diags_.report(DiagId::E49, openSpan,
                          "'==' cannot open a binding; a pin passes through -- "
                          "open with '='");
        }
        for (std::size_t i = 0; i < connectors.size(); ++i) {
            bool exits = elementHasExit(elements[i]);
            if (connectors[i] == Connector::Same && exits) {
                diags_.report(DiagId::E49, connectorSpans[i],
                              "'==' after an element that passes through; the "
                              "chain advances with '='");
            } else if (connectors[i] == Connector::Advance && !exits) {
                diags_.report(DiagId::E49, connectorSpans[i],
                              "'=' after an element with no far side; continue "
                              "on the node with '=='");
            }
        }
    }

    s->elements = commit(elements);
    s->connectors = commit(connectors);
    s->span = s->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return s;
}

bool Parser::atSegmentEnd() const {
    // The tokens that may legitimately follow a chain segment: a terminator, a
    // '^' partition, a directive or field against the statement, or the close
    // of the construct the segment sits in (a binding body, a group).
    return at(TokenKind::Semi) || at(TokenKind::Caret) || at(TokenKind::Amp) ||
           at(TokenKind::Hash) || at(TokenKind::At) || at(TokenKind::RBrace) ||
           at(TokenKind::RParen) || at(TokenKind::Eof);
}

Chain* Parser::parseChain() {
    auto* c = arena_.make<Chain>();
    c->span = here();
    std::vector<Segment*> segments;
    segments.push_back(parseSegment());
    // '^' binds looser than '=' and '==', partitioning a statement into
    // independent segments that share one directive scope (spec 6.4).
    while (accept(TokenKind::Caret)) segments.push_back(parseSegment());
    c->segments = commit(segments);
    c->span = c->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return c;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

Stmt* Parser::parsePortListStatement() {
    auto* s = arena_.make<Stmt>();
    s->kind = StmtKind::PortList;
    s->span = here();
    expect(TokenKind::LBracket, "opening a port list");
    std::vector<NetExpr*> ports;
    if (!at(TokenKind::RBracket)) {
        do {
            ports.push_back(parseNetExpr());
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RBracket, "closing a port list");
    s->ports = commit(ports);
    s->listArrow = parseTrailingArrow();

    std::vector<Directive*> dirs;
    while (at(TokenKind::Amp)) dirs.push_back(parseDirective());
    s->directives = commit(dirs);

    expect(TokenKind::Semi, "terminating a statement");
    s->span = s->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return s;
}

Stmt* Parser::parseStatement() {
    auto* s = arena_.make<Stmt>();
    s->span = here();

    // Spec 6.6: a statement may be prefixed 'extern' to declare that the
    // instance it refers to is declared in another object.
    if (at(TokenKind::Word) && curText() == "extern") {
        s->isExtern = true;
        advance();
    }

    if (looksLikeFieldDecl()) {
        s->kind = StmtKind::Field;
        s->field = parseFieldDecl();
    } else if (looksLikePortList()) {
        // Re-dispatch: the port-list form owns its own terminator.
        pos_ = pos_;
        Stmt* pl = parsePortListStatement();
        pl->isExtern = s->isExtern;
        pl->span = s->span.merge(pl->span);
        return pl;
    } else {
        s->kind = StmtKind::Chain;
        s->chain = parseChain();
    }

    // Spec 11.2: a directive is written at the end of the statement, before the
    // terminator, and covers every net in the declared chain.
    std::vector<Directive*> dirs;
    while (at(TokenKind::Amp)) dirs.push_back(parseDirective());
    s->directives = commit(dirs);

    expect(TokenKind::Semi, "terminating a statement");
    s->span = s->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return s;
}

// ---------------------------------------------------------------------------
// Part and harness bodies
// ---------------------------------------------------------------------------

PinMap* Parser::parsePinMap() {
    auto* p = arena_.make<PinMap>();
    p->span = here();
    p->physSpan = here();

    // pin_spec = integer | identifier | string | "[" integer ":" integer "]"
    // A range is numbered; a single pad may carry the name its footprint gives
    // it (revision 2.0).
    if (accept(TokenKind::LBracket)) {
        p->physIsRange = true;
        if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
            p->physLo = integerOf(cur());
            advance();
        } else {
            error(here(), "expected a physical pin number");
        }
        expect(TokenKind::Colon, "in a physical pin range");
        if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
            p->physHi = integerOf(cur());
            advance();
        } else {
            error(here(), "expected the end of a physical pin range");
        }
        expect(TokenKind::RBracket, "closing a physical pin range");
    } else if (at(TokenKind::Word) && cur().has(WordFlags::Integer)) {
        p->physLo = p->physHi = integerOf(cur());
        advance();
    } else if (at(TokenKind::Word)) {
        p->physName = intern(text(advance()));
    } else if (at(TokenKind::String)) {
        std::string_view raw = text(advance());
        p->physName = intern(raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : raw);
    } else {
        error(here(), "expected a physical pin number, pad name or range");
    }
    p->physSpan = p->physSpan.merge(toks_.at(pos_ - 1).span(file_.id()));

    // Revision 1.6: a pin declaration *maps* the pad to its name. ':' is the
    // mapping; '=' assigns values (fields) and joins nets (chains), and a pin
    // declaration does neither.
    if (at(TokenKind::Eq)) {
        error(here(), "a pin declaration maps its pad to a name with ':'");
        advance();
    } else {
        expect(TokenKind::Colon, "in a pin declaration");
    }

    // pin_name = identifier [ "[" range "]" ] | identifier "." "[" members "]"
    p->logical = parseName(true);
    if (at(TokenKind::Dot) && ahead(1).kind == TokenKind::LBracket) {
        advance();
        advance();
        std::vector<Name> members;
        if (!at(TokenKind::RBracket)) {
            do {
                Name m;
                m.span = here();
                if (at(TokenKind::Plus)) { advance(); m.symbol = intern("+"); }
                else if (at(TokenKind::Minus)) { advance(); m.symbol = intern("-"); }
                else m = parseName(true);
                members.push_back(m);
            } while (accept(TokenKind::Comma));
        }
        expect(TokenKind::RBracket, "closing a harness member list");
        p->memberList = commit(members);
        p->hasMemberList = true;
    } else if (at(TokenKind::LBracket)) {
        p->logicalRange = parseRange();
    }

    p->arrow = parseTrailingArrow();

    // Directives and fields may be interleaved. Both apply to every pin the
    // line produces.
    std::vector<Directive*> dirs;
    std::vector<FieldDecl*> fields;
    for (;;) {
        if (at(TokenKind::Amp)) {
            dirs.push_back(parseDirective());
        } else if (looksLikeFieldDecl()) {
            fields.push_back(parseFieldDecl());
        } else {
            break;
        }
    }
    p->directives = commit(dirs);
    p->fields = commit(fields);

    expect(TokenKind::Semi, "terminating a pin map");
    p->span = p->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return p;
}

MemberDecl* Parser::parseMemberDecl() {
    auto* m = arena_.make<MemberDecl>();
    m->span = here();
    m->name = parseName(true);
    m->arrow = parseTrailingArrow();
    std::vector<Directive*> dirs;
    while (at(TokenKind::Amp)) dirs.push_back(parseDirective());
    m->directives = commit(dirs);
    expect(TokenKind::Semi, "terminating a harness member declaration");
    m->span = m->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return m;
}

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

// Reports a section marker found in a body that does not admit one, spanning
// the whole marker, and consumes the token so the body loop makes progress.
void Parser::rejectSectionMarker() {
    error(sectionMarkerSpan(cur()), "a section marker is only legal in a block body");
    advance();
}

void Parser::parseBlockBody(std::vector<BodyEntry>& out, bool allowSections) {
    while (!at(TokenKind::RBrace) && !atEnd()) {
        std::size_t before = pos_;
        int depth = braceDepth();
        if (at(TokenKind::SectionMarker)) {
            const Token& t = cur();
            if (!allowSections) {
                rejectSectionMarker();
            } else if (t.length == 0) {
                // A bare '---' inside a block. At the top level that shape
                // ends the file's manta content (spec 2.8); here a section
                // marker needs a name to render under.
                error(sectionMarkerSpan(t), "a section marker needs a title: '--- TITLE'");
                advance();
            } else {
                auto* s = arena_.make<SectionMarker>();
                s->name = intern(text(t));
                s->span = sectionMarkerSpan(t);
                advance();
                BodyEntry e;
                e.kind = BodyKind::Section;
                e.section = s;
                out.push_back(e);
            }
            continue;
        }
        if (atItemStart()) {
            BodyEntry e;
            e.kind = BodyKind::Item;
            e.item = parseItem();
            if (e.item) out.push_back(e);
        } else {
            BodyEntry e;
            e.kind = BodyKind::Stmt;
            e.stmt = parseStatement();
            if (e.stmt) out.push_back(e);
        }
        if (pos_ == before) {
            error(here(), std::format("unexpected {} in a block body",
                                      tokenKindName(cur().kind)));
            recoverToStatementEnd();
            if (pos_ == before) ++pos_;
        }
        resyncToDepth(depth);
    }
}

void Parser::parsePartBody(std::vector<BodyEntry>& out) {
    while (!at(TokenKind::RBrace) && !atEnd()) {
        std::size_t before = pos_;
        int depth = braceDepth();
        if (at(TokenKind::SectionMarker)) {
            rejectSectionMarker();
            continue;
        }
        if (looksLikeFieldDecl()) {
            BodyEntry e;
            e.kind = BodyKind::Field;
            e.field = parseFieldDecl();
            expect(TokenKind::Semi, "terminating a field declaration");
            out.push_back(e);
        } else {
            BodyEntry e;
            e.kind = BodyKind::PinMap;
            e.pin = parsePinMap();
            out.push_back(e);
        }
        if (pos_ == before) {
            error(here(), std::format("unexpected {} in a part body",
                                      tokenKindName(cur().kind)));
            recoverToStatementEnd();
            if (pos_ == before) ++pos_;
        }
        resyncToDepth(depth);
    }
}

void Parser::parseHarnessBody(std::vector<BodyEntry>& out) {
    while (!at(TokenKind::RBrace) && !atEnd()) {
        std::size_t before = pos_;
        int depth = braceDepth();
        if (at(TokenKind::SectionMarker)) {
            rejectSectionMarker();
            continue;
        }
        if (at(TokenKind::Amp)) {
            // Spec 12.5: a harness type may carry directives, which apply to
            // every identifier assigned that type.
            BodyEntry e;
            e.kind = BodyKind::Directive;
            e.directive = parseDirective();
            expect(TokenKind::Semi, "terminating a directive");
            out.push_back(e);
        } else {
            BodyEntry e;
            e.kind = BodyKind::Member;
            e.member = parseMemberDecl();
            out.push_back(e);
        }
        if (pos_ == before) {
            error(here(), "unexpected token in a harness body");
            recoverToStatementEnd();
            if (pos_ == before) ++pos_;
        }
        resyncToDepth(depth);
    }
}

void Parser::parseNetclassBody(std::vector<BodyEntry>& out) {
    while (!at(TokenKind::RBrace) && !atEnd()) {
        std::size_t before = pos_;
        int depth = braceDepth();
        if (at(TokenKind::SectionMarker)) {
            rejectSectionMarker();
            continue;
        }
        BodyEntry e;
        e.kind = BodyKind::Directive;
        e.directive = parseDirective();
        expect(TokenKind::Semi, "terminating a directive");
        out.push_back(e);
        if (pos_ == before) {
            error(here(), "a netclass body contains directives only");
            recoverToStatementEnd();
            if (pos_ == before) ++pos_;
        }
        resyncToDepth(depth);
    }
}

void Parser::parseMatchBody(std::vector<BodyEntry>& out) {
    while (!at(TokenKind::RBrace) && !atEnd()) {
        std::size_t before = pos_;
        int depth = braceDepth();
        if (at(TokenKind::SectionMarker)) {
            rejectSectionMarker();
            continue;
        }
        if (at(TokenKind::Word) && curText() == "match") {
            // Spec 11.4: a match group may contain another.
            BodyEntry e;
            e.kind = BodyKind::Item;
            Span start = here();
            advance();
            e.item = parseMatch(start);
            if (e.item) out.push_back(e);
        } else {
            BodyEntry e;
            e.kind = BodyKind::Field;
            e.field = parseFieldDecl();
            expect(TokenKind::Semi, "terminating a field declaration");
            out.push_back(e);
        }
        if (pos_ == before) {
            error(here(), "unexpected token in a match group");
            recoverToStatementEnd();
            if (pos_ == before) ++pos_;
        }
        resyncToDepth(depth);
    }
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

Item* Parser::parseBlock(bool isStatic, Span startSpan) {
    auto* item = arena_.make<Item>();
    item->kind = ItemKind::Block;
    item->isStatic = isStatic;
    item->span = startSpan;
    item->name = parseName(true);
    item->nameSpan = item->name.span;

    if (!expect(TokenKind::LBrace, "opening a block body")) {
        recoverToDeclEnd();
        return item;
    }
    std::vector<BodyEntry> body;
    parseBlockBody(body, /*allowSections=*/true);
    item->body = commit(body);
    expect(TokenKind::RBrace, "closing a block body");
    expect(TokenKind::Semi, "after a block declaration");
    item->span = item->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return item;
}

// A cable is a chain of devices, so its body is a block body: replication,
// groups, ranges and bindings all work in a cable for free, and they are exactly
// what a twenty-way loom needs. What may be *instantiated* in one is narrower
// than a block, but that is a semantic rule and belongs in the local checker,
// where it can say which part offended and what its type is.
Item* Parser::parseCable(bool isStatic, Span startSpan) {
    auto* item = arena_.make<Item>();
    item->kind = ItemKind::Cable;
    item->isStatic = isStatic;
    item->span = startSpan;
    item->name = parseName(true);
    item->nameSpan = item->name.span;

    if (!expect(TokenKind::LBrace, "opening a cable body")) {
        recoverToDeclEnd();
        return item;
    }
    std::vector<BodyEntry> body;
    parseBlockBody(body, /*allowSections=*/false);
    item->body = commit(body);
    expect(TokenKind::RBrace, "closing a cable body");
    expect(TokenKind::Semi, "after a cable declaration");
    item->span = item->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return item;
}

Item* Parser::parsePart(bool isStatic, Span startSpan) {
    auto* item = arena_.make<Item>();
    item->kind = ItemKind::Part;
    item->isStatic = isStatic;
    item->span = startSpan;
    item->name = parseName(true);
    item->nameSpan = item->name.span;

    if (!expect(TokenKind::LBrace, "opening a part body")) {
        recoverToDeclEnd();
        return item;
    }
    std::vector<BodyEntry> body;
    parsePartBody(body);
    item->body = commit(body);
    expect(TokenKind::RBrace, "closing a part body");
    expect(TokenKind::Semi, "after a part declaration");
    item->span = item->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return item;
}

Item* Parser::parseHarness(Span startSpan) {
    auto* item = arena_.make<Item>();
    item->kind = ItemKind::Harness;
    item->span = startSpan;
    item->name = parseName(true);
    item->nameSpan = item->name.span;

    if (!expect(TokenKind::LBrace, "opening a harness body")) {
        recoverToDeclEnd();
        return item;
    }
    std::vector<BodyEntry> body;
    parseHarnessBody(body);
    item->body = commit(body);
    expect(TokenKind::RBrace, "closing a harness body");
    expect(TokenKind::Semi, "after a harness declaration");
    item->span = item->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return item;
}

Item* Parser::parseNetclass(Span startSpan) {
    auto* item = arena_.make<Item>();
    item->kind = ItemKind::Netclass;
    item->span = startSpan;
    item->name = parseName(true);
    item->nameSpan = item->name.span;

    if (!expect(TokenKind::LBrace, "opening a netclass body")) {
        recoverToDeclEnd();
        return item;
    }
    std::vector<BodyEntry> body;
    parseNetclassBody(body);
    item->body = commit(body);
    expect(TokenKind::RBrace, "closing a netclass body");
    expect(TokenKind::Semi, "after a netclass declaration");
    item->span = item->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return item;
}

Item* Parser::parseMatch(Span startSpan) {
    auto* item = arena_.make<Item>();
    item->kind = ItemKind::Match;
    item->span = startSpan;
    item->name = parseName(true);
    item->nameSpan = item->name.span;

    if (!expect(TokenKind::LBrace, "opening a match group")) {
        recoverToDeclEnd();
        return item;
    }
    std::vector<BodyEntry> body;
    parseMatchBody(body);
    item->body = commit(body);
    expect(TokenKind::RBrace, "closing a match group");
    expect(TokenKind::Semi, "after a match group");
    item->span = item->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return item;
}

Item* Parser::parseItem() {
    Span start = here();
    bool isStatic = false;

    if (at(TokenKind::Word) && curText() == "static") {
        // Spec 4.2: internal linkage. Visible only within its own object.
        isStatic = true;
        advance();
    }

    if (!at(TokenKind::Word)) {
        error(here(), "expected a declaration");
        recoverToDeclEnd();
        return nullptr;
    }

    std::string_view kw = curText();
    if (kw == "block") { advance(); return parseBlock(isStatic, start); }
    if (kw == "part") { advance(); return parsePart(isStatic, start); }
    if (kw == "harness") {
        advance();
        if (isStatic) error(start, "'static' applies to blocks and parts only");
        return parseHarness(start);
    }
    if (kw == "netclass") {
        advance();
        if (isStatic) error(start, "'static' applies to blocks and parts only");
        return parseNetclass(start);
    }
    if (kw == "match") {
        advance();
        if (isStatic) error(start, "'static' applies to blocks and parts only");
        return parseMatch(start);
    }
    if (kw == "cable") {
        advance();
        return parseCable(isStatic, start);
    }

    error(here(), std::format(
        "expected 'block', 'part', 'harness', 'netclass', 'match' or 'cable', found '{}'", kw));
    recoverToDeclEnd();
    return nullptr;
}

SourceUnit Parser::run() {
    SourceUnit unit;
    unit.file = file_.id();

    std::vector<Item*> items;
    while (!atEnd()) {
        std::size_t before = pos_;
        // Spec 4.1: "Bare statements appear only inside a block."
        Item* item = parseItem();
        if (item) items.push_back(item);
        if (pos_ == before) ++pos_;
        // A declaration that ran out of braces leaves the file's own nesting
        // adrift; the next '}' would then be read as the start of a
        // declaration and every line after it reported as garbage.
        resyncToDepth(0);
    }

    unit.items = commit(items);
    return unit;
}

}  // namespace manta
