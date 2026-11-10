// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Checking a design's pins against the pads of the KiCad footprints it names.
//
// A part's pin numbers are meant to be its footprint's pad names, and nothing
// in the language can see a footprint, so a part that says '7 : SHIELD' against
// a footprint whose shell pad is 'SH' links, checks and exports clean and then
// connects nothing on the board. Once the export has resolved a footprint to
// 'Library:Footprint' the pad names are one file away, so this is where the
// mistake can be caught before layout.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "diag/engine.h"
#include "export/exporters.h"
#include "link/netlist.h"

namespace manta {

// The pad names in a .kicad_mod, first-seen order, each once. A pad with an
// empty name is a mechanical hole and is left out: nothing can connect to it.
[[nodiscard]] std::vector<std::string> kicadPadNames(std::string_view module);

// Reads 'Library:Footprint' into `text`; false when it is not found. The
// caller owns the filesystem, which keeps this testable.
using FootprintLoader =
    std::function<bool(std::string_view library, std::string_view footprint, std::string& text)>;

// For each part and footprint pair, once:
//
//   E-PAD        a declared pin whose number is not a pad -- it would connect
//                nothing, so the export must not be written;
//   W-PAD        a pad the part does not declare -- the parts rule is that
//                every pad is declared, mechanical ones included;
//   W-FOOTPRINT  a qualified footprint the loader cannot find, so its pads
//                went unchecked.
void checkPads(const Design& design, const ExportOptions& options, const FootprintLoader& load,
               DiagEngine& diags);

}  // namespace manta
