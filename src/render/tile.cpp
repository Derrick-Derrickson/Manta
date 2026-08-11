// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/tile.h"

namespace manta::render {

// Stub (WP6 replaces it): no plan, so the caller keeps the classic shelf
// packing.
std::vector<RoomPlace> tileRooms(const std::vector<RoomExtent>& rooms, const InterRoomFlow& flow,
                                 int targetW) {
    (void)rooms;
    (void)flow;
    (void)targetW;
    return {};
}

}  // namespace manta::render
