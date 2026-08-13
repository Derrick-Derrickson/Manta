// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Cluster plans: the reference-style layout, decided before any geometry.
//
// A room is drawn as CLUSTERS, one per anchor IC or connector, the way a
// datasheet reference schematic is drawn: horizontal ARTERIES seeded at the
// anchor's left and right pins, with series parts inline on the wire, shunt
// strings hanging from junction dots, decoupling groups gathered per rail,
// and pull-ups/downs standing beside the pin they serve. This module is the
// planning half only -- it walks the flow graph, claims parts, and classifies
// nets; a later work package turns the plan into coordinates and wires.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/flow.h"
#include "render/model.h"
#include "render/sides.h"

namespace manta::render {

// One drawn two-terminal part of an artery or shunt string. `entryPin` is the
// pin facing the source -- the anchor for an inline part, the junction dot for
// a shunt element -- so the drawing package knows which way the body points.
struct ChainElem {
    std::uint32_t comp;      // index into Design::components
    std::uint32_t entryPin;  // 0 or 1: the pin the wire arrives on
};

// A string of parts hanging from a junction dot, walked away from the artery
// through private nets exactly as an inline chain is.
struct Shunt {
    std::vector<ChainElem> elems;  // junction-end first
    std::int32_t endNet = -1;      // where the string lands: its mark goes here
    bool up = false;               // endNet is a rail: the string stands above the line
};

// One step of an artery, in walk order away from the anchor pin.
struct ArteryStep {
    enum class Kind : std::uint8_t { Inline, Junction };
    Kind kind;
    ChainElem elem{};             // Inline: the series part (entry pin faces the source)
    std::int32_t net = -1;        // Junction: the multi-pin net at this point
    std::vector<Shunt> shunts;    // Junction only
};

// One horizontal wire seeded at an anchor pin. An artery ends on a net that
// keeps a mark (rail, ground, port...) or on a bare handoff net left Free for
// the router; only pins whose walk consumed at least one part get an artery
// at all -- a pin that stops immediately is a plain stub, no plan needed.
struct Artery {
    std::uint32_t anchorPin = 0;  // pin index on the cluster's anchor component
    std::int32_t startNet = -1;
    std::vector<ArteryStep> steps;
    std::int32_t endNet = -1;
    bool namedEnd = false;  // endNet keeps a terminal mark (partially covered multi-pin net)
};

// One cluster rail segment: a short local bar every consumer of the group
// taps. It forms when the cluster's consumers on the rail -- anchor pins,
// artery rail ends, claimed rail satellites, and the ladder caps below --
// number two or more (the classic pin-count rule, cluster-scoped), so
// `comps` may be empty: an anchor's own supply pins alone can earn it.
struct DecapGroup {
    std::int32_t rail = -1;
    std::vector<std::uint32_t> comps;  // ladder caps drawn under the bar, room order
};

// One cluster: an anchor (or child sheet symbol) and everything it claimed.
// The free cluster (anchorVert == -1, always last) collects what no anchor
// took; its rank is one past the room's last rank so the packing key stays
// total.
struct Cluster {
    std::int32_t anchorVert = -1;  // Anchor/Child flow vertex; -1 = the room's free cluster
    int rank = 0, order = 0;
    std::vector<Artery> left, right;              // per side, SidePlan slot order
    std::vector<std::uint32_t> satUps, satDowns;  // Vertical vertex ids
    std::vector<DecapGroup> decaps;
    std::vector<std::uint32_t> looseVerts;        // flow vertex ids, drawn as own cells
    std::vector<std::vector<ChainElem>> freeRuns; // free cluster only
    std::string shapeKey;                         // filled by a later package; leave empty
};

// What the plan decided about a page net. Free: the router labels or routes
// it. Drawn: every pin sits on drawn plan geometry, so no label is needed
// anywhere. Named: the plan draws part of it and every uncovered pin keeps a
// stub with the net's mark.
enum class NetState : std::uint8_t { Free, Drawn, Named };

struct RoomPlan {
    std::vector<Cluster> clusters;   // packing order: (rank, order), free cluster last
    std::vector<char> consumed;      // per component: drawn inside an artery/decap/satellite
    std::vector<NetState> netState;  // per page net
};

// Plans one room. `plans` is the per-component SidePlan vector the placer
// builds (indexed by Design::components; an empty plan means the builtin side
// heuristic, which is recomputed here for pin seeding order). Pure planning:
// deterministic, no geometry, nothing drawn.
[[nodiscard]] RoomPlan buildRoomPlan(const RenderPage&, const RenderRoom&, const RoomFlow&,
                                     const std::vector<SidePlan>&, const RenderModel&);

}  // namespace manta::render
