// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// 'manta render': a netlist as a clickable HTML schematic.
//
// The output is one self-contained file -- inline SVG, inline CSS, inline
// script, no external resources -- and is inside the determinism guarantee of
// spec 15.8: the same netlist renders to the same bytes on every platform.
#pragma once

#include <cstdint>
#include <string>

#include "link/netlist.h"

namespace manta::render {

struct RenderOptions {
    std::string title;  // title block override; empty means Design::top
    // Which placement engine lays the sheets out. Classic is the shipping
    // band/idiom engine; Flow is the flow-ranked placer being built behind
    // this flag, and delegates to Classic until it lands -- the hidden
    // '--layout' option selects it.
    enum class Pipeline : std::uint8_t { Classic, Flow } pipeline = Pipeline::Classic;
};

[[nodiscard]] std::string renderSchematic(const Design& design, const RenderOptions& options);

}  // namespace manta::render
