// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "json/json.h"

#include <charconv>
#include <format>

namespace manta {

void jsonEscape(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    // Multi-byte UTF-8 passes through unchanged; escaping it
                    // would be legal JSON but would not round-trip byte for byte.
                    out += c;
                }
        }
    }
}

void JsonWriter::newlineIndent() {
    if (!pretty_) return;
    out_ += '\n';
    out_.append(static_cast<std::size_t>(depth_) * 2, ' ');
}

void JsonWriter::punctuate() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (needComma_) out_ += ',';
    newlineIndent();
    needComma_ = true;
}

void JsonWriter::beginObject() {
    punctuate();
    out_ += '{';
    ++depth_;
    needComma_ = false;
}

void JsonWriter::endObject() {
    --depth_;
    if (needComma_) newlineIndent();
    out_ += '}';
    needComma_ = true;
}

void JsonWriter::beginArray() {
    punctuate();
    out_ += '[';
    ++depth_;
    needComma_ = false;
}

void JsonWriter::endArray() {
    --depth_;
    if (needComma_) newlineIndent();
    out_ += ']';
    needComma_ = true;
}

void JsonWriter::key(std::string_view k) {
    if (needComma_) out_ += ',';
    newlineIndent();
    out_ += '"';
    jsonEscape(out_, k);
    out_ += "\":";
    if (pretty_) out_ += ' ';
    needComma_ = true;
    afterKey_ = true;
}

void JsonWriter::value(std::string_view s) {
    punctuate();
    out_ += '"';
    jsonEscape(out_, s);
    out_ += '"';
}

void JsonWriter::value(std::int64_t n) {
    punctuate();
    out_ += std::to_string(n);
}

void JsonWriter::value(std::uint64_t n) {
    punctuate();
    out_ += std::to_string(n);
}

void JsonWriter::value(bool b) {
    punctuate();
    out_ += b ? "true" : "false";
}

void JsonWriter::null() {
    punctuate();
    out_ += "null";
}

void JsonWriter::rawNumber(std::string_view literal) {
    punctuate();
    out_ += literal;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

const JsonValue* JsonValue::find(std::string_view k) const {
    for (const auto& [name, v] : object) {
        if (name == k) return v.get();
    }
    return nullptr;
}

std::string_view JsonValue::str(std::string_view k, std::string_view fallback) const {
    const JsonValue* v = find(k);
    return (v && v->kind == JsonKind::String) ? std::string_view(v->text) : fallback;
}

std::int64_t JsonValue::asInteger(std::int64_t fallback) const {
    if (kind != JsonKind::Number) return fallback;
    std::int64_t out = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} ? out : fallback;
}

std::int64_t JsonValue::integer(std::string_view k, std::int64_t fallback) const {
    const JsonValue* v = find(k);
    return v ? v->asInteger(fallback) : fallback;
}

bool JsonValue::boolean_(std::string_view k, bool fallback) const {
    const JsonValue* v = find(k);
    return (v && v->kind == JsonKind::Bool) ? v->boolean : fallback;
}

const JsonValue* JsonValue::arr(std::string_view k) const {
    const JsonValue* v = find(k);
    return (v && v->kind == JsonKind::Array) ? v : nullptr;
}

namespace {

class JsonParser {
public:
    JsonParser(std::string_view text, JsonParseError& error) : s_(text), err_(error) {}

    JsonPtr parse() {
        skipSpace();
        JsonPtr v = parseValue(0);
        if (!v) return nullptr;
        skipSpace();
        if (i_ != s_.size()) {
            fail("trailing content after the top-level value");
            return nullptr;
        }
        return v;
    }

private:
    static constexpr int kMaxDepth = 200;

    void fail(std::string message) {
        if (err_.message.empty()) {
            err_.offset = i_;
            err_.message = std::move(message);
        }
    }

    [[nodiscard]] char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    void skipSpace() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            ++i_;
        }
    }

    bool literal(std::string_view lit) {
        if (s_.compare(i_, lit.size(), lit) == 0) {
            i_ += lit.size();
            return true;
        }
        return false;
    }

    JsonPtr parseValue(int depth) {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
            return nullptr;
        }
        skipSpace();
        char c = peek();
        switch (c) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': return parseString();
            case 't': {
                if (!literal("true")) { fail("bad literal"); return nullptr; }
                auto v = std::make_unique<JsonValue>();
                v->kind = JsonKind::Bool;
                v->boolean = true;
                return v;
            }
            case 'f': {
                if (!literal("false")) { fail("bad literal"); return nullptr; }
                auto v = std::make_unique<JsonValue>();
                v->kind = JsonKind::Bool;
                v->boolean = false;
                return v;
            }
            case 'n': {
                if (!literal("null")) { fail("bad literal"); return nullptr; }
                return std::make_unique<JsonValue>();
            }
            default: return parseNumber();
        }
    }

    JsonPtr parseObject(int depth) {
        ++i_;  // '{'
        auto v = std::make_unique<JsonValue>();
        v->kind = JsonKind::Object;
        skipSpace();
        if (peek() == '}') { ++i_; return v; }
        for (;;) {
            skipSpace();
            if (peek() != '"') { fail("expected a key"); return nullptr; }
            JsonPtr k = parseString();
            if (!k) return nullptr;
            skipSpace();
            if (peek() != ':') { fail("expected ':'"); return nullptr; }
            ++i_;
            JsonPtr val = parseValue(depth + 1);
            if (!val) return nullptr;
            v->object.emplace_back(std::move(k->text), std::move(val));
            skipSpace();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == '}') { ++i_; return v; }
            fail("expected ',' or '}'");
            return nullptr;
        }
    }

    JsonPtr parseArray(int depth) {
        ++i_;  // '['
        auto v = std::make_unique<JsonValue>();
        v->kind = JsonKind::Array;
        skipSpace();
        if (peek() == ']') { ++i_; return v; }
        for (;;) {
            JsonPtr item = parseValue(depth + 1);
            if (!item) return nullptr;
            v->array.push_back(std::move(item));
            skipSpace();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == ']') { ++i_; return v; }
            fail("expected ',' or ']'");
            return nullptr;
        }
    }

    JsonPtr parseString() {
        ++i_;  // '"'
        auto v = std::make_unique<JsonValue>();
        v->kind = JsonKind::String;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '"') {
                ++i_;
                return v;
            }
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) break;
                switch (s_[i_]) {
                    case '"': v->text += '"'; break;
                    case '\\': v->text += '\\'; break;
                    case '/': v->text += '/'; break;
                    case 'b': v->text += '\b'; break;
                    case 'f': v->text += '\f'; break;
                    case 'n': v->text += '\n'; break;
                    case 'r': v->text += '\r'; break;
                    case 't': v->text += '\t'; break;
                    case 'u': {
                        if (i_ + 4 >= s_.size()) { fail("truncated \\u escape"); return nullptr; }
                        unsigned cp = 0;
                        for (int k = 1; k <= 4; ++k) {
                            char h = s_[i_ + static_cast<std::size_t>(k)];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else { fail("bad \\u escape"); return nullptr; }
                        }
                        i_ += 4;
                        // Encode as UTF-8. Surrogate pairs are not reassembled;
                        // manta never emits them, and the schema forbids them.
                        if (cp < 0x80) {
                            v->text += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            v->text += static_cast<char>(0xC0 | (cp >> 6));
                            v->text += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            v->text += static_cast<char>(0xE0 | (cp >> 12));
                            v->text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            v->text += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: fail("unknown escape"); return nullptr;
                }
                ++i_;
                continue;
            }
            v->text += c;
            ++i_;
        }
        fail("unterminated string");
        return nullptr;
    }

    JsonPtr parseNumber() {
        std::size_t start = i_;
        if (peek() == '-') ++i_;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' ||
                                  s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' ||
                                  s_[i_] == '-')) {
            ++i_;
        }
        if (i_ == start) {
            fail("expected a value");
            return nullptr;
        }
        auto v = std::make_unique<JsonValue>();
        v->kind = JsonKind::Number;
        v->text = std::string(s_.substr(start, i_ - start));
        return v;
    }

    std::string_view s_;
    std::size_t i_ = 0;
    JsonParseError& err_;
};

}  // namespace

JsonPtr jsonParse(std::string_view text, JsonParseError& error) {
    error = JsonParseError{};
    JsonParser p(text, error);
    JsonPtr v = p.parse();
    if (!v && error.message.empty()) error.message = "invalid JSON";
    return v;
}

}  // namespace manta
