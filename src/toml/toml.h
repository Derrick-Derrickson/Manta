// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The subset of TOML a BOM template needs.
//
// A template is TOML so that one file can serve other tools that read TOML,
// rather than manta inventing a syntax of its own. Manta takes no
// third-party dependencies, and a template needs little of the language:
//
//   # comments                 key = value          [table]      [[array]]
//   "basic\tstrings"           'literal strings'    integers     true / false
//   [arrays, "of", [nested]]   spanning lines, trailing comma allowed
//
// Not supported, and refused with the line number rather than misread: dotted
// and quoted keys, inline tables, multi-line strings, floats and dates. Tables
// keep their keys in file order, so anything derived from one is
// deterministic.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manta {

struct TomlValue {
    enum class Kind : std::uint8_t { String, Integer, Bool, Array, Table };
    Kind kind = Kind::Table;
    std::string string;
    std::int64_t integer = 0;
    bool boolean = false;
    std::vector<TomlValue> array;
    std::vector<std::pair<std::string, TomlValue>> table;  // file order
    int line = 0;  // where the value began, for messages

    [[nodiscard]] const TomlValue* find(std::string_view key) const;
};

// Parses `text` into a root table. On failure returns false and sets `error`
// to "line N: what went wrong".
[[nodiscard]] bool parseToml(std::string_view text, TomlValue& root, std::string& error);

}  // namespace manta
