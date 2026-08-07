// The manta lexer.
//
// Two decisions shape it.
//
// 1. The lexer never resolves grammatical ambiguity. Spec 2.3 says a leading '-'
//    "is resolved by grammatical position": "-5V" is the net named -5V in a net
//    position and the value minus five volts in a value position. So the lexer
//    scans one maximal word and attaches a classification bitset computed over
//    the whole lexeme; the parser asks for the reading it needs. Because
//    classification looks at the entire word, "10kR-0603" simply fails the
//    dimensioned test and falls back to identifier, with no backtracking.
//
// 2. It has two modes. Inside "$...$" (spec 14.2) "every operator is an
//    operator": the delimiters place the lexer in a distinct mode where
//    + - * / ^ | & ! < > = carry their arithmetic meaning, '-' is always
//    subtraction rather than part of an identifier, comments are not recognised
//    and '/' is always division.
#pragma once

#include <string_view>
#include <vector>

#include "diag/engine.h"
#include "lex/token.h"
#include "source/source_manager.h"

namespace manta {

struct TokenStream {
    std::vector<Token> tokens;
    std::vector<Comment> comments;
    FileId file = kNoFile;

    [[nodiscard]] const Token& at(std::size_t i) const {
        return i < tokens.size() ? tokens[i] : tokens.back();  // back() is always Eof
    }
    [[nodiscard]] std::size_t size() const noexcept { return tokens.size(); }
};

class Lexer {
public:
    Lexer(const SourceFile& file, DiagEngine& diags) : file_(file), diags_(diags) {}

    // Lexes the whole file. Lexical errors are reported but never abort the
    // scan: the parser gets a complete stream so that one stray character does
    // not hide every later diagnostic.
    TokenStream run();

private:
    void scanToken(TokenStream& out);
    void scanWord(TokenStream& out);
    void scanExprWord(TokenStream& out);
    void scanString(TokenStream& out);
    bool skipTrivia(TokenStream& out);

    void push(TokenStream& out, TokenKind k, std::uint32_t start, std::uint32_t len,
              std::uint16_t flags = 0);

    [[nodiscard]] char peek(std::size_t ahead = 0) const {
        std::size_t i = pos_ + ahead;
        return i < text_.size() ? text_[i] : '\0';
    }
    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] std::string_view rest() const { return text_.substr(pos_); }
    [[nodiscard]] Span spanAt(std::uint32_t start, std::uint32_t len) const {
        return Span{file_.id(), start, len};
    }

    const SourceFile& file_;
    DiagEngine& diags_;
    std::string_view text_ = file_.text();
    std::size_t pos_ = 0;
    bool exprMode_ = false;      // inside $...$
    bool lineHadToken_ = false;  // for Comment::ownLine
    std::uint32_t blankRun_ = 0;
};

}  // namespace manta
