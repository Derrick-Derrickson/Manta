// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The room-local router: grid search, obstacle legality, the per-net junction
// count, and the untouched-buffer guarantee on every failure path.
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "harness.h"
#include "render/geometry.h"
#include "render/route.h"

using namespace manta;
using namespace manta::render;

namespace {

RoomBuf room(int w, int h) {
    RoomBuf buf;
    buf.grow(Rect{0, 0, w, h});
    return buf;
}

RouteRequest request(std::int32_t net, std::vector<std::pair<int, int>> pins) {
    RouteRequest rq;
    rq.net = net;
    rq.pins = std::move(pins);
    return rq;
}

// Every maximal segment of every wire of `net`, for geometric assertions.
std::vector<Seg> segmentsOf(const RoomBuf& buf, std::int32_t net) {
    std::vector<Seg> segs;
    for (const WireItem& w : buf.wires) {
        if (w.net != net) continue;
        for (std::size_t i = 0; i + 3 < w.pts.size(); i += 2) {
            segs.push_back(Seg{w.pts[i], w.pts[i + 1], w.pts[i + 2], w.pts[i + 3]});
        }
    }
    return segs;
}

bool sameWires(const std::vector<WireItem>& a, const std::vector<WireItem>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].net != b[i].net || a[i].pts != b[i].pts) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The plain cases.
// ---------------------------------------------------------------------------

TEST_CASE("a straight pair is one horizontal segment, reserved, dotless") {
    RoomBuf buf = room(100, 100);
    CHECK_FALSE(buf.wireRects.hits(Rect{40, 49, 60, 51}));
    CHECK(routeNet(buf, request(3, {{10, 50}, {90, 50}}), 3));
    CHECK_EQ(buf.wires.size(), std::size_t{1});
    const std::vector<int> want{10, 50, 90, 50};
    CHECK(buf.wires[0].pts == want);
    CHECK_EQ(buf.wires[0].net, 3);
    CHECK(buf.dots.empty());
    CHECK(buf.wireRects.hits(Rect{40, 49, 60, 51}));  // the reservation landed
    CHECK_FALSE(buf.collides(Rect{40, 49, 60, 51}));  // as a wire, not a solid
}

TEST_CASE("a route jogs around a solid and never sweeps its interior") {
    RoomBuf buf = room(100, 100);
    const Rect block{40, 30, 60, 70};
    buf.reserve(block);
    CHECK(routeNet(buf, request(3, {{10, 50}, {90, 50}}), 3));
    CHECK_EQ(buf.wires.size(), std::size_t{1});
    const std::vector<int>& p = buf.wires[0].pts;
    CHECK(p.size() >= 8);   // it had to leave the row: at least two bends
    CHECK(p.size() <= 12);  // and one detour suffices: at most four
    CHECK_EQ(p[0], 10);
    CHECK_EQ(p[1], 50);
    CHECK_EQ(p[p.size() - 2], 90);
    CHECK_EQ(p[p.size() - 1], 50);
    for (const Seg& s : segmentsOf(buf, 3)) {
        // The swept centre line of every segment stays clear of the block.
        const Rect sweep = s.y1 == s.y2
                               ? Rect{std::min(s.x1, s.x2), s.y1, std::max(s.x1, s.x2), s.y1}
                               : Rect{s.x1, std::min(s.y1, s.y2), s.x1, std::max(s.y1, s.y2)};
        CHECK_FALSE(overlaps(sweep, block));
    }
    CHECK(buf.dots.empty());
}

// ---------------------------------------------------------------------------
// Other wires: crossing is drawing, collinear overlap is forgery.
// ---------------------------------------------------------------------------

TEST_CASE("a foreign wire is crossed square-on and takes no dot") {
    RoomBuf buf = room(100, 100);
    buf.wire({50, 0, 50, 100}, 7);
    CHECK(routeNet(buf, request(3, {{10, 50}, {90, 50}}), 3));
    CHECK_EQ(buf.wires.size(), std::size_t{2});
    const std::vector<int> want{10, 50, 90, 50};  // straight through the crossing
    CHECK(buf.wires[1].pts == want);
    CHECK(buf.dots.empty());
    // Why no dot: the junction count only ever sees ONE net's segments. Mixed
    // together, two lines passing through the crossing would count 2+2 = 4
    // conductor ends, and 4 >= 3 would wrongly dot an unconnected crossing.
    const std::vector<Seg> mixed{Seg{50, 0, 50, 100}, Seg{10, 50, 90, 50}};
    CHECK_EQ(conductorsAt(mixed, 50, 50), 4);
    CHECK_EQ(conductorsAt(segmentsOf(buf, 3), 50, 50), 2);
}

TEST_CASE("collinear overlap with a foreign net is refused, not drawn over") {
    RoomBuf buf = room(100, 100);
    buf.wire({30, 50, 70, 50}, 7);  // squarely on the direct row
    CHECK(routeNet(buf, request(3, {{10, 50}, {90, 50}}), 3));
    bool leftTheRow = false;
    for (const Seg& s : segmentsOf(buf, 3)) {
        if (s.y1 != s.y2) {
            leftTheRow = true;
            continue;
        }
        if (s.y1 != 50) continue;  // only row-50 horizontals could overlap
        const int xa = std::min(s.x1, s.x2), xb = std::max(s.x1, s.x2);
        CHECK(std::max(xa, 30) >= std::min(xb, 70));  // touch at most, never run along
    }
    CHECK(leftTheRow);  // it went around
    CHECK(buf.dots.empty());
}

// ---------------------------------------------------------------------------
// Junction dots.
// ---------------------------------------------------------------------------

TEST_CASE("a third pin tapping mid-segment takes exactly one dot") {
    RoomBuf buf = room(100, 100);
    CHECK(routeNet(buf, request(3, {{10, 50}, {90, 50}, {50, 90}}), 3));
    CHECK_EQ(buf.wires.size(), std::size_t{2});
    CHECK_EQ(buf.dots.size(), std::size_t{1});
    CHECK_EQ(buf.dots[0].x, 50);
    CHECK_EQ(buf.dots[0].y, 50);
    CHECK_EQ(buf.dots[0].net, 3);
}

TEST_CASE("a pre-existing stub of the net joins the dot count") {
    RoomBuf buf = room(100, 100);
    buf.wire({50, 40, 50, 50}, 3);  // this net's own stub, drawn by the placer
    CHECK(routeNet(buf, request(3, {{10, 50}, {90, 50}}), 3));
    CHECK_EQ(buf.dots.size(), std::size_t{1});
    CHECK_EQ(buf.dots[0].x, 50);
    CHECK_EQ(buf.dots[0].y, 50);
}

TEST_CASE("an existing dot at the junction is not doubled") {
    RoomBuf buf = room(100, 100);
    buf.dots.push_back(DotItem{50, 50, 3});
    CHECK(routeNet(buf, request(3, {{10, 50}, {90, 50}, {50, 90}}), 3));
    CHECK_EQ(buf.dots.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Failure leaves no trace.
// ---------------------------------------------------------------------------

TEST_CASE("total blockage returns false and leaves the buffer bit-identical") {
    RoomBuf buf = room(100, 100);
    buf.wire({10, 50, 20, 50}, 3);  // pre-existing state to preserve
    buf.dots.push_back(DotItem{20, 50, 3});
    buf.reserve(Rect{40, 40, 80, 80});  // seals the pin at (60,60) inside
    const std::vector<WireItem> wires = buf.wires;
    const std::size_t nDots = buf.dots.size();
    const int mx = buf.maxX, my = buf.maxY;
    CHECK_FALSE(routeNet(buf, request(3, {{10, 50}, {60, 60}}), 3));
    CHECK(sameWires(buf.wires, wires));
    CHECK_EQ(buf.dots.size(), nDots);
    CHECK_EQ(buf.dots[0].x, 20);
    CHECK_EQ(buf.maxX, mx);
    CHECK_EQ(buf.maxY, my);
    CHECK_FALSE(buf.wireRects.hits(Rect{-100, -100, 300, 300}));  // nothing reserved
}

TEST_CASE("an unreachable goal floods, is refused, and leaves the buffer untouched") {
    // The goal pin is sealed inside a solid, so the search exhausts every
    // reachable state and gives up cleanly. The pop cap is proportional to
    // the grid (kPopFactor per state), so a full flood of a room this size
    // finishes well inside it -- the refusal is the honest "no path", not a
    // budget accident.
    RoomBuf buf = room(1200, 1200);
    buf.reserve(Rect{550, 550, 650, 650});
    CHECK_FALSE(routeNet(buf, request(3, {{10, 10}, {600, 600}}), 3));
    CHECK(buf.wires.empty());
    CHECK(buf.dots.empty());
    CHECK_FALSE(buf.wireRects.hits(Rect{-100, -100, 1500, 1500}));
}

TEST_CASE("a room-length route succeeds: the cap scales with the grid") {
    // The regression that surfaced as spurious XTAL labels on a real design:
    // pins a room-length apart need a cost diamond of tens of thousands of
    // states, which a fixed 20000-pop cap cut short. The cap now scales with
    // the grid, so the longest in-room route is always affordable.
    RoomBuf buf = room(2000, 1600);
    CHECK(routeNet(buf, request(3, {{10, 10}, {1990, 1590}}), 3));
    CHECK_EQ(buf.wires.size(), std::size_t{1});
    const std::vector<int>& p = buf.wires[0].pts;
    CHECK_EQ(p[0], 10);
    CHECK_EQ(p[1], 10);
    CHECK_EQ(p[p.size() - 2], 1990);
    CHECK_EQ(p[p.size() - 1], 1590);
}

TEST_CASE("degenerate requests are declined untouched") {
    RoomBuf buf = room(100, 100);
    CHECK_FALSE(routeNet(buf, request(3, {{10, 50}}), 3));             // one pin
    CHECK_FALSE(routeNet(buf, request(3, {{15, 50}, {90, 50}}), 3));   // off grid
    CHECK_FALSE(routeNet(buf, request(3, {{-10, 50}, {90, 50}}), 3));  // off clip
    CHECK(buf.wires.empty());
    CHECK(buf.dots.empty());
}

// ---------------------------------------------------------------------------
// Rail taps.
// ---------------------------------------------------------------------------

TEST_CASE("a clear tap routes straight up and dots the bar") {
    RoomBuf buf = room(200, 200);
    const RailBarItem bar{0, 200, 12, 5};
    CHECK(routeRailTap(buf, 100, 100, bar));
    CHECK_EQ(buf.wires.size(), std::size_t{1});
    const std::vector<int>& p = buf.wires[0].pts;
    // One vertical run whose final joint lands on the off-grid bar line.
    CHECK_EQ(p[0], 100);
    CHECK_EQ(p[1], 100);
    CHECK_EQ(p[p.size() - 2], 100);
    CHECK_EQ(p[p.size() - 1], 12);
    CHECK_EQ(buf.dots.size(), std::size_t{1});
    CHECK_EQ(buf.dots[0].x, 100);
    CHECK_EQ(buf.dots[0].y, 12);
    CHECK_EQ(buf.dots[0].net, 5);
}

TEST_CASE("a blocked tap jogs around the solid and still reaches the bar") {
    RoomBuf buf = room(200, 200);
    buf.reserve(Rect{60, 40, 140, 80});  // squarely astride the direct drop
    const RailBarItem bar{0, 200, 12, 5};
    CHECK(routeRailTap(buf, 100, 100, bar));
    CHECK_EQ(buf.wires.size(), std::size_t{1});
    const std::vector<int>& p = buf.wires[0].pts;
    CHECK(p.size() >= 6);  // it had to leave the direct column: at least one bend
    CHECK_EQ(p[0], 100);
    CHECK_EQ(p[1], 100);
    CHECK_EQ(p[p.size() - 1], 12);  // and it ends on the bar line
    bool leftTheColumn = false;
    for (std::size_t i = 0; i < p.size(); i += 2) {
        if (p[i] != 100) leftTheColumn = true;
    }
    CHECK(leftTheColumn);
    CHECK_EQ(buf.dots.size(), std::size_t{1});
    CHECK_EQ(buf.dots[0].y, 12);
}

TEST_CASE("an off-grid or sealed tap is refused with the buffer untouched") {
    RoomBuf buf = room(200, 200);
    const RailBarItem bar{0, 200, 12, 5};
    CHECK_FALSE(routeRailTap(buf, 105, 100, bar));  // off the P grid
    buf.reserve(Rect{60, 60, 140, 140});
    CHECK_FALSE(routeRailTap(buf, 100, 100, bar));  // sealed inside a solid
    CHECK(buf.wires.empty());
    CHECK(buf.dots.empty());
}

// ---------------------------------------------------------------------------
// Determinism (spec 15.8).
// ---------------------------------------------------------------------------

TEST_CASE("the same request on equal buffers routes identically") {
    auto build = [] {
        RoomBuf buf = room(100, 100);
        buf.reserve(Rect{40, 30, 60, 70});
        buf.wire({30, 20, 70, 20}, 7);
        return buf;
    };
    RoomBuf a = build();
    RoomBuf b = build();
    const RouteRequest rq = request(3, {{10, 50}, {90, 50}, {50, 90}});
    CHECK(routeNet(a, rq, 3));
    CHECK(routeNet(b, rq, 3));
    CHECK(sameWires(a.wires, b.wires));
    CHECK_EQ(a.dots.size(), b.dots.size());
}

TEST_CASE("a permutation that keeps the seed pair keeps the tree") {
    // The seed partner is the pin nearest pins[0] -- B in both orders -- and
    // only C remains, so both orders must build the identical tree. (A
    // permutation moving pins[0] itself may legitimately choose another seed
    // pair; that is the documented ordering rule, not nondeterminism.)
    RoomBuf a = room(100, 100);
    RoomBuf b = room(100, 100);
    CHECK(routeNet(a, request(3, {{10, 10}, {50, 10}, {90, 10}}), 3));
    CHECK(routeNet(b, request(3, {{10, 10}, {90, 10}, {50, 10}}), 3));
    CHECK(sameWires(a.wires, b.wires));
    // C joins the tree end-to-end at B: two conductor ends, no dot either way.
    CHECK(a.dots.empty());
    CHECK(b.dots.empty());
}

TEST_MAIN()
