// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The classic placement engine, moved whole from layout.cpp.
//
// Placement idioms instead of a router: bands per room (rail ladders, then
// anchors with their pulls and chains, then children, then the leftover free
// grid), designator-ordered anchors, and shelf-packed rooms on the sheet.
// Collision handling is one axis per element kind: chain and node strips move
// DOWN (y += P), pull-up/pull-down/ladder columns move RIGHT (x += P). A wire
// that still cannot be placed is not drawn at all -- the connection keeps its
// labels, which connect by name.
#pragma once

#include "render/layout.h"

namespace manta::render {

// Lays every room out, shelf-packs them, and returns the assembled sheet.
// `cache` is indexed by component index (buildSymbolCache).
[[nodiscard]] SheetLayout layoutPageClassic(const RenderModel& model, const RenderPage& page,
                                            const SymbolCache& cache);

}  // namespace manta::render
