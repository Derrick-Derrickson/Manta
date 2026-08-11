// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Sheet tiling: where each laid-out room lands on the page.
//
// Real implementation (later WP): choose room positions so pairs that share
// signal nets (InterRoomFlow) sit adjacent, still filling toward the classic
// target aspect. Integer geometry, insertion-order scanning, no floats.
// Stub guarantee: returns an empty vector, meaning "use the classic shelf
// packing"; a caller must treat empty as that fallback.
#pragma once

#include <vector>

#include "render/flow.h"
#include "render/geometry.h"

namespace manta::render {

struct RoomExtent {
    int w = 0, h = 0;  // the room's outer rectangle, pad and title strip in
};

struct RoomPlace {
    int x = 0, y = 0;  // top-left of the room's outer rectangle, sheet coords
};

// Parallel to `rooms` (page order) when non-empty. `targetW` is the same
// clamped ceil(sqrt(1.45 * area)) budget the classic shelf packing uses.
[[nodiscard]] std::vector<RoomPlace> tileRooms(const std::vector<RoomExtent>& rooms,
                                               const InterRoomFlow& flow, int targetW);

}  // namespace manta::render
