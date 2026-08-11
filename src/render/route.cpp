// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/route.h"

namespace manta::render {

// Stub (WP5 replaces it): decline every net, so every caller takes the
// stub+label fallback and the buffer is never touched.
bool routeNet(RoomBuf& buf, const RouteRequest& req, std::int32_t netForWires) {
    (void)buf;
    (void)req;
    (void)netForWires;
    return false;
}

}  // namespace manta::render
