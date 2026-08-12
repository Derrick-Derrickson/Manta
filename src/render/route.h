// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The room-local router of the Flow pipeline.
//
// Routing is strictly an upgrade over the mark fallback: a net the router
// declines -- or fails on -- keeps its stubs and labels, which connect by
// name and are always electrically correct. Correctness therefore never
// depends on this file.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "render/geometry.h"

namespace manta::render {

struct RouteRequest {
    std::int32_t net;
    std::vector<std::pair<int, int>> pins;  // stub-end points to join, room coords
};

// Join the pins with Manhattan wires through the RoomBuf's occupancy -- wires
// may cross wires, never a solid -- adding junction dots by the counting
// rule, and reserving what it draws. Returns false without touching the
// buffer when no route fits; the caller falls back to stub+label.
[[nodiscard]] bool routeNet(RoomBuf&, const RouteRequest&, std::int32_t netForWires);

// Join one rail consumer's tap point (a grid point, room coords) to its rail
// bar: a Manhattan path to the first grid row below the bar inside the bar's
// span, finished with the short joint onto the bar line and one junction dot
// there. Same commit-at-end contract as routeNet: false leaves the buffer
// untouched and the caller keeps the per-pin rail flag, which is always
// electrically correct.
[[nodiscard]] bool routeRailTap(RoomBuf&, int px, int py, const RailBarItem& bar);

}  // namespace manta::render
