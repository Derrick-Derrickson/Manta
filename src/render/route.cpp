// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The room-local router of the Flow pipeline (WP5).
//
// Cosmetic only: a false return leaves the RoomBuf untouched and the caller
// keeps the stub+label fallback, which is always electrically correct. The
// router therefore builds everything in local vectors and commits in a single
// block at the end, after every pin has joined.
//
// Determinism (spec 15.8): integer math throughout, a bucket queue instead of
// std::priority_queue, FIFO order within a cost bucket, and a fixed neighbour
// expansion order. Equal-cost routes therefore resolve identically on every
// run, with no tie left to container or platform whim.
#include "render/route.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

namespace manta::render {

namespace {

// Neighbour expansion order is part of the tie-break rule: Right, Down, Left,
// Up, always.
constexpr int kDx[4] = {1, 0, -1, 0};
constexpr int kDy[4] = {0, 1, 0, -1};
constexpr int kNoDir = 4;  // a seed state: the first step turns for free
constexpr std::uint32_t kStateDirs = 5;

constexpr std::int32_t kStepCost = 1;  // per P step
constexpr std::int32_t kTurnCost = 5;  // per 90-degree turn
constexpr int kPopCap = 20000;         // popped states per routeNet call

// Foreign-net wire segments split by axis: a step may cross one square-on --
// ordinary schematic drawing -- but never run collinearly along it, which
// would draw two nets as one conductor.
struct HSpan {
    int y = 0, x1 = 0, x2 = 0;  // x1 < x2
};
struct VSpan {
    int x = 0, y1 = 0, y2 = 0;  // y1 < y2
};

struct Router {
    const RoomBuf& buf;
    int gx1, gy1;  // grid clip, inclusive: cells at pitch P over (0,0)..(gx1,gy1)
    int nx, ny;    // grid size in cells
    std::vector<HSpan> hSpans;
    std::vector<VSpan> vSpans;
    std::vector<char> tree;  // cell -> covered by this net's new segments
    int pops = 0;

    [[nodiscard]] std::uint32_t stateOf(int x, int y, int d) const {
        const std::uint32_t cell =
            static_cast<std::uint32_t>(y / P) * static_cast<std::uint32_t>(nx) +
            static_cast<std::uint32_t>(x / P);
        return cell * kStateDirs + static_cast<std::uint32_t>(d);
    }

    // A step is legal when its swept centre line clears every solid and does
    // not run along a foreign-net wire. The probe is a degenerate rect, which
    // `overlaps` reads as "strictly astride the line": a solid merely touched
    // edge-on does not block, which is exactly how a stub lands on a body
    // edge, and a perpendicular wire crossing trips neither test.
    [[nodiscard]] bool stepLegal(int x, int y, int d) const {
        const int tx = x + kDx[d] * P;
        const int ty = y + kDy[d] * P;
        if (tx < 0 || ty < 0 || tx > gx1 || ty > gy1) return false;
        if (kDy[d] == 0) {
            const int xa = std::min(x, tx), xb = std::max(x, tx);
            if (buf.collides(Rect{xa, y, xb, y})) return false;
            for (const HSpan& s : hSpans) {
                if (s.y == y && std::max(xa, s.x1) < std::min(xb, s.x2)) return false;
            }
        } else {
            const int ya = std::min(y, ty), yb = std::max(y, ty);
            if (buf.collides(Rect{x, ya, x, yb})) return false;
            for (const VSpan& s : vSpans) {
                if (s.x == x && std::max(ya, s.y1) < std::min(yb, s.y2)) return false;
            }
        }
        return true;
    }

    // Dijkstra over (cell, entry-direction) states on a bucket queue: costs
    // are small integers, so a vector of FIFO deques indexed by cost replaces
    // the forbidden priority queue and makes every tie fall to insertion
    // order. The first goal state POPPED is the answer -- minimal cost by
    // bucket order, deterministic among equals by FIFO. `toTree` searches
    // accept any tree cell as the goal; otherwise the goal is the single cell
    // (gx,gy). On success `outCells` holds every path cell, seed to goal.
    [[nodiscard]] bool search(int sx, int sy, bool toTree, int gx, int gy,
                              std::vector<int>& outCells) {
        const std::size_t nStates =
            static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) * kStateDirs;
        std::vector<std::int32_t> dist(nStates, -1);
        std::vector<std::int32_t> parent(nStates, -1);
        std::vector<std::deque<std::uint32_t>> buckets(1);

        const std::uint32_t start = stateOf(sx, sy, kNoDir);
        dist[start] = 0;
        buckets[0].push_back(start);

        std::int64_t goal = -1;
        for (std::size_t c = 0; c < buckets.size() && goal < 0; ++c) {
            while (!buckets[c].empty()) {
                const std::uint32_t s = buckets[c].front();
                buckets[c].pop_front();
                if (dist[s] != static_cast<std::int32_t>(c)) continue;  // superseded
                if (++pops > kPopCap) return false;
                const std::uint32_t cell = s / kStateDirs;
                const int d = static_cast<int>(s % kStateDirs);
                const int x = static_cast<int>(cell % static_cast<std::uint32_t>(nx)) * P;
                const int y = static_cast<int>(cell / static_cast<std::uint32_t>(nx)) * P;
                if (toTree ? tree[cell] != 0 : (x == gx && y == gy)) {
                    goal = s;
                    break;
                }
                for (int nd = 0; nd < 4; ++nd) {
                    if (!stepLegal(x, y, nd)) continue;
                    const std::int32_t cost = static_cast<std::int32_t>(c) + kStepCost +
                                              (d != kNoDir && d != nd ? kTurnCost : 0);
                    const std::uint32_t t = stateOf(x + kDx[nd] * P, y + kDy[nd] * P, nd);
                    // Strictly-better only: an equal-cost rival keeps the
                    // earlier insertion, so ties are stable.
                    if (dist[t] >= 0 && dist[t] <= cost) continue;
                    dist[t] = cost;
                    parent[t] = static_cast<std::int32_t>(s);
                    if (static_cast<std::size_t>(cost) >= buckets.size()) {
                        buckets.resize(static_cast<std::size_t>(cost) + 1);
                    }
                    buckets[static_cast<std::size_t>(cost)].push_back(t);
                }
            }
        }
        if (goal < 0) return false;

        std::vector<int> rev;  // goal-first x,y pairs off the parent chain
        for (std::int64_t s = goal; s >= 0; s = parent[static_cast<std::size_t>(s)]) {
            const std::uint32_t cell = static_cast<std::uint32_t>(s) / kStateDirs;
            rev.push_back(static_cast<int>(cell % static_cast<std::uint32_t>(nx)) * P);
            rev.push_back(static_cast<int>(cell / static_cast<std::uint32_t>(nx)) * P);
        }
        for (std::size_t i = rev.size(); i >= 2; i -= 2) {
            outCells.push_back(rev[i - 2]);
            outCells.push_back(rev[i - 1]);
        }
        return true;
    }

    void markTree(const std::vector<int>& cells) {
        for (std::size_t i = 0; i + 1 < cells.size(); i += 2) {
            tree[stateOf(cells[i], cells[i + 1], 0) / kStateDirs] = 1;
        }
    }
};

// Path cells to corner points: keep the two ends and every bend.
void compress(const std::vector<int>& cells, std::vector<int>& pts) {
    const std::size_t n = cells.size() / 2;
    if (n < 2) return;
    pts.push_back(cells[0]);
    pts.push_back(cells[1]);
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const int ax = cells[2 * i - 2], ay = cells[2 * i - 1];
        const int bx = cells[2 * i], by = cells[2 * i + 1];
        const int cx = cells[2 * i + 2], cy = cells[2 * i + 3];
        const bool straight = (ax == bx && bx == cx) || (ay == by && by == cy);
        if (!straight) {
            pts.push_back(bx);
            pts.push_back(by);
        }
    }
    pts.push_back(cells[2 * n - 2]);
    pts.push_back(cells[2 * n - 1]);
}

}  // namespace

bool routeNet(RoomBuf& buf, const RouteRequest& req, std::int32_t netForWires) {
    const std::size_t nPins = req.pins.size();
    if (nPins < 2) return false;  // nothing to join; the stub fallback stands

    // The grid is frozen at entry: reservations made on success may grow the
    // buffer afterwards, never the clip mid-search.
    const int gx1 = buf.maxX + 2 * P;
    const int gy1 = buf.maxY + 2 * P;
    Router r{buf, gx1, gy1, gx1 / P + 1, gy1 / P + 1, {}, {}, {}, 0};

    // The caller promises stub-end points on the P grid inside the clip;
    // anything else is answered with the safe refusal.
    for (const auto& [px, py] : req.pins) {
        if (px < 0 || py < 0 || px > gx1 || py > gy1) return false;
        if (px % P != 0 || py % P != 0) return false;
    }

    for (const WireItem& w : buf.wires) {
        if (w.net == netForWires) continue;  // same-net overlap merges: legal
        for (std::size_t i = 0; i + 3 < w.pts.size(); i += 2) {
            const int ax = w.pts[i], ay = w.pts[i + 1];
            const int bx = w.pts[i + 2], by = w.pts[i + 3];
            if (ay == by && ax != bx) {
                r.hSpans.push_back(HSpan{ay, std::min(ax, bx), std::max(ax, bx)});
            } else if (ax == bx && ay != by) {
                r.vSpans.push_back(VSpan{ax, std::min(ay, by), std::max(ay, by)});
            }
        }
    }

    r.tree.assign(static_cast<std::size_t>(r.nx) * static_cast<std::size_t>(r.ny), 0);

    // The seed pair is pins[0] and its nearest sibling by Manhattan distance,
    // ties to the earlier request index.
    std::size_t seed = 1;
    {
        int best = -1;
        for (std::size_t i = 1; i < nPins; ++i) {
            const int dx = req.pins[i].first - req.pins[0].first;
            const int dy = req.pins[i].second - req.pins[0].second;
            const int m = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (best < 0 || m < best) {
                best = m;
                seed = i;
            }
        }
    }

    std::vector<std::vector<int>> paths;  // corner polylines, committed at the end
    {
        std::vector<int> cells;
        if (!r.search(req.pins[0].first, req.pins[0].second, false,
                      req.pins[seed].first, req.pins[seed].second, cells)) {
            return false;
        }
        r.markTree(cells);
        std::vector<int> pts;
        compress(cells, pts);
        if (pts.size() >= 4) paths.push_back(std::move(pts));
    }
    // Every remaining pin, in request order, runs to the nearest point of the
    // tree so far: the search seeds at the pin and the first tree cell popped
    // is, by construction, the cheapest attachment.
    for (std::size_t i = 1; i < nPins; ++i) {
        if (i == seed) continue;
        std::vector<int> cells;
        if (!r.search(req.pins[i].first, req.pins[i].second, true, 0, 0, cells)) {
            return false;
        }
        r.markTree(cells);
        std::vector<int> pts;
        compress(cells, pts);
        if (pts.size() >= 4) paths.push_back(std::move(pts));
    }

    if (paths.empty()) return true;  // coincident pins: joined, nothing to draw

    // Commit. Reservations first, then the wires, then the dot count over
    // every segment this net now owns -- pre-existing stubs included, so a
    // route tapping a stub mid-flight dots correctly.
    for (const std::vector<int>& pts : paths) {
        for (std::size_t i = 0; i + 3 < pts.size(); i += 2) {
            const int ax = pts[i], ay = pts[i + 1], bx = pts[i + 2], by = pts[i + 3];
            const Rect rect =
                ay == by ? Rect{std::min(ax, bx), ay - 2, std::max(ax, bx), ay + 2}
                         : Rect{ax - 2, std::min(ay, by), ax + 2, std::max(ay, by)};
            buf.reserveWire(rect);
        }
    }
    for (std::vector<int>& pts : paths) {
        WireItem w;
        w.pts = std::move(pts);
        w.net = netForWires;
        buf.wires.push_back(std::move(w));
    }

    std::vector<Seg> segs;
    for (const WireItem& w : buf.wires) {
        if (w.net != netForWires) continue;
        for (std::size_t i = 0; i + 3 < w.pts.size(); i += 2) {
            segs.push_back(Seg{w.pts[i], w.pts[i + 1], w.pts[i + 2], w.pts[i + 3]});
        }
    }
    std::vector<DotItem> fresh;
    addJunctionDots(fresh, segs, netForWires);
    // addJunctionDots dedups within its own run but not against dots already
    // drawn, so dedup by exact coordinate: a re-derived stub junction must
    // not double its dot.
    for (const DotItem& nd : fresh) {
        bool present = false;
        for (const DotItem& e : buf.dots) {
            if (e.x == nd.x && e.y == nd.y) present = true;
        }
        if (!present) buf.dots.push_back(nd);
    }
    return true;
}

}  // namespace manta::render
