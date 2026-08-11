// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The Flow placement engine's entry point.
//
// Real implementation (later WPs): per room, place vertices column by column
// from the RoomFlow ranks -- rails above, grounds below -- plan pin sides
// with planSides, route room-local nets with routeNet (falling back to
// stub+label), then tile the rooms with tileRooms. Same SheetLayout contract
// as the classic engine, same debug invariants.
// Stub guarantee: delegates to layoutPageClassic, so '--layout=flow' is
// selectable today and byte-identical to the default.
#pragma once

#include "render/layout.h"

namespace manta::render {

[[nodiscard]] SheetLayout layoutPageFlow(const RenderModel& model, const RenderPage& page,
                                         const SymbolCache& cache);

}  // namespace manta::render
