// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Sheet tiling: flow columns (WP6).
//
// The reference schematics read as a few vertical columns whose borders
// partition the sheet -- every border meets a neighbour or the sheet edge.
// This tiler reproduces that shape:
//
//   1. Reading order: room indices sorted by (pull ascending, page order),
//      stable, so indifferent rooms keep page order.
//   2. Column count: for each k, the reading order is split into k contiguous
//      columns balanced by a binary search over the per-column height cap
//      (the smallest cap a k-column greedy fill can meet -- the classic
//      linear-partition answer, integers only). Among the k whose summed
//      column widths fit the hard budget max(targetW, widest room), the one
//      whose bounding box lands nearest the landscape reference aspect
//      (W : H = sqrt(2) : 1, taken as 141 : 100) wins; ties go to fewer
//      columns. k = 1 always fits, so there is always an answer.
//   3. Assignment: walk the reading order left to right, stacking into the
//      current column until the next room would push it past the cap, then
//      advance. Order is never changed -- flow reads left to right across
//      columns, top to bottom within one. When inter-room counts exist, one
//      deterministic accept-first sweep may swap two equal-pull rooms of
//      adjacent columns if that strictly raises the summed counts between
//      rooms of horizontally adjacent columns without breaking the width
//      budget or the bounding-box height.
//   4. Stretch to partition: every member takes its column's width (max
//      member width); each column's members are stretched so the column
//      exactly fills the tallest column's height, the deficit shared in
//      proportion to each member's own height (integer division, remainder
//      to the last member). The result partitions its bounding box exactly:
//      bbox = (sum of column widths) x (tallest column height).
//
// Empty result means "use the classic shelf packing"; only the vacuous
// zero-room input takes that path. Integer arithmetic throughout, no
// floats, deterministic iteration, explicit tie-breaks (spec 15.8).
#include "render/tile.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <numeric>

namespace manta::render {

namespace {

// ceil(a / b) for a >= 0, b > 0.
std::int64_t ceilDiv(std::int64_t a, std::int64_t b) { return (a + b - 1) / b; }

// A column holds room indices, reading order preserved top to bottom.
using Columns = std::vector<std::vector<std::size_t>>;

std::int64_t columnWidth(const std::vector<RoomExtent>& rooms,
                         const std::vector<std::size_t>& col) {
    std::int64_t w = 0;
    for (std::size_t i : col) w = std::max(w, static_cast<std::int64_t>(rooms[i].w));
    return w;
}

std::int64_t columnHeight(const std::vector<RoomExtent>& rooms,
                          const std::vector<std::size_t>& col) {
    std::int64_t h = 0;
    for (std::size_t i : col) h += rooms[i].h;
    return h;
}

std::int64_t totalWidth(const std::vector<RoomExtent>& rooms, const Columns& cols) {
    std::int64_t w = 0;
    for (const auto& c : cols) w += columnWidth(rooms, c);
    return w;
}

std::int64_t tallestColumn(const std::vector<RoomExtent>& rooms, const Columns& cols) {
    std::int64_t h = 0;
    for (const auto& c : cols) h = std::max(h, columnHeight(rooms, c));
    return h;
}

// Greedy fill of the reading order into at most k columns: stack into the
// current column until the next room would push it past `threshold`, then
// advance. Every opened column takes at least one room; once column k-1 is
// reached everything left stacks there. Trailing empty columns are dropped.
Columns assignGreedy(const std::vector<RoomExtent>& rooms, const std::vector<std::size_t>& order,
                     std::size_t k, std::int64_t threshold) {
    Columns cols(k);
    std::size_t c = 0;
    std::int64_t stacked = 0;
    for (std::size_t idx : order) {
        const std::int64_t h = rooms[idx].h;
        if (!cols[c].empty() && c + 1 < k && stacked + h > threshold) {
            ++c;
            stacked = 0;
        }
        cols[c].push_back(idx);
        stacked += h;
    }
    while (!cols.empty() && cols.back().empty()) cols.pop_back();
    return cols;
}

}  // namespace

std::vector<RoomPlace> tileRooms(const std::vector<RoomExtent>& rooms, const InterRoomFlow& flow,
                                 int targetW) {
    const std::size_t n = rooms.size();
    if (n == 0) return {};  // vacuous: the classic fallback packs nothing too

    // Reading direction: pull ascending, page order breaking ties (stable).
    // An absent or malformed pull vector means "no preference" -- page order.
    std::vector<int> pull(n, 0);
    if (flow.pull.size() == n) pull = flow.pull;
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&pull](std::size_t a, std::size_t b) { return pull[a] < pull[b]; });

    // The width budget is hard: max(targetW, widest room). Within it, the
    // column count is chosen by the shape it produces, not by a height cap.
    std::int64_t totalH = 0, widest = 0, tallest = 0;
    for (const RoomExtent& r : rooms) {
        totalH += r.h;
        widest = std::max(widest, static_cast<std::int64_t>(r.w));
        tallest = std::max(tallest, static_cast<std::int64_t>(r.h));
    }
    const std::int64_t budgetW =
        std::max({static_cast<std::int64_t>(targetW), widest, std::int64_t{1}});

    // The smallest per-column height cap a k-column contiguous fill of the
    // reading order can meet: greedy count over a binary-searched cap (the
    // linear-partition minimum, integer arithmetic only). assignGreedy with
    // that cap reproduces the counted fill exactly.
    auto columnsUnder = [&](std::int64_t cap) {
        std::size_t c = 1;
        std::int64_t stacked = 0;
        bool empty = true;
        for (std::size_t idx : order) {
            const std::int64_t h = rooms[idx].h;
            if (!empty && stacked + h > cap) {
                ++c;
                stacked = 0;
            }
            stacked += h;
            empty = false;
        }
        return c;
    };
    auto fillBalanced = [&](std::size_t k) {
        std::int64_t lo = std::max(tallest, ceilDiv(totalH, static_cast<std::int64_t>(k)));
        std::int64_t hi = totalH;
        while (lo < hi) {
            const std::int64_t mid = lo + (hi - lo) / 2;
            if (columnsUnder(mid) <= k) hi = mid;
            else lo = mid + 1;
        }
        return assignGreedy(rooms, order, k, lo);
    };

    // Aspect selection: |W/H - 141/100| compared by cross-multiplication --
    // |100*W - 141*H| weighted by the rival's H -- so no division and no
    // float ever decides a tie. Fewer columns win an exact tie.
    Columns cols;
    std::int64_t bestW = 0, bestH = 1;
    bool have = false;
    for (std::size_t k = 1; k <= n; ++k) {
        Columns c = fillBalanced(k);
        if (c.size() != k) continue;  // a duplicate of a smaller k
        const std::int64_t w = totalWidth(rooms, c);
        if (k > 1 && w > budgetW) continue;  // k = 1 always fits
        const std::int64_t h = std::max(tallestColumn(rooms, c), std::int64_t{1});
        if (!have) {
            cols = std::move(c);
            bestW = w;
            bestH = h;
            have = true;
            continue;
        }
        const std::int64_t distNew = std::abs(100 * w - 141 * h) * bestH;
        const std::int64_t distOld = std::abs(100 * bestW - 141 * bestH) * h;
        if (distNew < distOld) {
            cols = std::move(c);
            bestW = w;
            bestH = h;
        }
    }

    // Adjacency pass: one deterministic accept-first sweep. Swapping two
    // rooms of adjacent columns is accepted when it strictly raises the
    // summed counts between rooms of horizontally adjacent columns, keeps
    // both budgets, and exchanges equal pulls only -- a room that asked for
    // an edge is never traded away from it.
    const bool countsUsable = [&] {
        if (flow.counts.size() != n || n < 2 || cols.size() < 2) return false;
        for (const auto& row : flow.counts)
            if (row.size() != n) return false;
        return true;
    }();
    if (countsUsable) {
        const auto adjacentSum = [&](const Columns& c) {
            std::int64_t s = 0;
            for (std::size_t ci = 0; ci + 1 < c.size(); ++ci)
                for (std::size_t a : c[ci])
                    for (std::size_t b : c[ci + 1]) s += flow.counts[a][b];
            return s;
        };
        std::int64_t current = adjacentSum(cols);
        for (std::size_t ci = 0; ci + 1 < cols.size(); ++ci) {
            for (std::size_t i = 0; i < cols[ci].size(); ++i) {
                for (std::size_t j = 0; j < cols[ci + 1].size(); ++j) {
                    const std::size_t u = cols[ci][i], v = cols[ci + 1][j];
                    if (pull[u] != pull[v]) continue;
                    const std::int64_t oldTallest = tallestColumn(rooms, cols);
                    std::swap(cols[ci][i], cols[ci + 1][j]);
                    const bool fits = totalWidth(rooms, cols) <= budgetW &&
                                      tallestColumn(rooms, cols) <= oldTallest;
                    const std::int64_t swapped = fits ? adjacentSum(cols) : -1;
                    if (fits && swapped > current)
                        current = swapped;  // keep the swap
                    else
                        std::swap(cols[ci][i], cols[ci + 1][j]);  // revert
                }
            }
        }
    }

    // Stretch to an exact partition of bbox = (sum of widths) x (tallest
    // column). Members take their column's width; each column's height
    // deficit is shared in proportion to member height, the integer-division
    // remainder going to the last member by rule.
    std::vector<std::int64_t> colW(cols.size()), colH(cols.size());
    std::int64_t sheetH = 0;
    for (std::size_t c = 0; c < cols.size(); ++c) {
        colW[c] = columnWidth(rooms, cols[c]);
        colH[c] = columnHeight(rooms, cols[c]);
        sheetH = std::max(sheetH, colH[c]);
    }

    std::vector<RoomPlace> out(n);
    std::int64_t x = 0;
    for (std::size_t c = 0; c < cols.size(); ++c) {
        const std::int64_t deficit = sheetH - colH[c];
        std::int64_t y = 0, given = 0;
        for (std::size_t m = 0; m < cols[c].size(); ++m) {
            const std::size_t idx = cols[c][m];
            const std::int64_t h = rooms[idx].h;
            const std::int64_t share = (m + 1 == cols[c].size()) ? deficit - given
                                       : (colH[c] > 0)           ? deficit * h / colH[c]
                                                                 : std::int64_t{0};
            given += share;
            out[idx].x = static_cast<int>(x);
            out[idx].y = static_cast<int>(y);
            out[idx].w = static_cast<int>(colW[c]);
            out[idx].h = static_cast<int>(h + share);
            y += h + share;
        }
        x += colW[c];
    }
    return out;
}

}  // namespace manta::render
