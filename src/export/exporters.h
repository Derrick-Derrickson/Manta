// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// 'manta export' (spec 15.5).
//
// The specification names four targets -- kicad, altium, orcad, allegro -- but
// specifies no dialect for any of them. The dialects chosen are recorded in
// docs/assumptions.md, C4.
//
// Directives that a target's netlist format cannot carry are written to the
// --constraints sidecar rather than dropped, so no constraint is lost by
// exporting.
#pragma once

#include <string>

#include "diag/engine.h"
#include "export/footprint_map.h"
#include "json/json.h"
#include "link/netlist.h"

namespace manta {

enum class ExportFormat { KiCad, Altium, OrCad, Allegro };

[[nodiscard]] bool parseExportFormat(std::string_view name, ExportFormat& out);
[[nodiscard]] std::string_view exportExtension(ExportFormat format);

// Reads a .mantaNets file back into a Design.
[[nodiscard]] bool readNetlist(const JsonValue& root, DiagEngine& diags, Design& out);

struct ExportOptions {
    // Overrides @FLATFORMAT for hierarchical designators (spec 13.4).
    std::string_view flatFormat;
    // Null when no map file was given. Applies to every format: it is only a
    // rename table, and what belongs in it is the caller's business.
    const FootprintMap* footprints = nullptr;
    // The default library nickname. KiCad only -- a nickname means nothing to
    // Allegro -- and so is the W-FOOTPRINT warning that goes with it.
    std::string_view footprintLib;
};

// Renders the design in the target's format.
[[nodiscard]] std::string exportDesign(const Design& design, ExportFormat format,
                                       const ExportOptions& options, DiagEngine& diags);

// The constraints a target cannot express, as JSON.
[[nodiscard]] std::string exportConstraints(const Design& design);

}  // namespace manta
