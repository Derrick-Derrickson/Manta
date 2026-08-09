// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic hashing.
//
// Spec 15.8 requires byte-identical output for identical input, so every hash
// used anywhere near an emitted ordering must be stable across runs, builds and
// platforms. std::hash gives none of those guarantees (libstdc++ and MSVC differ,
// and some implementations salt per-process), so manta uses its own.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace manta {

// FNV-1a, 64-bit. Small, branch-free, and adequate for identifier-length keys.
[[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ull;
    }
    return h;
}

// Integer finaliser (splitmix64). Used for hashing ids rather than strings.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

[[nodiscard]] constexpr std::uint64_t hashCombine(std::uint64_t a, std::uint64_t b) noexcept {
    return mix64(a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)));
}

}  // namespace manta
