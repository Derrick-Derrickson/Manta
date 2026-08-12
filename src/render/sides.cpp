// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Flow-aware pin sides: a pin faces the ranked position of its net's far end.
//
// Everything here is deterministic (spec 15.8): every ordering is total, every
// tie broken explicitly, and no decision reads Component::pins order except as
// the very last tie-break between pins whose names are byte-identical -- so a
// permutation of the pin list yields the same plan per pin name.
#include "render/sides.h"

#include <algorithm>
#include <limits>

namespace manta::render {

namespace {

// Slot-sort key for a pin with no far end: sorts after every real partner
// (ranks and orders are small non-negative ints in practice).
constexpr int kNoPartner = std::numeric_limits<int>::max();

// The pin is electrically dead on this sheet: typed NC, explicitly unbound
// ("&NET=?", spec 11.6), or simply on no net. These become the contiguous
// greyed block at the bottom of the right column; the existing rendering
// already greys and crosses them.
bool isDead(const ComponentPin& p) {
    return p.type == PinType::NC || p.unbound || p.net < 0;
}

struct Partner {
    bool found = false;      // some far-end vertex exists, at any rank
    bool before = false;     // the chosen partner ranks strictly left of us
    bool after = false;      // ... strictly right of us
    int rank = kNoPartner;   // the chosen partner's rank (slot-sort key)
    int order = kNoPartner;  // ... and its order within that rank
};

// The net's other significant end, seen from `comp` (vertex `ownVert`, rank
// `ownRank`). Candidates are the net's other pins whose component has a
// vertex in this room's flow -- vertexOf is -1 for everything outside the
// room -- excluding our own vertex (a series-string partner is the same drawn
// column, not a far end). Preference: any candidate strictly before us beats
// any strictly after, which beats any at our own rank (an equal-rank
// neighbour fixes slot order but expresses no side). Within the preferred
// class the winner is the minimum by (rank, order, vertex index): "lowest
// rank" per the plan, then leftmost in that rank, then the smaller vertex
// index -- vertex indices come from room order, not pin order, so the choice
// is permutation-stable.
Partner findPartner(const Design& d, const RoomFlow& flow, std::uint32_t comp,
                    std::int32_t ownVert, int ownRank, std::int32_t net) {
    Partner best;
    if (net < 0) return best;
    int bestClass = 3;
    std::int32_t bestVert = -1;
    for (const PinRef& ref : d.nets[static_cast<std::size_t>(net)].pins) {
        if (ref.component == comp) continue;
        if (ref.component >= flow.vertexOf.size()) continue;
        std::int32_t v = flow.vertexOf[ref.component];
        if (v < 0 || v == ownVert) continue;
        const FlowVertex& fv = flow.verts[static_cast<std::size_t>(v)];
        int cls = fv.rank < ownRank ? 0 : fv.rank > ownRank ? 1 : 2;
        bool take = false;
        if (cls != bestClass) {
            take = cls < bestClass;
        } else if (fv.rank != best.rank) {
            take = fv.rank < best.rank;
        } else if (fv.order != best.order) {
            take = fv.order < best.order;
        } else {
            take = v < bestVert;
        }
        if (take) {
            bestClass = cls;
            best.rank = fv.rank;
            best.order = fv.order;
            bestVert = v;
        }
    }
    if (bestVert >= 0) {
        best.found = true;
        best.before = bestClass == 0;
        best.after = bestClass == 1;
    }
    return best;
}

// Name-first comparison of two pins: natural order of the logical name, then
// of the physical id, then pin index. The index is reached only when both
// names are byte-identical, i.e. the pins are indistinguishable, so plans
// agree per pin name under any permutation of Component::pins.
bool nameLess(const Component& c, std::uint32_t a, std::uint32_t b) {
    const ComponentPin& pa = c.pins[a];
    const ComponentPin& pb = c.pins[b];
    if (naturalLess(pa.logical, pb.logical)) return true;
    if (naturalLess(pb.logical, pa.logical)) return false;
    if (naturalLess(pa.physical, pb.physical)) return true;
    if (naturalLess(pb.physical, pa.physical)) return false;
    return a < b;
}

// Physical-first comparison: today's natural pin-number order, used for the
// Top/Bottom rows and the dead block. Logical name before index for the same
// permutation-stability reason as nameLess.
bool numberLess(const Component& c, std::uint32_t a, std::uint32_t b) {
    const ComponentPin& pa = c.pins[a];
    const ComponentPin& pb = c.pins[b];
    if (naturalLess(pa.physical, pb.physical)) return true;
    if (naturalLess(pb.physical, pa.physical)) return false;
    if (naturalLess(pa.logical, pb.logical)) return true;
    if (naturalLess(pb.logical, pa.logical)) return false;
    return a < b;
}

// Slot order along a horizontal side: partner order first -- two pins wired
// to the same far vertex (USART1-TX and -RX into one connector) end up
// adjacent -- then partner rank, then pin names. Partnerless pins carry
// kNoPartner keys, so they gather after every partnered pin.
bool flowLess(const Component& c, const std::vector<Partner>& partner, std::uint32_t a,
              std::uint32_t b) {
    const Partner& fa = partner[a];
    const Partner& fb = partner[b];
    if (fa.order != fb.order) return fa.order < fb.order;
    if (fa.rank != fb.rank) return fa.rank < fb.rank;
    return nameLess(c, a, b);
}

}  // namespace

SidePlan planSides(std::uint32_t comp, const RoomFlow& flow, const RenderPage& page,
                   const RenderModel& m) {
    const Design& d = *m.design;
    if (comp >= d.components.size()) return {};
    // Only the box symbols take a plan; classic artwork owns its pin places.
    SymbolKind kind = m.kinds[comp];
    if (kind != SymbolKind::Generic && kind != SymbolKind::Connector) return {};
    const Component& c = d.components[comp];
    const std::size_t n = c.pins.size();
    if (n == 0) return {};

    std::int32_t ownVert = comp < flow.vertexOf.size() ? flow.vertexOf[comp] : -1;
    int ownRank = ownVert >= 0 ? flow.verts[static_cast<std::size_t>(ownVert)].rank : 0;

    std::vector<Partner> partner(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!isDead(c.pins[i])) {
            partner[i] = findPartner(d, flow, comp, ownVert, ownRank, c.pins[i].net);
        }
    }

    // A pin shows its net's name inside the body only when the wire alone
    // would not say it: the net is a plain routed label net (rails, grounds
    // and port/crossing flags already print their name at the mark) and the
    // page-local display name differs from the silicon name. Dead pins never.
    auto shows = [&](std::uint32_t i) {
        const ComponentPin& p = c.pins[i];
        if (isDead(p)) return false;
        if (static_cast<std::size_t>(p.net) >= page.nets.size()) return false;
        const RenderNet& rn = page.nets[static_cast<std::size_t>(p.net)];
        if (rn.mark != NetMark::Label) return false;
        if (rn.crossing || rn.direction != PortDir::None) return false;
        return p.logical != rn.display;
    };

    SidePlan plan;
    plan.byPin.resize(n);

    if (kind == SymbolKind::Connector) {
        // A connector stays one column of numbered pins -- the body faces by
        // placement, not by pin sides. It receives from the left when placed
        // at the sheet's right or bottom edge ('&EDGE', spec 11.10), so its
        // pins flip to the Left side then.
        Side side = (c.edge == "RIGHT" || c.edge == "BOTTOM") ? Side::Left : Side::Right;
        std::vector<std::uint32_t> rows(n);
        for (std::uint32_t i = 0; i < n; ++i) rows[i] = i;
        // Slot order: partner vertex order (rank as its qualifier -- order is
        // only defined within a rank), then natural pin-number order; dead
        // and partnerless pins carry kNoPartner keys and gather at the end.
        std::sort(rows.begin(), rows.end(), [&](std::uint32_t a, std::uint32_t b) {
            const Partner& fa = partner[a];
            const Partner& fb = partner[b];
            if (fa.order != fb.order) return fa.order < fb.order;
            if (fa.rank != fb.rank) return fa.rank < fb.rank;
            return numberLess(c, a, b);
        });
        for (std::size_t s = 0; s < rows.size(); ++s) {
            plan.byPin[rows[s]] = PinPlan{side, static_cast<int>(s), shows(rows[s])};
        }
        return plan;
    }

    // Generic box. Fixed sides first, then partners, then balance.
    std::vector<std::uint32_t> left, right, top, bottom, dead, floating;
    for (std::uint32_t i = 0; i < n; ++i) {
        const ComponentPin& p = c.pins[i];
        if (isDead(p)) {
            dead.push_back(i);
            continue;
        }
        NetMark mark = static_cast<std::size_t>(p.net) < page.nets.size()
                           ? page.nets[static_cast<std::size_t>(p.net)].mark
                           : NetMark::Label;
        // The net's evidence outranks the pin's declared type: a part is free
        // to type every supply pin POWER (WCH's CH32 datasheet does), but a
        // pin standing on a ground-marked net drinks downward wherever the
        // silicon vendor filed it, and its ground mark must not point up.
        if (mark == NetMark::Ground) {
            bottom.push_back(i);
        } else if (mark == NetMark::Rail) {
            // A rail is ambient, not a far end: any pin tied to a rail net
            // (a Passive one included) goes Top, even if the net also lands
            // on ranked components elsewhere in the room.
            top.push_back(i);
        } else if (p.type == PinType::Power) {
            top.push_back(i);
        } else if (p.type == PinType::Ground) {
            bottom.push_back(i);
        } else if (partner[i].before) {
            left.push_back(i);
        } else if (partner[i].after) {
            right.push_back(i);
        } else {
            // No partner at all, or one at our own rank (which fixes slot
            // order but no side): balanced below.
            floating.push_back(i);
        }
    }

    // Balance: partnerless pins go to whichever of Left/Right holds fewer
    // live signal pins so far (the dead block never counts), ties to Left.
    // They are taken in name order, not pin order, so the split -- not just
    // the membership -- is identical under any pin permutation.
    std::sort(floating.begin(), floating.end(),
              [&](std::uint32_t a, std::uint32_t b) { return nameLess(c, a, b); });
    for (std::uint32_t i : floating) {
        if (left.size() <= right.size()) left.push_back(i);
        else right.push_back(i);
    }

    // Slot order. Left/Right by flow keys; Top/Bottom keep today's natural
    // pin-number order; the dead block continues the right column's numbering
    // after every live pin, itself in natural pin-number order.
    std::sort(left.begin(), left.end(),
              [&](std::uint32_t a, std::uint32_t b) { return flowLess(c, partner, a, b); });
    std::sort(right.begin(), right.end(),
              [&](std::uint32_t a, std::uint32_t b) { return flowLess(c, partner, a, b); });
    std::sort(top.begin(), top.end(),
              [&](std::uint32_t a, std::uint32_t b) { return numberLess(c, a, b); });
    std::sort(bottom.begin(), bottom.end(),
              [&](std::uint32_t a, std::uint32_t b) { return numberLess(c, a, b); });
    std::sort(dead.begin(), dead.end(),
              [&](std::uint32_t a, std::uint32_t b) { return numberLess(c, a, b); });

    auto assign = [&](const std::vector<std::uint32_t>& idx, Side side, int firstSlot,
                      bool showAllowed) {
        for (std::size_t s = 0; s < idx.size(); ++s) {
            plan.byPin[idx[s]] = PinPlan{side, firstSlot + static_cast<int>(s),
                                         showAllowed && shows(idx[s])};
        }
    };
    assign(left, Side::Left, 0, true);
    assign(right, Side::Right, 0, true);
    // Top/Bottom pins never show a net name: their rail/ground marks (or the
    // label their stub keeps) already say it, and the box widens for inside
    // net text on the horizontal rows only.
    assign(top, Side::Top, 0, false);
    assign(bottom, Side::Bottom, 0, false);
    assign(dead, Side::Right, static_cast<int>(right.size()), false);
    return plan;
}

}  // namespace manta::render
