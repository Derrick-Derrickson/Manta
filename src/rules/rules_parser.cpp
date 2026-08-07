#include "rules/rules_parser.h"

#include <format>

namespace manta {

std::string_view ruleBinOpText(RuleBinOp op) noexcept {
    switch (op) {
        case RuleBinOp::Add: return "+";
        case RuleBinOp::Subtract: return "-";
        case RuleBinOp::Multiply: return "*";
        case RuleBinOp::Divide: return "/";
        case RuleBinOp::Less: return "<";
        case RuleBinOp::LessEqual: return "<=";
        case RuleBinOp::Greater: return ">";
        case RuleBinOp::GreaterEqual: return ">=";
        case RuleBinOp::Equal: return "==";
        case RuleBinOp::NotEqual: return "!=";
        case RuleBinOp::And: return "&";
        case RuleBinOp::Or: return "|";
    }
    return "?";
}

std::string_view aggregateName(Aggregate a) noexcept {
    switch (a) {
        case Aggregate::Sum: return "sum";
        case Aggregate::Min: return "min";
        case Aggregate::Max: return "max";
        case Aggregate::Count: return "count";
        case Aggregate::Any: return "any";
        case Aggregate::All: return "all";
    }
    return "sum";
}

bool aggregateFromName(std::string_view name, Aggregate& out) noexcept {
    if (name == "sum") { out = Aggregate::Sum; return true; }
    if (name == "min") { out = Aggregate::Min; return true; }
    if (name == "max") { out = Aggregate::Max; return true; }
    if (name == "count") { out = Aggregate::Count; return true; }
    if (name == "any") { out = Aggregate::Any; return true; }
    if (name == "all") { out = Aggregate::All; return true; }
    return false;
}

std::string_view ruleDomainName(RuleDomain d) noexcept {
    switch (d) {
        case RuleDomain::Part: return "part";
        case RuleDomain::Net: return "net";
        case RuleDomain::Component: return "component";
        case RuleDomain::PinPair: return "net pin pair";
    }
    return "net";
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

void RulesParser::error(Span at, std::string message) {
    if (errorBudget_-- <= 0) return;
    diags_.report(DiagId::Syntax, at, std::move(message));
}

bool RulesParser::expect(TokenKind k, std::string_view context) {
    if (at(k)) {
        ++pos_;
        return true;
    }
    error(here(), std::format("expected {} {}, found {}", tokenKindName(k), context,
                              tokenKindName(cur().kind)));
    return false;
}

void RulesParser::recoverToSemi() {
    int depth = 0;
    while (!atEnd()) {
        TokenKind k = cur().kind;
        if (k == TokenKind::LBrace) ++depth;
        if (k == TokenKind::RBrace) {
            if (depth == 0) return;
            --depth;
        }
        ++pos_;
        if (k == TokenKind::Semi && depth == 0) return;
    }
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

std::string RulesParser::unescape(std::string_view raw) const {
    std::string out;
    if (raw.size() < 2) return out;
    std::string_view body = raw.substr(1, raw.size() - 2);
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            switch (body[i + 1]) {
                case 'n': out += '\n'; ++i; continue;
                case 't': out += '\t'; ++i; continue;
                case '"': out += '"'; ++i; continue;
                case '\\': out += '\\'; ++i; continue;
                default: break;
            }
        }
        out += body[i];
    }
    return out;
}

// A message hole. Deliberately not the full expression grammar: a hole names
// something, and a nested lexer for the handful of shapes a diagnostic needs
// would cost more than it earns.
//
//   name | name.member[.member...] | agg(path)
RuleExpr* RulesParser::parseMessageHole(std::string_view body, Span messageSpan) {
    while (!body.empty() && (body.front() == ' ' || body.front() == '\t')) body.remove_prefix(1);
    while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.remove_suffix(1);
    if (body.empty()) return nullptr;

    Aggregate agg{};
    bool aggregated = false;
    std::size_t open = body.find('(');
    if (open != std::string_view::npos && body.back() == ')') {
        if (aggregateFromName(body.substr(0, open), agg)) {
            aggregated = true;
            body = body.substr(open + 1, body.size() - open - 2);
        }
    }

    RuleExpr* current = nullptr;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        if (i != body.size() && body[i] != '.') continue;
        std::string_view part = body.substr(start, i - start);
        if (part.empty()) {
            error(messageSpan, std::format("'{}' is not a name or a dotted path", body));
            return nullptr;
        }
        auto* e = arena_.make<RuleExpr>();
        e->span = messageSpan;
        e->text = interner_.intern(part);
        if (!current) {
            e->kind = RuleExprKind::Name;
        } else {
            e->kind = RuleExprKind::Member;
            e->lhs = current;
        }
        current = e;
        start = i + 1;
    }

    if (aggregated && current) {
        auto* e = arena_.make<RuleExpr>();
        e->kind = RuleExprKind::Aggregate;
        e->aggregate = agg;
        e->lhs = current;
        e->span = messageSpan;
        current = e;
    }
    return current;
}

std::span<MessageChunk> RulesParser::parseMessage() {
    Span messageSpan = here();
    std::string joined;
    while (at(TokenKind::String)) {
        joined += unescape(text(advance()));
    }

    std::vector<MessageChunk> chunks;
    std::string literal;
    for (std::size_t i = 0; i < joined.size(); ++i) {
        if (joined[i] != '{') {
            literal += joined[i];
            continue;
        }
        std::size_t close = joined.find('}', i);
        if (close == std::string::npos) {
            error(messageSpan, "unterminated '{' in a message");
            literal += joined.substr(i);
            break;
        }
        if (!literal.empty()) {
            chunks.push_back(MessageChunk{false, interner_.intern(literal), nullptr});
            literal.clear();
        }
        if (RuleExpr* e = parseMessageHole(std::string_view(joined).substr(i + 1, close - i - 1),
                                           messageSpan)) {
            chunks.push_back(MessageChunk{true, SymbolId::kInvalid, e});
        }
        i = close;
    }
    if (!literal.empty()) {
        chunks.push_back(MessageChunk{false, interner_.intern(literal), nullptr});
    }
    return commit(chunks);
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

RuleExpr* RulesParser::parsePrimary() {
    auto* e = arena_.make<RuleExpr>();
    e->span = here();

    if (accept(TokenKind::LParen)) {
        RuleExpr* inner = parseExpr();
        expect(TokenKind::RParen, "closing a group");
        return inner;
    }

    if (at(TokenKind::String)) {
        const Token& t = advance();
        e->kind = RuleExprKind::String;
        e->text = interner_.intern(unescape(text(t)));
        e->span = t.span(file_.id());
        return e;
    }

    if (at(TokenKind::Word)) {
        const Token& t = cur();
        std::string_view word = curText();

        // has(x) -- presence. Explicit, so a voltage never has an implicit
        // truthiness of its own.
        if (word == "has" && ahead(1).kind == TokenKind::LParen) {
            advance();
            advance();
            e->kind = RuleExprKind::Has;
            e->lhs = parseExpr();
            expect(TokenKind::RParen, "closing 'has('");
            e->span = e->span.merge(toks_.at(pos_ - 1).span(file_.id()));
            return e;
        }

        Aggregate agg{};
        if (aggregateFromName(word, agg) && ahead(1).kind == TokenKind::LParen) {
            advance();
            advance();
            e->kind = RuleExprKind::Aggregate;
            e->aggregate = agg;
            e->lhs = parseExpr();
            // An optional filter over the elements: sum(pins.DRAW where
            // direction == in). Inside it, a bare name is a property of the
            // element being considered.
            if (acceptWord("where")) e->filter = parseExpr();
            expect(TokenKind::RParen, "closing an aggregate");
            e->span = e->span.merge(toks_.at(pos_ - 1).span(file_.id()));
            return e;
        }

        // A numeric literal, dimensioned or bare.
        if (t.has(WordFlags::Dimensioned) || t.has(WordFlags::Integer) ||
            t.has(WordFlags::Decimal)) {
            advance();
            e->kind = RuleExprKind::Number;
            (void)parseDimensioned(word, e->number);
            e->span = t.span(file_.id());
            return e;
        }

        advance();
        e->kind = RuleExprKind::Name;
        e->text = interner_.intern(word);
        e->span = t.span(file_.id());
        return e;
    }

    error(here(), std::format("expected a value, found {}", tokenKindName(cur().kind)));
    e->kind = RuleExprKind::Number;
    return e;
}

RuleExpr* RulesParser::parsePostfix() {
    RuleExpr* base = parsePrimary();
    while (at(TokenKind::Dot)) {
        advance();
        if (!at(TokenKind::Word)) {
            error(here(), "expected a property name after '.'");
            break;
        }
        const Token& t = advance();
        auto* member = arena_.make<RuleExpr>();
        member->kind = RuleExprKind::Member;
        member->lhs = base;
        member->text = interner_.intern(text(t));
        member->span = base->span.merge(t.span(file_.id()));
        base = member;
    }
    return base;
}

RuleExpr* RulesParser::parseUnary() {
    if (at(TokenKind::Bang) || at(TokenKind::Minus)) {
        auto* e = arena_.make<RuleExpr>();
        e->kind = RuleExprKind::Unary;
        e->span = here();
        e->unOp = at(TokenKind::Bang) ? RuleUnOp::Not : RuleUnOp::Negate;
        advance();
        e->lhs = parseUnary();
        e->span = e->span.merge(e->lhs->span);
        return e;
    }
    return parsePostfix();
}

namespace {

RuleExpr* makeBinary(Arena& arena, RuleBinOp op, RuleExpr* lhs, RuleExpr* rhs) {
    auto* e = arena.make<RuleExpr>();
    e->kind = RuleExprKind::Binary;
    e->binOp = op;
    e->lhs = lhs;
    e->rhs = rhs;
    e->span = lhs->span.merge(rhs->span);
    return e;
}

}  // namespace

RuleExpr* RulesParser::parseProduct() {
    RuleExpr* lhs = parseUnary();
    for (;;) {
        RuleBinOp op;
        if (at(TokenKind::Star)) op = RuleBinOp::Multiply;
        else if (at(TokenKind::Slash)) op = RuleBinOp::Divide;
        else break;
        advance();
        lhs = makeBinary(arena_, op, lhs, parseUnary());
    }
    return lhs;
}

RuleExpr* RulesParser::parseSum() {
    RuleExpr* lhs = parseProduct();
    for (;;) {
        RuleBinOp op;
        if (at(TokenKind::Plus)) op = RuleBinOp::Add;
        else if (at(TokenKind::Minus)) op = RuleBinOp::Subtract;
        else break;
        advance();
        lhs = makeBinary(arena_, op, lhs, parseProduct());
    }
    return lhs;
}

RuleExpr* RulesParser::parseComparison() {
    RuleExpr* lhs = parseSum();
    RuleBinOp op;
    // '>=' and '<=' arrive as single tokens only inside a substitution, so here
    // they are a relation followed by '='. Both spellings are accepted.
    if (at(TokenKind::Le)) op = RuleBinOp::LessEqual;
    else if (at(TokenKind::Ge)) op = RuleBinOp::GreaterEqual;
    else if (at(TokenKind::Lt)) {
        op = ahead(1).kind == TokenKind::Eq ? RuleBinOp::LessEqual : RuleBinOp::Less;
    } else if (at(TokenKind::Gt)) {
        op = ahead(1).kind == TokenKind::Eq ? RuleBinOp::GreaterEqual : RuleBinOp::Greater;
    } else if (at(TokenKind::EqEq)) {
        op = RuleBinOp::Equal;
    } else if (at(TokenKind::BangEq)) {
        op = RuleBinOp::NotEqual;
    } else if (at(TokenKind::Bang) && ahead(1).kind == TokenKind::Eq) {
        op = RuleBinOp::NotEqual;
    } else {
        return lhs;
    }

    bool twoTokens = (at(TokenKind::Lt) || at(TokenKind::Gt) || at(TokenKind::Bang)) &&
                     ahead(1).kind == TokenKind::Eq;
    advance();
    if (twoTokens) advance();
    return makeBinary(arena_, op, lhs, parseSum());
}

RuleExpr* RulesParser::parseAnd() {
    RuleExpr* lhs = parseComparison();
    while (at(TokenKind::Amp)) {
        advance();
        lhs = makeBinary(arena_, RuleBinOp::And, lhs, parseComparison());
    }
    return lhs;
}

RuleExpr* RulesParser::parseOr() {
    RuleExpr* lhs = parseAnd();
    while (at(TokenKind::Pipe)) {
        advance();
        lhs = makeBinary(arena_, RuleBinOp::Or, lhs, parseAnd());
    }
    return lhs;
}

RuleExpr* RulesParser::parseExpr() { return parseOr(); }

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

FieldTypeDecl* RulesParser::parseFieldDecl() {
    auto* f = arena_.make<FieldTypeDecl>();
    f->span = here();
    expect(TokenKind::Hash, "introducing a field type");

    if (at(TokenKind::Word)) {
        const Token& t = advance();
        f->name = interner_.intern(text(t));
    } else {
        error(here(), "expected a field name");
    }

    expect(TokenKind::Colon, "before a field's quantity");

    if (at(TokenKind::Word)) {
        const Token& t = advance();
        std::string_view quantity = text(t);
        if (quantity == "boolean") {
            f->isBoolean = true;
        } else if (quantity == "text") {
            f->isText = true;
        } else if (!unitFromName(quantity, f->unit)) {
            error(t.span(file_.id()),
                  std::format("'{}' is not a quantity; expected voltage, current, resistance, "
                              "capacitance, inductance, power, frequency, time, length, "
                              "temperature, number, boolean or text",
                              quantity));
        }
    } else {
        error(here(), "expected a quantity");
    }

    expect(TokenKind::Semi, "terminating a field type");
    f->span = f->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return f;
}

bool RulesParser::parseDomain(RuleCheck& check) {
    if (acceptWord("part")) {
        check.domain = RuleDomain::Part;
        return true;
    }
    if (acceptWord("component")) {
        check.domain = RuleDomain::Component;
        return true;
    }
    if (!acceptWord("net")) {
        error(here(), "expected 'net', 'component' or 'part' after 'for'");
        return false;
    }

    // Plain "for net", or the pin-pair form "for net.a -> net.b".
    if (!at(TokenKind::Dot)) {
        check.domain = RuleDomain::Net;
        return true;
    }

    advance();
    if (!at(TokenKind::Word)) {
        error(here(), "expected a binding name after 'net.'");
        return false;
    }
    check.leftBinding = interner_.intern(text(advance()));

    // "->" lexes as Minus then Gt, since manta has no arrow token.
    if (!accept(TokenKind::Minus) || !accept(TokenKind::Gt)) {
        error(here(), "expected '->' between the two pin bindings");
        return false;
    }

    if (!acceptWord("net") || !accept(TokenKind::Dot)) {
        error(here(), "expected 'net.' after '->'");
        return false;
    }
    if (!at(TokenKind::Word)) {
        error(here(), "expected a binding name after 'net.'");
        return false;
    }
    check.rightBinding = interner_.intern(text(advance()));
    check.domain = RuleDomain::PinPair;
    return true;
}

RuleCheck* RulesParser::parseCheck() {
    auto* c = arena_.make<RuleCheck>();
    c->span = here();
    advance();  // "check"

    if (at(TokenKind::Word)) {
        const Token& t = advance();
        c->name = interner_.intern(text(t));
        c->nameSpan = t.span(file_.id());
    } else {
        error(here(), "expected a name for the check");
    }

    if (!acceptWord("for")) error(here(), "expected 'for' and a domain");
    if (!parseDomain(*c)) {
        recoverToSemi();
        return c;
    }

    if (!expect(TokenKind::LBrace, "opening a check body")) return c;

    std::vector<RuleExpr*> guards;
    bool sawMessage = false;

    while (!at(TokenKind::RBrace) && !atEnd()) {
        std::size_t before = pos_;

        if (acceptWord("when")) {
            guards.push_back(parseExpr());
            expect(TokenKind::Semi, "terminating a 'when' clause");
        } else if (acceptWord("require")) {
            if (c->requirement) {
                error(here(), "a check has one 'require'; combine conditions with '&'");
            }
            c->requirement = parseExpr();
            expect(TokenKind::Semi, "terminating a 'require' clause");
        } else if (atWord("error") || atWord("warning")) {
            c->severity = curText() == "error" ? RuleSeverity::Error : RuleSeverity::Warning;
            advance();
            if (!at(TokenKind::String)) {
                error(here(), "expected a message");
            } else {
                c->message = parseMessage();
                sawMessage = true;
            }
            expect(TokenKind::Semi, "terminating a message");
        } else {
            error(here(), "expected 'when', 'require', 'error' or 'warning'");
            recoverToSemi();
        }

        if (pos_ == before) ++pos_;
    }

    c->guards = commit(guards);
    expect(TokenKind::RBrace, "closing a check body");
    expect(TokenKind::Semi, "after a check");
    c->span = c->span.merge(toks_.at(pos_ - 1).span(file_.id()));

    if (!c->requirement) {
        error(c->span, "a check needs a 'require' clause; without one it can never fire");
    }
    if (!sawMessage) {
        error(c->span, "a check needs an 'error' or 'warning' message");
    }
    return c;
}

RuleSet* RulesParser::parseRuleSet() {
    auto* set = arena_.make<RuleSet>();
    set->span = here();
    advance();  // "rules"

    if (at(TokenKind::Word)) {
        set->name = interner_.intern(text(advance()));
    } else {
        error(here(), "expected a name for the rule set");
    }

    if (!expect(TokenKind::LBrace, "opening a rule set")) return set;

    std::vector<FieldTypeDecl*> fields;
    std::vector<RuleCheck*> checks;

    while (!at(TokenKind::RBrace) && !atEnd()) {
        std::size_t before = pos_;
        if (at(TokenKind::Hash)) {
            fields.push_back(parseFieldDecl());
        } else if (atWord("check")) {
            checks.push_back(parseCheck());
        } else {
            error(here(), "expected a '#' field type or a 'check'");
            recoverToSemi();
        }
        if (pos_ == before) ++pos_;
    }

    set->fields = commit(fields);
    set->checks = commit(checks);
    expect(TokenKind::RBrace, "closing a rule set");
    expect(TokenKind::Semi, "after a rule set");
    set->span = set->span.merge(toks_.at(pos_ - 1).span(file_.id()));
    return set;
}

RuleFile RulesParser::run() {
    RuleFile out;
    out.file = file_.id();

    std::vector<RuleSet*> sets;
    while (!atEnd()) {
        std::size_t before = pos_;
        if (atWord("rules")) {
            sets.push_back(parseRuleSet());
        } else {
            error(here(), std::format("expected 'rules', found {}", tokenKindName(cur().kind)));
            recoverToSemi();
        }
        if (pos_ == before) ++pos_;
    }

    out.sets = commit(sets);
    return out;
}

}  // namespace manta
