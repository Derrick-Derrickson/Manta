// Deterministic JSON writing and reading.
//
// Hand-rolled rather than pulled in, for one reason above all: spec 15.8
// requires byte-identical output for identical input. That means fixed key
// order (insertion order, never sorted-by-hash), no floating point anywhere in
// the output path, and no library-version-dependent escaping. A general-purpose
// JSON library would have to be constrained into exactly this shape anyway.
//
// Numbers are emitted from integers or from pre-rendered strings; the writer has
// no double overload at all, so a rounding difference between platforms cannot
// reach a .mantaO or .mantaNets file.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace manta {

class JsonWriter {
public:
    explicit JsonWriter(std::string& out, bool pretty = true) : out_(out), pretty_(pretty) {}

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();

    // Names the next value. Only legal inside an object.
    void key(std::string_view k);

    void value(std::string_view s);
    void value(const char* s) { value(std::string_view(s)); }
    void value(std::int64_t n);
    void value(std::uint64_t n);
    void value(int n) { value(static_cast<std::int64_t>(n)); }
    void value(bool b);
    void null();

    // Writes a pre-rendered numeric literal, for values whose exact text matters
    // (canonical dimensioned values are strings, but counts and indices are not).
    void rawNumber(std::string_view literal);

    // Convenience: key + value in one call.
    template <typename T>
    void field(std::string_view k, T&& v) {
        key(k);
        value(std::forward<T>(v));
    }

private:
    void punctuate();
    void newlineIndent();

    std::string& out_;
    bool pretty_;
    int depth_ = 0;
    bool needComma_ = false;
    bool afterKey_ = false;
};

void jsonEscape(std::string& out, std::string_view s);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

class JsonValue;
using JsonPtr = std::unique_ptr<JsonValue>;

enum class JsonKind : std::uint8_t { Null, Bool, Number, String, Array, Object };

class JsonValue {
public:
    JsonKind kind = JsonKind::Null;
    bool boolean = false;
    std::string text;  // String, and the literal text of Number
    std::vector<JsonPtr> array;
    std::vector<std::pair<std::string, JsonPtr>> object;  // insertion order preserved

    [[nodiscard]] const JsonValue* find(std::string_view k) const;
    [[nodiscard]] std::string_view str(std::string_view k, std::string_view fallback = {}) const;
    [[nodiscard]] std::int64_t integer(std::string_view k, std::int64_t fallback = 0) const;
    [[nodiscard]] bool boolean_(std::string_view k, bool fallback = false) const;
    [[nodiscard]] const JsonValue* arr(std::string_view k) const;

    [[nodiscard]] std::int64_t asInteger(std::int64_t fallback = 0) const;
};

struct JsonParseError {
    std::size_t offset = 0;
    std::string message;
};

// Parses UTF-8 JSON. Returns nullptr and fills `error` on failure.
JsonPtr jsonParse(std::string_view text, JsonParseError& error);

}  // namespace manta
