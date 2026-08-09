// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Insertion-ordered hash map and set.
//
// Iteration order is the order in which keys were first inserted, never bucket
// order. That is the whole point: spec 15.8 demands byte-identical output, and
// std::unordered_map's iteration order depends on the standard library, the
// bucket count and the insertion history in ways that are not portable. Here,
// entries live in a dense vector and the open-addressed index table only ever
// points into it, so iterating is both deterministic and cache-friendly.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
#include <utility>
#include <vector>

#include "base/hash.h"

namespace manta {

namespace detail {
constexpr std::uint32_t kEmptySlot = 0xFFFFFFFFu;

inline std::size_t nextPow2(std::size_t n) {
    std::size_t p = 8;
    while (p < n) p <<= 1;
    return p;
}
}  // namespace detail

template <typename K>
struct DefaultHash {
    std::uint64_t operator()(const K& k) const noexcept {
        if constexpr (std::is_integral_v<K> || std::is_enum_v<K>) {
            return mix64(static_cast<std::uint64_t>(k));
        } else {
            return fnv1a(k);
        }
    }
};

template <typename K, typename V, typename Hash = DefaultHash<K>>
class FlatMap {
public:
    struct Entry {
        K key;
        V value;
    };

    FlatMap() = default;

    void reserve(std::size_t n) {
        entries_.reserve(n);
        rehash(detail::nextPow2(n * 2));
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    [[nodiscard]] V* find(const K& key) {
        if (entries_.empty()) return nullptr;
        std::uint32_t idx = lookup(key);
        return idx == detail::kEmptySlot ? nullptr : &entries_[idx].value;
    }

    [[nodiscard]] const V* find(const K& key) const {
        return const_cast<FlatMap*>(this)->find(key);
    }

    [[nodiscard]] bool contains(const K& key) const { return find(key) != nullptr; }

    // Inserts if absent. Returns {slot, inserted}.
    std::pair<V*, bool> insert(const K& key, V value) {
        if (slots_.empty()) rehash(16);
        std::uint32_t idx = lookup(key);
        if (idx != detail::kEmptySlot) return {&entries_[idx].value, false};

        auto pos = static_cast<std::uint32_t>(entries_.size());
        entries_.push_back(Entry{key, std::move(value)});
        place(key, pos);
        if (entries_.size() * 4 >= slots_.size() * 3) rehash(slots_.size() * 2);
        return {&entries_.back().value, true};
    }

    // Inserts or overwrites.
    V& set(const K& key, V value) {
        auto [slot, inserted] = insert(key, std::move(value));
        if (!inserted) *slot = std::move(value);
        return *slot;
    }

    V& operator[](const K& key) { return *insert(key, V{}).first; }

    // Iteration is insertion order, by construction.
    auto begin() { return entries_.begin(); }
    auto end() { return entries_.end(); }
    auto begin() const { return entries_.begin(); }
    auto end() const { return entries_.end(); }

    [[nodiscard]] std::span<const Entry> entries() const { return entries_; }
    [[nodiscard]] std::span<Entry> entries() { return entries_; }

    void clear() {
        entries_.clear();
        slots_.assign(slots_.size(), detail::kEmptySlot);
    }

private:
    [[nodiscard]] std::uint32_t lookup(const K& key) const {
        if (slots_.empty()) return detail::kEmptySlot;
        std::size_t mask = slots_.size() - 1;
        std::size_t i = static_cast<std::size_t>(Hash{}(key)) & mask;
        for (;;) {
            std::uint32_t s = slots_[i];
            if (s == detail::kEmptySlot) return detail::kEmptySlot;
            if (entries_[s].key == key) return s;
            i = (i + 1) & mask;
        }
    }

    void place(const K& key, std::uint32_t pos) {
        std::size_t mask = slots_.size() - 1;
        std::size_t i = static_cast<std::size_t>(Hash{}(key)) & mask;
        while (slots_[i] != detail::kEmptySlot) i = (i + 1) & mask;
        slots_[i] = pos;
    }

    void rehash(std::size_t n) {
        slots_.assign(detail::nextPow2(n), detail::kEmptySlot);
        for (std::uint32_t p = 0; p < entries_.size(); ++p) place(entries_[p].key, p);
    }

    std::vector<Entry> entries_;
    std::vector<std::uint32_t> slots_;
};

template <typename K, typename Hash = DefaultHash<K>>
class FlatSet {
public:
    void reserve(std::size_t n) { map_.reserve(n); }
    bool insert(const K& k) { return map_.insert(k, std::monostate{}).second; }
    [[nodiscard]] bool contains(const K& k) const { return map_.contains(k); }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] bool empty() const noexcept { return map_.empty(); }
    void clear() { map_.clear(); }

    class Iter {
    public:
        explicit Iter(typename FlatMap<K, std::monostate, Hash>::Entry* p) : p_(p) {}
        const K& operator*() const { return p_->key; }
        Iter& operator++() { ++p_; return *this; }
        bool operator!=(const Iter& o) const { return p_ != o.p_; }

    private:
        typename FlatMap<K, std::monostate, Hash>::Entry* p_;
    };

    auto begin() { return Iter(map_.entries().data()); }
    auto end() { return Iter(map_.entries().data() + map_.size()); }

private:
    FlatMap<K, std::monostate, Hash> map_;
};

}  // namespace manta
