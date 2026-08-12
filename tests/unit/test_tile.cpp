// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Sheet tiling: the flow-column tiler is a pure function, so every property
// is checked directly -- the partition invariant, reading order under pull,
// height balancing, the remainder rules, the degenerate inputs, determinism,
// and the bounded adjacency-swap pass.
#include <cstdint>
#include <vector>

#include "harness.h"
#include "render/tile.h"

using namespace manta::render;

namespace {

InterRoomFlow noFlow() { return {}; }

InterRoomFlow pullFlow(std::vector<int> pull) {
    InterRoomFlow f;
    f.pull = std::move(pull);
    return f;
}

InterRoomFlow countsFlow(std::size_t n, std::size_t a, std::size_t b, std::uint32_t c) {
    InterRoomFlow f;
    f.counts.assign(n, std::vector<std::uint32_t>(n, 0));
    f.counts[a][b] = c;
    f.counts[b][a] = c;
    return f;
}

// The core invariant: placements parallel to input, each at least its
// extent, pairwise disjoint, and together an exact partition of their
// bounding box (disjoint + areas summing to the bbox area implies the
// union is the bbox).
void checkPartition(const std::vector<RoomExtent>& rooms, const std::vector<RoomPlace>& places) {
    CHECK_EQ(places.size(), rooms.size());
    if (places.size() != rooms.size() || places.empty()) return;

    std::int64_t bboxW = 0, bboxH = 0, sum = 0;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const RoomPlace& p = places[i];
        CHECK(p.x >= 0);
        CHECK(p.y >= 0);
        CHECK(p.w >= rooms[i].w);
        CHECK(p.h >= rooms[i].h);
        bboxW = std::max(bboxW, static_cast<std::int64_t>(p.x) + p.w);
        bboxH = std::max(bboxH, static_cast<std::int64_t>(p.y) + p.h);
        sum += static_cast<std::int64_t>(p.w) * p.h;
    }
    for (std::size_t i = 0; i < places.size(); ++i) {
        for (std::size_t j = i + 1; j < places.size(); ++j) {
            const RoomPlace& a = places[i];
            const RoomPlace& b = places[j];
            if (a.w == 0 || a.h == 0 || b.w == 0 || b.h == 0) continue;
            const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w &&  //
                                 a.y < b.y + b.h && b.y < a.y + a.h;
            CHECK_FALSE(overlap);
        }
    }
    CHECK_EQ(sum, bboxW * bboxH);
}

void checkSame(const std::vector<RoomPlace>& a, const std::vector<RoomPlace>& b) {
    CHECK_EQ(a.size(), b.size());
    if (a.size() != b.size()) return;
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK_EQ(a[i].x, b[i].x);
        CHECK_EQ(a[i].y, b[i].y);
        CHECK_EQ(a[i].w, b[i].w);
        CHECK_EQ(a[i].h, b[i].h);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The partition property
// ---------------------------------------------------------------------------

TEST_CASE("a mixed set of rooms partitions its bounding box") {
    std::vector<RoomExtent> rooms = {{120, 80}, {100, 200}, {90, 90}, {110, 50}, {70, 130}};
    checkPartition(rooms, tileRooms(rooms, noFlow(), 300));
}

// ---------------------------------------------------------------------------
// Reading order under pull
// ---------------------------------------------------------------------------

TEST_CASE("pull sorts rooms into reading order") {
    // Page order carries pulls +1, -1, 0. The reading order is pull
    // ascending -- room 1, room 2, room 0 -- walked left to right across
    // columns and top to bottom within one. Three equal squares land as two
    // columns (200x200 sits nearer the landscape reference than 300x100),
    // so the -1 room leads the first column, the indifferent one stacks
    // under it, and the +1 room takes the last column.
    std::vector<RoomExtent> rooms = {{100, 100}, {100, 100}, {100, 100}};
    auto places = tileRooms(rooms, pullFlow({+1, -1, 0}), 300);
    checkPartition(rooms, places);
    CHECK_EQ(places[1].x, 0);
    CHECK_EQ(places[1].y, 0);
    CHECK_EQ(places[2].x, 0);
    CHECK_EQ(places[2].y, 100);
    CHECK_EQ(places[0].x, 100);
    CHECK_EQ(places[0].y, 0);
}

// ---------------------------------------------------------------------------
// Height balancing
// ---------------------------------------------------------------------------

TEST_CASE("four equal rooms at two-column budget tile as a balanced 2x2") {
    std::vector<RoomExtent> rooms = {{100, 100}, {100, 100}, {100, 100}, {100, 100}};
    auto places = tileRooms(rooms, noFlow(), 200);
    checkPartition(rooms, places);
    CHECK_EQ(places[0].x, 0);
    CHECK_EQ(places[0].y, 0);
    CHECK_EQ(places[1].x, 0);
    CHECK_EQ(places[1].y, 100);
    CHECK_EQ(places[2].x, 100);
    CHECK_EQ(places[2].y, 0);
    CHECK_EQ(places[3].x, 100);
    CHECK_EQ(places[3].y, 100);
    for (const RoomPlace& p : places) {
        CHECK_EQ(p.w, 100);
        CHECK_EQ(p.h, 100);
    }
}

// ---------------------------------------------------------------------------
// Aspect-driven column count
// ---------------------------------------------------------------------------

TEST_CASE("a generous width budget still lands near the landscape aspect") {
    // Eight equal squares under a budget that would allow a 800x100 strip:
    // the aspects on offer are 100x800, 200x400, 300x300, 400x200, ... and
    // 300x300 sits nearest the sqrt(2):1 reference, so three columns win.
    // The budget is a ceiling, never a demand.
    std::vector<RoomExtent> rooms(8, RoomExtent{100, 100});
    auto places = tileRooms(rooms, noFlow(), 1000);
    checkPartition(rooms, places);
    std::int64_t w = 0, h = 0;
    for (const RoomPlace& p : places) {
        w = std::max<std::int64_t>(w, p.x + p.w);
        h = std::max<std::int64_t>(h, p.y + p.h);
    }
    CHECK_EQ(w, 300);
    CHECK_EQ(h, 300);
}

TEST_CASE("the width budget binds the aspect choice") {
    // The same eight squares under a 200 budget cannot take their preferred
    // three columns: two is the widest fill that fits.
    std::vector<RoomExtent> rooms(8, RoomExtent{100, 100});
    auto places = tileRooms(rooms, noFlow(), 200);
    checkPartition(rooms, places);
    std::int64_t w = 0;
    for (const RoomPlace& p : places) w = std::max<std::int64_t>(w, p.x + p.w);
    CHECK_EQ(w, 200);
}

// ---------------------------------------------------------------------------
// Remainder rules
// ---------------------------------------------------------------------------

TEST_CASE("uneven height deficit goes proportionally, remainder to the last member") {
    // Two columns: [A h100, B h50] beside [C h157]. Column one is 7 short of
    // 157; A gets 7*100/150 = 4, B (last) absorbs the remaining 3. B is also
    // narrower than its column and stretches to the column width.
    std::vector<RoomExtent> rooms = {{100, 100}, {90, 50}, {100, 157}};
    auto places = tileRooms(rooms, noFlow(), 200);
    checkPartition(rooms, places);
    CHECK_EQ(places[0].x, 0);
    CHECK_EQ(places[0].y, 0);
    CHECK_EQ(places[0].w, 100);
    CHECK_EQ(places[0].h, 104);
    CHECK_EQ(places[1].x, 0);
    CHECK_EQ(places[1].y, 104);
    CHECK_EQ(places[1].w, 100);  // stretched to the column width
    CHECK_EQ(places[1].h, 53);   // 50 + (7 - 4): the last member absorbs
    CHECK_EQ(places[2].x, 100);
    CHECK_EQ(places[2].y, 0);
    CHECK_EQ(places[2].w, 100);
    CHECK_EQ(places[2].h, 157);
    // Width has no remainder by construction: the bbox width IS the sum of
    // the column widths, and every member takes its column's width exactly.
    CHECK_EQ(places[0].w + places[2].w, 200);
}

// ---------------------------------------------------------------------------
// Degenerates
// ---------------------------------------------------------------------------

TEST_CASE("a single room fills its own bounding box unchanged") {
    std::vector<RoomExtent> rooms = {{123, 77}};
    auto places = tileRooms(rooms, noFlow(), 500);
    checkPartition(rooms, places);
    CHECK_EQ(places[0].x, 0);
    CHECK_EQ(places[0].y, 0);
    CHECK_EQ(places[0].w, 123);
    CHECK_EQ(places[0].h, 77);
}

TEST_CASE("a room wider than targetW still tiles") {
    std::vector<RoomExtent> rooms = {{500, 100}, {100, 100}};
    auto places = tileRooms(rooms, noFlow(), 200);
    checkPartition(rooms, places);
    // The width budget is the widest room; here that forces one column.
    CHECK_EQ(places[0].w, 500);
    CHECK_EQ(places[1].w, 500);
    CHECK_EQ(places[0].x, places[1].x);
}

TEST_CASE("a narrow targetW yields a single stacked column") {
    std::vector<RoomExtent> rooms = {{100, 50}, {100, 50}, {100, 50}};
    auto places = tileRooms(rooms, noFlow(), 100);
    checkPartition(rooms, places);
    for (const RoomPlace& p : places) {
        CHECK_EQ(p.x, 0);
        CHECK_EQ(p.w, 100);
        CHECK_EQ(p.h, 50);
    }
    CHECK_EQ(places[0].y, 0);
    CHECK_EQ(places[1].y, 50);
    CHECK_EQ(places[2].y, 100);
}

TEST_CASE("zero rooms tile to nothing") {
    std::vector<RoomExtent> rooms;
    CHECK(tileRooms(rooms, noFlow(), 300).empty());
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST_CASE("the same input tiles identically twice") {
    std::vector<RoomExtent> rooms = {{120, 80}, {100, 200}, {90, 90}, {110, 50}, {70, 130}};
    checkSame(tileRooms(rooms, noFlow(), 300), tileRooms(rooms, noFlow(), 300));
}

TEST_CASE("distinct pulls make the tiling independent of page order") {
    // Reading order is (pull, page order); with all pulls distinct the page
    // order never breaks a tie, so permuting the input must give every
    // physical room the same rectangle.
    std::vector<RoomExtent> rooms1 = {{100, 80}, {90, 120}, {110, 60}, {80, 100}};
    std::vector<int> pull1 = {-2, -1, +1, +2};
    // Permutation: page order C, A, D, B of the same physical rooms.
    std::vector<RoomExtent> rooms2 = {{110, 60}, {100, 80}, {80, 100}, {90, 120}};
    std::vector<int> pull2 = {+1, -2, +2, -1};

    auto p1 = tileRooms(rooms1, pullFlow(pull1), 400);
    auto p2 = tileRooms(rooms2, pullFlow(pull2), 400);
    checkPartition(rooms1, p1);
    checkPartition(rooms2, p2);
    const std::size_t map[4] = {2, 0, 3, 1};  // rooms2[i] is rooms1[map[i]]
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK_EQ(p2[i].x, p1[map[i]].x);
        CHECK_EQ(p2[i].y, p1[map[i]].y);
        CHECK_EQ(p2[i].w, p1[map[i]].w);
        CHECK_EQ(p2[i].h, p1[map[i]].h);
    }
}

// ---------------------------------------------------------------------------
// The adjacency-swap pass
// ---------------------------------------------------------------------------

TEST_CASE("a beneficial swap moves connected rooms into adjacent columns") {
    // 2x2 tiling puts rooms 0 and 1 in the same column, where their shared
    // nets earn nothing. Swapping 0 with 2 makes 1 and 0 horizontal
    // neighbours; the pass must take it.
    std::vector<RoomExtent> rooms = {{100, 100}, {100, 100}, {100, 100}, {100, 100}};
    auto places = tileRooms(rooms, countsFlow(4, 0, 1, 5), 200);
    checkPartition(rooms, places);
    CHECK_EQ(places[2].x, 0);  // room 2 took room 0's slot
    CHECK_EQ(places[2].y, 0);
    CHECK_EQ(places[1].x, 0);
    CHECK_EQ(places[0].x, 100);  // room 0 moved beside room 1
    CHECK_EQ(places[0].y, 0);
    CHECK_EQ(places[3].x, 100);
}

TEST_CASE("a swap the width budget forbids is not taken") {
    // Same appetite -- rooms 0 and 1 share nets -- but the second column is
    // narrow: any swap drags a 100-wide room into it and pushes the summed
    // column widths past the budget of 160. The layout must stand.
    std::vector<RoomExtent> rooms = {{100, 100}, {100, 100}, {60, 100}, {60, 100}};
    auto places = tileRooms(rooms, countsFlow(4, 0, 1, 5), 160);
    checkPartition(rooms, places);
    CHECK_EQ(places[0].x, 0);
    CHECK_EQ(places[1].x, 0);  // still stacked with room 0
    CHECK_EQ(places[2].x, 100);
    CHECK_EQ(places[3].x, 100);
}

TEST_MAIN()
