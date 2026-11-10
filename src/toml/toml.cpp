// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "toml/toml.h"

#include <format>

namespace manta {

const TomlValue* TomlValue::find(std::string_view key) const {
    for (const auto& [k, v] : table) {
        if (k == key) return &v;
    }
    return nullptr;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool run(TomlValue& root, std::string& error) {
        root = TomlValue{};
        root.kind = TomlValue::Kind::Table;
        TomlValue* current = &root;
        while (true) {
            skipBlankLines();
            if (atEnd()) break;
            if (peek() == '[') {
                bool arrayOfTables = at(1) == '[';
                pos_ += arrayOfTables ? 2 : 1;
                skipSpace();
                std::string name;
                if (!key(name)) return fail(error);
                skipSpace();
                if (!expect(']') || (arrayOfTables && !expect(']'))) return fail(error);
                if (!endOfLine()) return fail(error);
                current = openTable(root, name, arrayOfTables);
                if (!current) return fail(error);
                continue;
            }
            std::string name;
            if (!key(name)) return fail(error);
            skipSpace();
            if (!expect('=')) return fail(error);
            skipSpace();
            TomlValue value;
            if (!parseValue(value)) return fail(error);
            if (!endOfLine()) return fail(error);
            if (current->find(name)) {
                message_ = std::format("'{}' is set twice", name);
                return fail(error);
            }
            current->table.emplace_back(std::move(name), std::move(value));
        }
        return true;
    }

private:
    bool atEnd() const { return pos_ >= text_.size(); }
    char peek() const { return atEnd() ? '\0' : text_[pos_]; }
    char at(std::size_t ahead) const {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }

    bool fail(std::string& error) const {
        error = std::format("line {}: {}", line_, message_.empty() ? "malformed" : message_);
        return false;
    }

    void skipSpace() {
        while (peek() == ' ' || peek() == '\t') ++pos_;
    }

    void skipComment() {
        if (peek() != '#') return;
        while (!atEnd() && peek() != '\n') ++pos_;
    }

    // Whitespace, comments and newlines, as between statements or inside an
    // array.
    void skipBlankLines() {
        while (true) {
            skipSpace();
            skipComment();
            if (peek() == '\r' && at(1) == '\n') ++pos_;
            if (peek() != '\n') return;
            ++pos_;
            ++line_;
        }
    }

    bool endOfLine() {
        skipSpace();
        skipComment();
        if (peek() == '\r' && at(1) == '\n') ++pos_;
        if (atEnd()) return true;
        if (peek() == '\n') {
            ++pos_;
            ++line_;
            return true;
        }
        message_ = std::format("unexpected '{}' after a value", peek());
        return false;
    }

    bool expect(char c) {
        if (peek() == c) {
            ++pos_;
            return true;
        }
        message_ = atEnd() ? std::format("expected '{}' at end of file", c)
                           : std::format("expected '{}', found '{}'", c, peek());
        return false;
    }

    static bool bareKeyChar(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    }

    bool key(std::string& out) {
        std::size_t start = pos_;
        while (bareKeyChar(peek())) ++pos_;
        out = std::string(text_.substr(start, pos_ - start));
        if (out.empty()) {
            message_ = atEnd() ? "expected a key" : std::format("expected a key, found '{}'", peek());
            return false;
        }
        if (peek() == '.') {
            message_ = std::format("dotted key '{}.' is not supported; use a [table]", out);
            return false;
        }
        return true;
    }

    TomlValue* openTable(TomlValue& root, const std::string& name, bool arrayOfTables) {
        TomlValue* existing = nullptr;
        for (auto& [k, v] : root.table) {
            if (k == name) existing = &v;
        }
        if (arrayOfTables) {
            if (!existing) {
                TomlValue array;
                array.kind = TomlValue::Kind::Array;
                array.line = line_;
                root.table.emplace_back(name, std::move(array));
                existing = &root.table.back().second;
            } else if (existing->kind != TomlValue::Kind::Array) {
                message_ = std::format("'{}' is already a table, not an array of tables", name);
                return nullptr;
            }
            TomlValue table;
            table.line = line_;
            existing->array.push_back(std::move(table));
            return &existing->array.back();
        }
        if (existing) {
            message_ = std::format("table [{}] is declared twice", name);
            return nullptr;
        }
        TomlValue table;
        table.line = line_;
        root.table.emplace_back(name, std::move(table));
        return &root.table.back().second;
    }

    bool hexDigits(std::size_t count, std::uint32_t& out) {
        out = 0;
        for (std::size_t i = 0; i < count; ++i) {
            char c = peek();
            std::uint32_t d = 0;
            if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(c - 'A' + 10);
            else {
                message_ = "a \\u escape needs hex digits";
                return false;
            }
            out = out * 16 + d;
            ++pos_;
        }
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool basicString(std::string& out) {
        ++pos_;  // opening quote
        if (peek() == '"' && at(1) == '"') {
            message_ = "multi-line strings are not supported";
            return false;
        }
        while (true) {
            if (atEnd() || peek() == '\n') {
                message_ = "unterminated string";
                return false;
            }
            char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            char e = peek();
            ++pos_;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                case 'U': {
                    std::uint32_t cp = 0;
                    if (!hexDigits(e == 'u' ? 4 : 8, cp)) return false;
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    message_ = std::format("unknown escape '\\{}'", e);
                    return false;
            }
        }
    }

    bool literalString(std::string& out) {
        ++pos_;
        if (peek() == '\'' && at(1) == '\'') {
            message_ = "multi-line strings are not supported";
            return false;
        }
        while (true) {
            if (atEnd() || peek() == '\n') {
                message_ = "unterminated string";
                return false;
            }
            char c = text_[pos_++];
            if (c == '\'') return true;
            out += c;
        }
    }

    bool parseValue(TomlValue& out) {
        out.line = line_;
        char c = peek();
        if (c == '"') {
            out.kind = TomlValue::Kind::String;
            return basicString(out.string);
        }
        if (c == '\'') {
            out.kind = TomlValue::Kind::String;
            return literalString(out.string);
        }
        if (c == '[') {
            out.kind = TomlValue::Kind::Array;
            ++pos_;
            while (true) {
                skipBlankLines();
                if (peek() == ']') {
                    ++pos_;
                    return true;
                }
                TomlValue element;
                if (!parseValue(element)) return false;
                out.array.push_back(std::move(element));
                skipBlankLines();
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                if (peek() == ']') {
                    ++pos_;
                    return true;
                }
                message_ = atEnd() ? "unterminated array" : "expected ',' or ']' in an array";
                return false;
            }
        }
        if (c == '{') {
            message_ = "inline tables are not supported";
            return false;
        }
        std::size_t start = pos_;
        while (!atEnd() && (bareKeyChar(peek()) || peek() == '+' || peek() == '.' ||
                            peek() == ':')) {
            ++pos_;
        }
        std::string_view word = text_.substr(start, pos_ - start);
        if (word == "true" || word == "false") {
            out.kind = TomlValue::Kind::Bool;
            out.boolean = word == "true";
            return true;
        }
        std::string_view digits = word;
        bool negative = false;
        if (!digits.empty() && (digits[0] == '+' || digits[0] == '-')) {
            negative = digits[0] == '-';
            digits.remove_prefix(1);
        }
        std::int64_t value = 0;
        bool ok = !digits.empty();
        for (char d : digits) {
            if (d == '_') continue;
            if (d < '0' || d > '9' || value > (INT64_MAX - 9) / 10) {
                ok = false;
                break;
            }
            value = value * 10 + (d - '0');
        }
        if (!ok) {
            message_ = word.empty() ? "expected a value"
                                    : std::format("'{}' is not a supported value; quote strings",
                                                  word);
            return false;
        }
        out.kind = TomlValue::Kind::Integer;
        out.integer = negative ? -value : value;
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    int line_ = 1;
    std::string message_;
};

}  // namespace

bool parseToml(std::string_view text, TomlValue& root, std::string& error) {
    return Parser(text).run(root, error);
}

}  // namespace manta
