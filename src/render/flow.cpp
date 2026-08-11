// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/flow.h"

namespace manta::render {

// Stub (WP3 replaces it): every component is its own Anchor at rank 0, in
// room order, so a consumer written against the contract sees a legal --
// merely unranked -- graph.
RoomFlow buildRoomFlow(const RenderPage& page, const RenderRoom& room, const RenderModel& m) {
    (void)page;
    RoomFlow f;
    f.vertexOf.assign(m.design->components.size(), -1);
    f.ranks.emplace_back();
    for (std::size_t i = 0; i < room.components.size(); ++i) {
        std::uint32_t comp = room.components[i];
        FlowVertex v;
        v.kind = FlowVertex::Kind::Anchor;
        v.comps.push_back(comp);
        v.rank = 0;
        v.order = static_cast<int>(i);
        f.vertexOf[comp] = static_cast<std::int32_t>(f.verts.size());
        f.ranks[0].push_back(static_cast<std::uint32_t>(f.verts.size()));
        f.verts.push_back(std::move(v));
    }
    return f;
}

// Stub (WP3 replaces it): no adjacency preference.
InterRoomFlow buildInterRoomFlow(const RenderPage& page, const RenderModel& m) {
    (void)page;
    (void)m;
    return {};
}

}  // namespace manta::render
