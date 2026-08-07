// 'manta export' (spec 15.5).
//
// The specification names four targets -- kicad, altium, orcad, allegro -- but
// specifies no dialect for any of them. The dialects chosen are recorded in
// docs/assumptions.md, B3.
//
// Directives that a target's netlist format cannot carry are written to the
// --constraints sidecar rather than dropped, so no constraint is lost by
// exporting.
#pragma once

#include <string>

#include "diag/engine.h"
#include "json/json.h"
#include "link/netlist.h"

namespace manta {

enum class ExportFormat { KiCad, Altium, OrCad, Allegro };

[[nodiscard]] bool parseExportFormat(std::string_view name, ExportFormat& out);
[[nodiscard]] std::string_view exportExtension(ExportFormat format);

// Reads a .mantaNets file back into a Design.
[[nodiscard]] bool readNetlist(const JsonValue& root, DiagEngine& diags, Design& out);

// Renders the design in the target's format. `flatFormat` overrides
// @FLATFORMAT for hierarchical designators (spec 13.4).
[[nodiscard]] std::string exportDesign(const Design& design, ExportFormat format,
                                       std::string_view flatFormat);

// The constraints a target cannot express, as JSON.
[[nodiscard]] std::string exportConstraints(const Design& design);

}  // namespace manta
