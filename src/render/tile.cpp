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
//   2. Column count: the smallest k whose greedy height-balanced fill keeps
//      every column at or under the height the width budget implies
//      (ceil(area / budget) plus 25% slack) with the column widths summing
//      within the budget max(targetW, widest room). If targetW is generous
//      enough that no k <= 4 can meet the height cap, more columns are
//      allowed -- that is targetW demanding them. If no k meets the height
//      cap at all (the width budget binds), fall back to the best-balanced
//      k in 1..4 that fits the width budget; k = 1 always does.
//   3. Assignment: walk the reading order left to right, stacking into the
//      current column until the next room would push it past the cap, then
//      advance. Order is never changed -- flow reads left to right across
//      columns, top to bottom within one. When inter-room counts exist, one
//      deterministic accept-first sweep may swap two equal-pull rooms of
//      adjacent columns if that strictly raises the summed counts between
//      rooms of horizontally adjacent columns without breaking the budgets.
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

    // Budgets. Width: the classic targetW, never below the widest room.
    // Height: the balanced height that width implies, plus 25% slack.
    std::int64_t area = 0, totalH = 0, widest = 0;
    for (const RoomExtent& r : rooms) {
        area += static_cast<std::int64_t>(r.w) * r.h;
        totalH += r.h;
        widest = std::max(widest, static_cast<std::int64_t>(r.w));
    }
    const std::int64_t budgetW = std::max({static_cast<std::int64_t>(targetW), widest,
                                           std::int64_t{1}});
    const std::int64_t hStar = ceilDiv(area, budgetW);
    const std::int64_t hCap = hStar + hStar / 4;

    // Column count: smallest k meeting both budgets. Raising the greedy
    // threshold to ceil(totalH / k) when that exceeds hCap never hides a
    // feasible k -- by pigeonhole some column of any k-way split reaches
    // totalH / k -- and keeps the infeasible fills balanced for the
    // fallback below.
    Columns cols;
    bool feasible = false;
    for (std::size_t k = 1; k <= n; ++k) {
        const std::int64_t threshold =
            std::max(hCap, ceilDiv(totalH, static_cast<std::int64_t>(k)));
        Columns c = assignGreedy(rooms, order, k, threshold);
        if (totalWidth(rooms, c) <= budgetW && tallestColumn(rooms, c) <= hCap) {
            cols = std::move(c);
            feasible = true;
            break;
        }
    }
    if (!feasible) {
        // The width budget binds: no k meets the height cap. Take the
        // best-balanced fill within the cap of four columns; smallest k
        // wins a tie, and k = 1 always fits the width budget.
        std::int64_t bestH = -1;
        for (std::size_t k = 1; k <= std::min<std::size_t>(n, 4); ++k) {
            const std::int64_t threshold =
                std::max(hCap, ceilDiv(totalH, static_cast<std::int64_t>(k)));
            Columns c = assignGreedy(rooms, order, k, threshold);
            if (totalWidth(rooms, c) > budgetW) continue;
            const std::int64_t h = tallestColumn(rooms, c);
            if (bestH < 0 || h < bestH) {
                bestH = h;
                cols = std::move(c);
            }
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
                                      tallestColumn(rooms, cols) <= std::max(hCap, oldTallest);
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
