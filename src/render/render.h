// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// 'manta render': a netlist as a clickable HTML schematic.
//
// The output is one self-contained file -- inline SVG, inline CSS, inline
// script, no external resources -- and is inside the determinism guarantee of
// spec 15.8: the same netlist renders to the same bytes on every platform.
#pragma once

#include <string>
#include <vector>

#include "link/netlist.h"

namespace manta::render {

struct RenderOptions {
    std::string title;  // title block override; empty means Design::top
    // When set, collects one line per '&RENDER=WIRE' net that had to fall
    // back to a name (revision 1.6). The caller reports them as W-RENDER.
    std::vector<std::string>* warnings = nullptr;
};

[[nodiscard]] std::string renderSchematic(const Design& design, const RenderOptions& options);

}  // namespace manta::render
