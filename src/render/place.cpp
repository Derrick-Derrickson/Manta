// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/place.h"

#include "render/place_classic.h"

namespace manta::render {

// Stub (WP4 replaces it): the Flow pipeline is the classic one until the
// flow placer lands, so the flag can ship first and the diff later.
SheetLayout layoutPageFlow(const RenderModel& model, const RenderPage& page,
                           const SymbolCache& cache) {
    return layoutPageClassic(model, page, cache);
}

}  // namespace manta::render
