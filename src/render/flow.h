// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Signal flow, extracted before anything is placed.
//
// The Flow pipeline reads a room as a directed graph -- sources on the left,
// loads on the right -- and ranks it before choosing coordinates, so the
// placer can put a divider between the things it divides instead of wherever
// the designator order happened to fall. The real implementation arrives in a
// later work package; the stubs below are contracts, not behaviour.
#pragma once

#include <cstdint>
#include <vector>

#include "render/model.h"

namespace manta::render {

// One node of a room's flow graph. A vertex is not always one component: a
// series string of two-terminal parts collapses into a single Series vertex,
// exactly as the classic chain walk treats it as one drawn path.
struct FlowVertex {
    enum class Kind : std::uint8_t { Anchor, Series, Vertical, Child, Loose };
    Kind kind;
    std::vector<std::uint32_t> comps;   // 1 for Anchor/Vertical/Loose; the whole string for Series
    int rank = 0;                       // longest-path from sources, left->right
    int order = 0;                      // barycentre position within the rank
};

// A room's ranked flow graph. Rails and grounds are not vertices -- they are
// the ambient top and bottom of every column -- so they are listed apart.
struct RoomFlow {
    std::vector<FlowVertex> verts;
    std::vector<std::vector<std::uint32_t>> ranks;  // rank -> vertex indices, ordered
    std::vector<std::int32_t> vertexOf;             // component index -> vertex, -1
    std::vector<std::int32_t> roomRails, roomGrounds;
};

// Net traffic between the rooms of one page, for the tiler: how strongly two
// rooms want to be adjacent. Kept minimal on purpose -- room-to-room counts
// of shared signal (label/port) nets; rails and grounds connect everywhere
// and say nothing about adjacency.
struct InterRoomFlow {
    // counts[a][b]: signal nets touching both room a and room b of the page,
    // square and symmetric, rooms in page order. Empty from the stub.
    std::vector<std::vector<std::uint32_t>> counts;
};

// Real implementation (later WP): collapse series strings, rank the vertex
// graph by longest path from its sources, order each rank by barycentre.
// Stub guarantee: every component of the room is its own Anchor vertex at
// rank 0, order = its position in the room's component list; rails/grounds
// empty. Deterministic, and total -- vertexOf covers every component.
[[nodiscard]] RoomFlow buildRoomFlow(const RenderPage&, const RenderRoom&, const RenderModel&);

// Real implementation (later WP): count shared signal nets per room pair.
// Stub guarantee: empty counts, which every consumer must read as "no
// adjacency preference".
[[nodiscard]] InterRoomFlow buildInterRoomFlow(const RenderPage&, const RenderModel&);

}  // namespace manta::render
