#include "lex/lexer.h"

#include <format>

#include "base/utf8.h"
#include "lex/dimensioned.h"

namespace manta {

namespace {

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isLetter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool isWordStart(char c) noexcept { return isLetter(c) || isDigit(c) || c == '_'; }
constexpr bool isWordBody(char c) noexcept { return isWordStart(c) || c == '-'; }
constexpr bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

}  // namespace

std::string_view tokenKindName(TokenKind k) noexcept {
    switch (k) {
        case TokenKind::Eof: return "end of file";
        case TokenKind::Word: return "word";
        case TokenKind::String: return "string";
        case TokenKind::LBrace: return "'{'";
        case TokenKind::RBrace: return "'}'";
        case TokenKind::LParen: return "'('";
        case TokenKind::RParen: return "')'";
        case TokenKind::LBracket: return "'['";
        case TokenKind::RBracket: return "']'";
        case TokenKind::Semi: return "';'";
        case TokenKind::Comma: return "','";
        case TokenKind::Colon: return "':'";
        case TokenKind::Dot: return "'.'";
        case TokenKind::Tilde: return "'~'";
        case TokenKind::Bang: return "'!'";
        case TokenKind::Question: return "'?'";
        case TokenKind::Percent: return "'%'";
        case TokenKind::Hash: return "'#'";
        case TokenKind::At: return "'@'";
        case TokenKind::Amp: return "'&'";
        case TokenKind::Dollar: return "'$'";
        case TokenKind::PlusMinus: return "'±'";
        case TokenKind::Eq: return "'='";
        case TokenKind::EqEq: return "'=='";
        case TokenKind::EqStar: return "'=*'";
        case TokenKind::StarEq: return "'*='";
        case TokenKind::Gt: return "'>'";
        case TokenKind::Lt: return "'<'";
        case TokenKind::LtGt: return "'<>'";
        case TokenKind::GtGt: return "'>>'";
        case TokenKind::Plus: return "'+'";
        case TokenKind::Minus: return "'-'";
        case TokenKind::Star: return "'*'";
        case TokenKind::Pipe: return "'|'";
        case TokenKind::Caret: return "'^'";
        case TokenKind::Slash: return "'/'";
        case TokenKind::Le: return "'<='";
        case TokenKind::Ge: return "'>='";
        case TokenKind::BangEq: return "'!='";
        case TokenKind::Invalid: return "invalid character";
    }
    return "token";
}

void Lexer::push(TokenStream& out, TokenKind k, std::uint32_t start, std::uint32_t len,
                 std::uint16_t flags) {
    out.tokens.push_back(Token{k, flags, start, len});
    lineHadToken_ = true;
}

// Returns true if any trivia was consumed.
bool Lexer::skipTrivia(TokenStream& out) {
    bool any = false;
    for (;;) {
        // Whitespace. Newlines reset the "own line" tracking used by the
        // formatter to decide whether a comment sits above or beside code.
        while (!atEnd() && isSpace(peek())) {
            if (peek() == '\n') {
                if (!lineHadToken_) ++blankRun_;
                lineHadToken_ = false;
            }
            ++pos_;
            any = true;
        }

        // Spec 14.2: comments are not recognised inside "$...$", where '/' is
        // always division.
        if (exprMode_) return any;

        if (peek() == '/' && peek(1) == '/') {
            auto start = static_cast<std::uint32_t>(pos_);
            bool ownLine = !lineHadToken_;
            pos_ += 2;
            while (!atEnd() && peek() != '\n') ++pos_;
            out.comments.push_back(Comment{start, static_cast<std::uint32_t>(pos_) - start,
                                           static_cast<std::uint32_t>(out.tokens.size()), false,
                                           ownLine, blankRun_ > 0});
            blankRun_ = 0;
            lineHadToken_ = true;
            any = true;
            continue;
        }

        if (peek() == '/' && peek(1) == '*') {
            auto start = static_cast<std::uint32_t>(pos_);
            bool ownLine = !lineHadToken_;
            pos_ += 2;
            // Spec 2.5: block comments do not nest; the first '*/' closes it.
            bool closed = false;
            while (!atEnd()) {
                if (peek() == '*' && peek(1) == '/') {
                    pos_ += 2;
                    closed = true;
                    break;
                }
                if (peek() == '\n') lineHadToken_ = false;
                ++pos_;
            }
            if (!closed) {
                diags_.report(DiagId::Syntax, spanAt(start, 2),
                              std::string("unterminated block comment"));
            }
            out.comments.push_back(Comment{start, static_cast<std::uint32_t>(pos_) - start,
                                           static_cast<std::uint32_t>(out.tokens.size()), true,
                                           ownLine, blankRun_ > 0});
            blankRun_ = 0;
            lineHadToken_ = true;
            any = true;
            continue;
        }

        return any;
    }
}

void Lexer::scanString(TokenStream& out) {
    auto start = static_cast<std::uint32_t>(pos_);
    ++pos_;  // opening quote
    bool closed = false;
    while (!atEnd()) {
        char c = peek();
        if (c == '\\') {
            // Spec 3.5 supports \" \\ \n \t.
            char e = peek(1);
            if (e == '"' || e == '\\' || e == 'n' || e == 't') {
                pos_ += 2;
                continue;
            }
            diags_.report(DiagId::Syntax, spanAt(static_cast<std::uint32_t>(pos_), 2),
                          std::format("unknown escape '\\{}' in string literal", e));
            pos_ += 2;
            continue;
        }
        if (c == '"') {
            ++pos_;
            closed = true;
            break;
        }
        if (c == '\n') break;  // unterminated; report at the opening quote
        // Spec 2.1: string literals may contain any UTF-8 codepoint.
        pos_ += static_cast<std::size_t>(utf8SequenceLength(static_cast<unsigned char>(c)));
    }
    if (!closed) {
        diags_.report(DiagId::Syntax, spanAt(start, 1), std::string("unterminated string literal"));
    }
    push(out, TokenKind::String, start, static_cast<std::uint32_t>(pos_) - start);
}

// A word outside "$...$": maximal munch over identifier and numeric characters.
void Lexer::scanWord(TokenStream& out) {
    auto start = static_cast<std::uint32_t>(pos_);

    if (peek() == '-') ++pos_;

    // Tracks whether everything since the start of the word, or since the last
    // '-', has been digits -- that is, whether a *number* is being built.
    std::size_t runStart = pos_;

    while (!atEnd()) {
        char c = peek();
        if (c == '-') {
            ++pos_;
            runStart = pos_;
            continue;
        }
        if (isWordBody(c)) {
            ++pos_;
            continue;
        }
        // A '.' belongs to the word only while a number is being built, so
        // "4.7kR", "0.002" and the version range "0.2-1.2" stay whole, while
        // "U1.GPIO1" and "U9.1" stop at the dot and parse as the pin references
        // that they are (spec 5.2). Requiring the run so far to be all digits is
        // what separates the two: "4" is a number, "U9" is not.
        if (c == '.' && pos_ > runStart && isDigit(peek(1))) {
            bool numericRun = true;
            for (std::size_t k = runStart; k < pos_; ++k) {
                if (!isDigit(text_[k])) { numericRun = false; break; }
            }
            if (numericRun) {
                ++pos_;
                continue;
            }
        }
        break;
    }

    auto len = static_cast<std::uint32_t>(pos_) - start;
    std::string_view lexeme = text_.substr(start, len);
    WordClass wc = classifyWord(lexeme);

    // The trailing-hyphen rule of spec 2.3 is E-02, but it is *not* reported
    // here. "@VERSION = 1.2-" is a legal upper-bound version constraint
    // (spec 4.3) that lexes as a hyphen-terminated word, so only the parser --
    // which knows whether the word sits in an identifier position or a value
    // position -- can tell an error from a constraint. The flag travels; the
    // decision is the parser's.
    push(out, TokenKind::Word, start, len, wc.flags);
}

// A word inside "$...$": plain identifiers and integers only. Spec 14.5 makes
// '-' always subtraction here, so hyphens never join a word, and a field name
// containing one is written quoted.
void Lexer::scanExprWord(TokenStream& out) {
    auto start = static_cast<std::uint32_t>(pos_);
    while (!atEnd() && (isLetter(peek()) || isDigit(peek()) || peek() == '_')) ++pos_;

    auto len = static_cast<std::uint32_t>(pos_) - start;
    std::string_view lexeme = text_.substr(start, len);

    std::uint16_t flags = 0;
    bool allDigits = true;
    for (char c : lexeme) {
        if (!isDigit(c)) { allDigits = false; break; }
    }
    if (allDigits) flags |= WordFlags::Integer;
    else flags |= WordFlags::Identifier;
    if (lexeme == "true" || lexeme == "TRUE") flags |= WordFlags::BoolTrue;
    if (lexeme == "false" || lexeme == "FALSE") flags |= WordFlags::BoolFalse;

    push(out, TokenKind::Word, start, len, flags);
}

void Lexer::scanToken(TokenStream& out) {
    auto start = static_cast<std::uint32_t>(pos_);
    char c = peek();

    // Multi-byte tokens the language accepts outside comments and strings.
    if (!isAscii(static_cast<unsigned char>(c))) {
        if (startsWithPlusMinus(rest())) {
            pos_ += 2;
            push(out, TokenKind::PlusMinus, start, 2);
            return;
        }
        if (startsWithMicro(rest())) {
            // Spec 3.2: "The prefix u denotes micro; µ is not accepted."
            auto len = static_cast<std::uint32_t>(utf8SequenceLength(static_cast<unsigned char>(c)));
            diags_.report(DiagId::Syntax, spanAt(start, len),
                          std::string("'µ' is not accepted as an SI prefix; write 'u'"));
            pos_ += len;
            push(out, TokenKind::Invalid, start, len);
            return;
        }
        auto len = static_cast<std::uint32_t>(utf8SequenceLength(static_cast<unsigned char>(c)));
        diags_.report(DiagId::Syntax, spanAt(start, len),
                      std::string("non-ASCII character outside a comment or string literal"));
        pos_ += len;
        push(out, TokenKind::Invalid, start, len);
        return;
    }

    if (exprMode_) {
        // Inside "$...$" every operator carries its arithmetic meaning.
        if (isLetter(c) || c == '_' || isDigit(c)) {
            scanExprWord(out);
            return;
        }
        switch (c) {
            case '$': ++pos_; exprMode_ = false; push(out, TokenKind::Dollar, start, 1); return;
            case '"': scanString(out); return;
            case '(': ++pos_; push(out, TokenKind::LParen, start, 1); return;
            case ')': ++pos_; push(out, TokenKind::RParen, start, 1); return;
            case '+': ++pos_; push(out, TokenKind::Plus, start, 1); return;
            case '-': ++pos_; push(out, TokenKind::Minus, start, 1); return;
            case '*': ++pos_; push(out, TokenKind::Star, start, 1); return;
            case '/': ++pos_; push(out, TokenKind::Slash, start, 1); return;
            case '^': ++pos_; push(out, TokenKind::Caret, start, 1); return;
            case '|': ++pos_; push(out, TokenKind::Pipe, start, 1); return;
            case '&': ++pos_; push(out, TokenKind::Amp, start, 1); return;
            case '~': ++pos_; push(out, TokenKind::Tilde, start, 1); return;
            case '=': ++pos_; push(out, TokenKind::Eq, start, 1); return;
            case '!':
                if (peek(1) == '=') { pos_ += 2; push(out, TokenKind::BangEq, start, 2); return; }
                ++pos_; push(out, TokenKind::Bang, start, 1); return;
            case '<':
                if (peek(1) == '=') { pos_ += 2; push(out, TokenKind::Le, start, 2); return; }
                ++pos_; push(out, TokenKind::Lt, start, 1); return;
            case '>':
                if (peek(1) == '=') { pos_ += 2; push(out, TokenKind::Ge, start, 2); return; }
                ++pos_; push(out, TokenKind::Gt, start, 1); return;
            default: break;
        }
        diags_.report(DiagId::Syntax, spanAt(start, 1),
                      std::format("unexpected character '{}' in a substitution", c));
        ++pos_;
        push(out, TokenKind::Invalid, start, 1);
        return;
    }

    // A word starts at a letter, digit or '_', or at a '-' that begins one.
    // A bare '-' followed by a non-word character is the subtraction-shaped
    // token, which outside "$...$" only appears in version constraints.
    if (isWordStart(c) || (c == '-' && isWordStart(peek(1)))) {
        scanWord(out);
        return;
    }

    switch (c) {
        case '"': scanString(out); return;
        case '{': ++pos_; push(out, TokenKind::LBrace, start, 1); return;
        case '}': ++pos_; push(out, TokenKind::RBrace, start, 1); return;
        case '(': ++pos_; push(out, TokenKind::LParen, start, 1); return;
        case ')': ++pos_; push(out, TokenKind::RParen, start, 1); return;
        // '[' and ']' stay single characters. Replication "[[ ]]", counted
        // replication "[N[ ]M]", ranges "[a:b]" and lists "[a,b]" all begin
        // with '[', and the parser separates them with two tokens of lookahead
        // rather than the lexer guessing.
        case '[': ++pos_; push(out, TokenKind::LBracket, start, 1); return;
        case ']': ++pos_; push(out, TokenKind::RBracket, start, 1); return;
        case ';': ++pos_; push(out, TokenKind::Semi, start, 1); return;
        case ',': ++pos_; push(out, TokenKind::Comma, start, 1); return;
        case ':': ++pos_; push(out, TokenKind::Colon, start, 1); return;
        case '.': ++pos_; push(out, TokenKind::Dot, start, 1); return;
        case '~': ++pos_; push(out, TokenKind::Tilde, start, 1); return;
        case '?': ++pos_; push(out, TokenKind::Question, start, 1); return;
        case '%': ++pos_; push(out, TokenKind::Percent, start, 1); return;
        case '#': ++pos_; push(out, TokenKind::Hash, start, 1); return;
        case '@': ++pos_; push(out, TokenKind::At, start, 1); return;
        case '&': ++pos_; push(out, TokenKind::Amp, start, 1); return;
        case '^': ++pos_; push(out, TokenKind::Caret, start, 1); return;
        case '|': ++pos_; push(out, TokenKind::Pipe, start, 1); return;
        case '!': ++pos_; push(out, TokenKind::Bang, start, 1); return;
        case '-': ++pos_; push(out, TokenKind::Minus, start, 1); return;
        case '+':
            // "+-" is the ASCII spelling of "±" (spec 3.3). Multiplicity "+N"
            // always takes a digit, so the two never collide.
            if (peek(1) == '-') { pos_ += 2; push(out, TokenKind::PlusMinus, start, 2); return; }
            ++pos_;
            push(out, TokenKind::Plus, start, 1);
            return;
        case '$':
            ++pos_;
            exprMode_ = true;
            push(out, TokenKind::Dollar, start, 1);
            return;
        case '=':
            if (peek(1) == '=') { pos_ += 2; push(out, TokenKind::EqEq, start, 2); return; }
            if (peek(1) == '*') { pos_ += 2; push(out, TokenKind::EqStar, start, 2); return; }
            ++pos_;
            push(out, TokenKind::Eq, start, 1);
            return;
        case '*':
            if (peek(1) == '=') { pos_ += 2; push(out, TokenKind::StarEq, start, 2); return; }
            ++pos_;
            push(out, TokenKind::Star, start, 1);
            return;
        case '>':
            if (peek(1) == '>') { pos_ += 2; push(out, TokenKind::GtGt, start, 2); return; }
            ++pos_;
            push(out, TokenKind::Gt, start, 1);
            return;
        case '<':
            if (peek(1) == '>') { pos_ += 2; push(out, TokenKind::LtGt, start, 2); return; }
            ++pos_;
            push(out, TokenKind::Lt, start, 1);
            return;
        case '/':
            // Comments were already consumed by skipTrivia, so a bare '/' here
            // has no meaning outside a substitution.
            diags_.report(DiagId::Syntax, spanAt(start, 1),
                          std::string("'/' is only an operator inside a '$...$' substitution"));
            ++pos_;
            push(out, TokenKind::Invalid, start, 1);
            return;
        default: break;
    }

    diags_.report(DiagId::Syntax, spanAt(start, 1),
                  std::format("unexpected character '{}'", c));
    ++pos_;
    push(out, TokenKind::Invalid, start, 1);
}

TokenStream Lexer::run() {
    TokenStream out;
    out.file = file_.id();
    out.tokens.reserve(text_.size() / 4 + 16);

    for (;;) {
        skipTrivia(out);
        if (atEnd()) break;
        std::size_t before = pos_;
        scanToken(out);
        // Every path above advances; this guards against a future edit that
        // forgets to, which would otherwise hang the compiler.
        if (pos_ == before) ++pos_;
    }

    // An unterminated substitution would otherwise swallow the rest of the file
    // in expression mode without explanation.
    if (exprMode_) {
        diags_.report(DiagId::Syntax, spanAt(static_cast<std::uint32_t>(text_.size()), 0),
                      std::string("unterminated substitution; '$...$' requires a closing '$'"));
    }

    out.tokens.push_back(Token{TokenKind::Eof, 0, static_cast<std::uint32_t>(text_.size()), 0});
    return out;
}

}  // namespace manta
