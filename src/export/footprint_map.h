// Mapping a manta footprint name to the name a layout tool knows it by.
//
// A part library says '@~footprint = R-0603' because that is what the package
// is, not because of anything a particular EDA tool calls it. KiCad wants
// 'Resistor_SMD:R_0603_1608Metric', resolved against its footprint library
// table; Allegro wants something else again. Keeping the translation in a file
// beside the design rather than in the part means one library serves every
// export target, which is the point of having four of them.
//
// The file is plain text rather than JSON because it is written and read by
// hand. writeElaborationMap (src/link/netlist.cpp) sets the precedent.
//
//     # comments run to end of line
//     R-0603     Resistor_SMD:R_0603_1608Metric
//     SOT-23-5   Package_TO_SOT_SMD:SOT-23-5
#pragma once

#include <string>
#include <string_view>

#include "base/flat_map.h"
#include "diag/engine.h"

namespace manta {

struct FootprintMap {
    // Insertion-ordered, so a diagnostic about this file reports entries in the
    // order they were written (spec 15.8).
    FlatMap<std::string, std::string> entries;
};

// Parses a map file. Reports E-IO and returns false on a malformed line, a
// duplicate key, or a target with no ':' -- which would defeat the whole
// purpose of writing the file.
[[nodiscard]] bool loadFootprintMap(std::string_view text, std::string_view path,
                                    DiagEngine& diags, FootprintMap& out);

// The resolution ladder, in order:
//
//   1. an entry in the map file;
//   2. otherwise the raw name, if it already names a library;
//   3. otherwise '<defaultLib>:<raw>', when a default was given;
//   4. otherwise the raw name, with `qualified` left false.
//
// `map` may be null when no file was supplied.
[[nodiscard]] std::string resolveFootprint(const FootprintMap* map, std::string_view defaultLib,
                                           std::string_view raw, bool& qualified);

}  // namespace manta
