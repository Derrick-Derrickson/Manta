// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/sides.h"

namespace manta::render {

// Stub (WP2 replaces it): empty plan, so every symbol keeps the builtin
// side heuristic and geometry is byte-identical to today's.
SidePlan planSides(std::uint32_t comp, const RoomFlow& flow, const RenderPage& page,
                   const RenderModel& m) {
    (void)comp;
    (void)flow;
    (void)page;
    (void)m;
    return {};
}

}  // namespace manta::render
