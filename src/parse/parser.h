// Recursive-descent parser for the grammar of spec 19.
//
// The grammar is LL(2) once three ambiguities are handled by bounded lookahead
// rather than by the lexer guessing:
//
//  * '[' opens a replication "[[ ... ]]", a counted replication "[N[ ... ]M]",
//    a range "[lo:hi]", a list "[a, b]" or a port list "[3V3, GND]>>". Two
//    tokens separate them.
//  * A chain element beginning with a word is a device when a '{' follows the
//    terminal, and a net expression otherwise: "I{U1~AMP012}O" against "SIG-IN".
//  * A leading '-' is an identifier in a net position and a negative number in
//    a value position (spec 2.3), which is why the lexer hands over a
//    classification bitset and the parser picks.
#pragma once

#include <vector>

#include "ast/ast.h"
#include "base/arena.h"
#include "base/intern.h"
#include "diag/engine.h"
#include "lex/lexer.h"

namespace manta {

class Parser {
public:
    Parser(const TokenStream& tokens, const SourceFile& file, Arena& arena,
           StringInterner& interner, DiagEngine& diags)
        : toks_(tokens), file_(file), arena_(arena), interner_(interner), diags_(diags) {}

    [[nodiscard]] SourceUnit run();

private:
    // ---- token access -----------------------------------------------------
    [[nodiscard]] const Token& cur() const { return toks_.at(pos_); }
    [[nodiscard]] const Token& ahead(std::size_t n = 1) const { return toks_.at(pos_ + n); }
    [[nodiscard]] bool at(TokenKind k) const { return cur().kind == k; }
    [[nodiscard]] bool atEnd() const { return cur().kind == TokenKind::Eof; }
    [[nodiscard]] Span span(const Token& t) const { return t.span(file_.id()); }
    [[nodiscard]] Span here() const { return span(cur()); }
    [[nodiscard]] std::string_view text(const Token& t) const {
        return file_.text().substr(t.offset, t.length);
    }
    [[nodiscard]] std::string_view curText() const { return text(cur()); }

    // True when two tokens abut with no intervening whitespace, which is how an
    // interpolated identifier such as "R?~$val$-0603" is recognised.
    [[nodiscard]] bool adjacent(std::size_t i) const {
        return toks_.at(i).offset + toks_.at(i).length == toks_.at(i + 1).offset;
    }

    const Token& advance() { return toks_.at(pos_++); }
    bool accept(TokenKind k) {
        if (at(k)) { ++pos_; return true; }
        return false;
    }
    bool expect(TokenKind k, std::string_view context);

    // ---- error recovery ---------------------------------------------------
    void error(Span at, std::string message);
    void recoverToStatementEnd();
    void recoverToDeclEnd();

    // ---- declarations -----------------------------------------------------
    Item* parseItem();
    Item* parseBlock(bool isStatic, Span startSpan);
    Item* parsePart(bool isStatic, Span startSpan);
    Item* parseHarness(Span startSpan);
    Item* parseNetclass(Span startSpan);
    Item* parseMatch(Span startSpan);
    Item* parseCable(bool isStatic, Span startSpan);

    // ---- bodies -----------------------------------------------------------
    void parseBlockBody(std::vector<BodyEntry>& out);
    void parsePartBody(std::vector<BodyEntry>& out);
    void parseHarnessBody(std::vector<BodyEntry>& out);
    void parseNetclassBody(std::vector<BodyEntry>& out);
    void parseMatchBody(std::vector<BodyEntry>& out);

    PinMap* parsePinMap();
    MemberDecl* parseMemberDecl();

    // ---- statements -------------------------------------------------------
    Stmt* parseStatement();
    Stmt* parsePortListStatement();
    Chain* parseChain();
    Segment* parseSegment();
    Element* parseElement();
    Device* parseDevice();
    Group* parseGroup();
    Replication* parseReplication();
    Instance* parseInstance();
    Binding* parseBinding();
    Terminal parseTerminal();
    NetExpr* parseNetExpr();

    // ---- leaves -----------------------------------------------------------
    FieldDecl* parseFieldDecl();
    Directive* parseDirective();
    Value* parseValue();
    Value* parseListValue();
    Name parseName(bool identifierPosition);
    Range parseRange();
    Index parseIndex();
    PortSpec parseLeadingArrow();
    PortSpec parseTrailingArrow();
    Designator parseDesignator();
    InterpText* parseInterpRun(std::vector<InterpChunk>& chunks, Span start);

    // ---- substitution expressions (spec 14.3) -----------------------------
    Expr* parseInterpolation();  // consumes '$' expr '$'
    Expr* parseExpr();
    Expr* parseBinary(int minPrecedence);
    Expr* parseUnary();
    Expr* parseAtom();

    // ---- lookahead helpers ------------------------------------------------
    [[nodiscard]] bool looksLikeDevice() const;
    [[nodiscard]] bool looksLikeReplication() const;
    [[nodiscard]] bool looksLikePortList() const;
    [[nodiscard]] bool looksLikeFieldDecl() const;
    [[nodiscard]] bool atItemStart() const;

    // ---- helpers ----------------------------------------------------------
    template <typename T>
    std::span<T> commit(std::vector<T>& v) {
        auto dst = arena_.makeArray<T>(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) dst[i] = v[i];
        return dst;
    }

    SymbolId intern(std::string_view s) { return interner_.intern(s); }

    // The value of a Word already known to carry WordFlags::Integer.
    [[nodiscard]] std::int64_t integerOf(const Token& t) const {
        Dimensioned d;
        return parseDimensioned(text(t), d) ? d.mantissa : 0;
    }

    // Reports E-02 when a hyphen-terminated word is used where an identifier is
    // required. Values may legitimately end in '-' (version constraints).
    void checkTrailingDash(const Token& t);

    const TokenStream& toks_;
    const SourceFile& file_;
    Arena& arena_;
    StringInterner& interner_;
    DiagEngine& diags_;
    std::size_t pos_ = 0;
    int errorBudget_ = 200;  // stop reporting after a cascade; keep parsing
};

}  // namespace manta
