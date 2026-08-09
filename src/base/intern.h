// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// String interning.
//
// Identifiers dominate a manta source file and are compared constantly (net
// names, pin names, part names, field names). Interning turns every comparison
// into a 32-bit integer compare and every name into 4 bytes inside an AST node.
//
// Spec 2.2 makes identifiers case sensitive, so interning is exact-match: no
// case folding anywhere.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/arena.h"
#include "base/flat_map.h"
#include "base/hash.h"

namespace manta {

// An interned identifier. Index 0 is reserved for "no symbol".
enum class SymbolId : std::uint32_t { kInvalid = 0 };

[[nodiscard]] constexpr bool valid(SymbolId s) noexcept { return s != SymbolId::kInvalid; }
[[nodiscard]] constexpr std::uint32_t raw(SymbolId s) noexcept {
    return static_cast<std::uint32_t>(s);
}

class StringInterner {
public:
    StringInterner() {
        // Slot 0 == kInvalid, so a default-constructed SymbolId is falsy.
        strings_.emplace_back(std::string_view{});
        rehash(1024);
    }

    [[nodiscard]] SymbolId intern(std::string_view s) {
        std::uint64_t h = fnv1a(s);
        std::size_t mask = slots_.size() - 1;
        std::size_t i = static_cast<std::size_t>(h) & mask;
        for (;;) {
            std::uint32_t slot = slots_[i];
            if (slot == 0) break;
            if (strings_[slot] == s) return static_cast<SymbolId>(slot);
            i = (i + 1) & mask;
        }

        // Copy the text into arena storage so the view stays valid for the
        // lifetime of the interner regardless of where it came from.
        auto stored = arena_.makeArray<char>(s.size());
        for (std::size_t k = 0; k < s.size(); ++k) stored[k] = s[k];

        auto id = static_cast<std::uint32_t>(strings_.size());
        strings_.emplace_back(stored.data(), stored.size());
        slots_[i] = id;
        if (strings_.size() * 4 >= slots_.size() * 3) rehash(slots_.size() * 2);
        return static_cast<SymbolId>(id);
    }

    [[nodiscard]] std::string_view text(SymbolId id) const {
        std::uint32_t i = raw(id);
        return i < strings_.size() ? strings_[i] : std::string_view{};
    }

    // Interns the concatenation of parts without a temporary std::string when
    // the result already exists. Used by substitution rendering, which builds
    // identifiers piecewise.
    [[nodiscard]] SymbolId internConcat(std::string_view a, std::string_view b) {
        std::string tmp;
        tmp.reserve(a.size() + b.size());
        tmp += a;
        tmp += b;
        return intern(tmp);
    }

    [[nodiscard]] std::size_t count() const noexcept { return strings_.size() - 1; }

private:
    void rehash(std::size_t n) {
        std::size_t size = 16;
        while (size < n) size <<= 1;
        slots_.assign(size, 0u);
        std::size_t mask = size - 1;
        for (std::uint32_t id = 1; id < strings_.size(); ++id) {
            std::size_t i = static_cast<std::size_t>(fnv1a(strings_[id])) & mask;
            while (slots_[i] != 0) i = (i + 1) & mask;
            slots_[i] = id;
        }
    }

    Arena arena_{16 * 1024};
    std::vector<std::string_view> strings_;
    std::vector<std::uint32_t> slots_;
};

}  // namespace manta

template <>
struct manta::DefaultHash<manta::SymbolId> {
    std::uint64_t operator()(manta::SymbolId s) const noexcept {
        return mix64(static_cast<std::uint64_t>(manta::raw(s)));
    }
};
