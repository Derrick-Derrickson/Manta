// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Sheet tiling: where each laid-out room lands on the page.
//
// Flow columns: the reading order (pull, page order) split into contiguous
// height-balanced columns, the column count chosen by the aspect it
// produces -- nearest the landscape sqrt(2):1 reference within the width
// budget -- and every placement stretched so the rooms partition their
// bounding box exactly. Integer geometry, insertion-order scanning, no
// floats (spec 15.8).
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
    // The outer rectangle as placed, never smaller than the extent handed in.
    // A tiler stretches rooms -- content stays anchored top-left, the frame
    // grows -- so that the placed rectangles partition their bounding box
    // exactly: borders meet, and no paper between rooms is outside every room.
    int w = 0, h = 0;
};

// Parallel to `rooms` (page order) when non-empty. `targetW` is a hard width
// ceiling (never below the widest room); within it the tiler picks the
// column count whose bounding box lands nearest the landscape aspect.
[[nodiscard]] std::vector<RoomPlace> tileRooms(const std::vector<RoomExtent>& rooms,
                                               const InterRoomFlow& flow, int targetW);

}  // namespace manta::render
