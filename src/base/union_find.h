// Disjoint-set forest, used to merge nodes into nets.
//
// Chain evaluation (spec 6) is almost entirely a sequence of "these two things
// are the same net" assertions, which is exactly what union-find is for. With
// path halving and union by size the whole netlist build is effectively linear.
//
// Roots are chosen by size, so the representative of a set is not stable across
// unions; anything order-sensitive keys off the *smallest member index* instead,
// which is stable and therefore safe for deterministic output (spec 15.8).
#pragma once

#include <cstdint>
#include <numeric>
#include <vector>

namespace manta {

class UnionFind {
public:
    UnionFind() = default;
    explicit UnionFind(std::size_t n) { reset(n); }

    void reset(std::size_t n) {
        parent_.resize(n);
        std::iota(parent_.begin(), parent_.end(), std::uint32_t{0});
        size_.assign(n, 1u);
    }

    std::uint32_t add() {
        auto id = static_cast<std::uint32_t>(parent_.size());
        parent_.push_back(id);
        size_.push_back(1);
        return id;
    }

    [[nodiscard]] std::uint32_t find(std::uint32_t x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];  // path halving
            x = parent_[x];
        }
        return x;
    }

    // Returns the new root, or kNoMerge semantics: if already joined, the root.
    std::uint32_t unite(std::uint32_t a, std::uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return a;
        if (size_[a] < size_[b]) std::swap(a, b);
        parent_[b] = a;
        size_[a] += size_[b];
        return a;
    }

    [[nodiscard]] bool joined(std::uint32_t a, std::uint32_t b) { return find(a) == find(b); }
    [[nodiscard]] std::size_t count() const noexcept { return parent_.size(); }
    [[nodiscard]] std::uint32_t setSize(std::uint32_t x) { return size_[find(x)]; }

private:
    std::vector<std::uint32_t> parent_;
    std::vector<std::uint32_t> size_;
};

}  // namespace manta
