// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// A user-defined bill of materials: 'manta link --bom-template FILE'.
//
// The built-in BOM is one row per component with a column per '#' field. A
// supplier or an assembler wants something else -- rows grouped by value and
// footprint, columns named their way -- and every one wants a different
// something, so the shape is the user's to describe, never manta's to build
// in. The file is TOML and may describe other outputs as well; manta takes
// the one '[[output]]' with kind = "bom" and ignores the rest.
// docs/bom-template.md is the reference.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "diag/engine.h"
#include "export/footprint_map.h"
#include "link/netlist.h"
#include "toml/toml.h"

namespace manta {

// One '{...}' or a run of literal text.
struct TemplatePiece {
    enum class Kind : std::uint8_t {
        Text, Ref, Refs, Qty, Value, Footprint, FootprintName, Lib, Part, Type, Fitted, Field
    };
    Kind kind = Kind::Text;
    std::string text;      // Text: the literal; Field: the field name
    std::string fallback;  // Field: after '|'
};

struct Template {
    std::vector<TemplatePiece> pieces;
    bool perGroup = false;  // mentions {refs} or {qty}
};

struct BomTemplate {
    std::vector<Template> groupBy;
    Template sort;
    bool skipDnp = false;
    std::string separator = ",";
    std::string refsSeparator = ",";
    enum class Quote : std::uint8_t { Always, Needed, Never } quote = Quote::Needed;
    std::vector<std::pair<std::string, Template>> columns;
};

// Reads the template from a parsed file. Every problem -- a missing key, an
// unknown placeholder, a placement-only placeholder, a combination that would
// corrupt a row -- is reported against `path` and makes it return false.
[[nodiscard]] bool loadBomTemplate(const TomlValue& root, std::string_view path,
                                   DiagEngine& diags, BomTemplate& out);

// Renders the design. Returns false, having reported why, when a column
// differs between components of one group or a value cannot be written under
// quote = "never". `footprints` may be null.
[[nodiscard]] bool renderBomTemplate(const Design& design, const BomTemplate& tpl,
                                     const FootprintMap* footprints, DiagEngine& diags,
                                     std::string& out);

// Natural order: digit runs compare as numbers, so R2 sorts before R10.
[[nodiscard]] bool naturalLess(std::string_view a, std::string_view b);

}  // namespace manta
