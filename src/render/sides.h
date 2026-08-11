// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Flow-aware pin side assignment for box symbols.
//
// The builtin heuristic in symbols.cpp sorts pins onto sides by type and
// direction alone. The Flow pipeline plans sides from the room's flow graph
// instead -- a pin faces the vertex its net flows to -- and hands the plan to
// the geometry builder. The real implementation arrives in a later work
// package; the stub below is the contract.
#pragma once

#include <cstdint>
#include <vector>

#include "render/flow.h"
#include "render/symbols.h"

namespace manta::render {

struct PinPlan {
    Side side;
    int slot;      // row/column index along the side, 0 at the top or left
    bool showNet;  // false: the wire is routed, so the stub carries no name
};

// Parallel to Component::pins. Empty byPin means "use the builtin heuristic",
// which is also what buildSymbol does when handed no plan at all.
struct SidePlan {
    std::vector<PinPlan> byPin;
};

// Real implementation (later WP): place each pin on the side facing the
// ranked position of its net's far end, slots ordered to minimise crossings.
// Stub guarantee: an empty SidePlan, meaning "use the builtin heuristic".
[[nodiscard]] SidePlan planSides(std::uint32_t comp, const RoomFlow&, const RenderPage&,
                                 const RenderModel&);

}  // namespace manta::render
