// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The room-local router of the Flow pipeline.
//
// Routing is strictly an upgrade over the mark fallback: a net the router
// declines -- or fails on -- keeps its stubs and labels, which connect by
// name and are always electrically correct. Correctness therefore never
// depends on this file. The real implementation arrives in a later work
// package; the stub below is the contract.
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

// Real implementation (later WP): join the pins with Manhattan wires through
// the RoomBuf's occupancy -- wires may cross wires, never a solid -- adding
// junction dots by the counting rule, and reserving what it draws. Returns
// false without touching the buffer when no route fits.
// Stub guarantee: returns false always; the caller falls back to stub+label.
[[nodiscard]] bool routeNet(RoomBuf&, const RouteRequest&, std::int32_t netForWires);

}  // namespace manta::render
