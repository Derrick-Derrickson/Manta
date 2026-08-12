// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The Flow placement engine's entry point.
//
// Per room: rank columns from the RoomFlow (vertices left to right by rank,
// top to bottom by barycentre order), rail bars with ladders and taps across
// the top, room-local label nets routed by routeNet with stub+label as the
// fallback, then the rooms tiled by tileRooms so their placed rectangles
// partition the bounding box. The pipeline plans pin sides from the flow
// graph before geometry is measured, so it builds its own per-page
// SymbolCache rather than taking the orchestrator's. Same SheetLayout
// contract as the classic engine, same debug invariants, plus the room
// partition invariant layout.cpp proves on multi-room pages.
#pragma once

#include "render/layout.h"

namespace manta::render {

[[nodiscard]] SheetLayout layoutPageFlow(const RenderModel& model, const RenderPage& page);

}  // namespace manta::render
