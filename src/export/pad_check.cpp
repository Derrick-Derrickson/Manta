// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "export/pad_check.h"

#include <algorithm>
#include <format>

#include "base/flat_map.h"
#include "export/footprint_map.h"

namespace manta {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace

std::vector<std::string> kicadPadNames(std::string_view module) {
    // Only the '(pad <name>' prefix matters, and both spellings occur: current
    // KiCad quotes the name, files from before version 6 may not. Anything
    // else that starts '(pad' -- '(pad_to_mask_clearance' -- is not followed
    // by whitespace and is skipped.
    std::vector<std::string> names;
    std::size_t at = 0;
    while ((at = module.find("(pad", at)) != std::string_view::npos) {
        at += 4;
        if (at >= module.size() || !isSpace(module[at])) continue;
        while (at < module.size() && isSpace(module[at])) ++at;
        std::string name;
        if (at < module.size() && module[at] == '"') {
            ++at;
            while (at < module.size() && module[at] != '"') {
                if (module[at] == '\\' && at + 1 < module.size()) ++at;
                name += module[at++];
            }
        } else {
            while (at < module.size() && !isSpace(module[at]) && module[at] != ')') {
                name += module[at++];
            }
        }
        if (name.empty()) continue;
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
}

void checkPads(const Design& design, const ExportOptions& options, const FootprintLoader& load,
               DiagEngine& diags) {
    // Every instance of a part has the part's pins, so a part and footprint
    // pair is checked once, at its first instance, in component order.
    FlatSet<std::string> seen;
    FlatSet<std::string> missing;
    for (const Component& c : design.components) {
        bool qualified = false;
        std::string footprint =
            resolveFootprint(options.footprints, options.footprintLib, c.footprint, qualified);
        // An unqualified name has already drawn W-FOOTPRINT from the exporter.
        if (!qualified) continue;
        std::string key = c.partName + '\n' + footprint;
        if (seen.contains(key)) continue;
        seen.insert(key);

        std::size_t colon = footprint.find(':');
        std::string text;
        if (!load(std::string_view(footprint).substr(0, colon),
                  std::string_view(footprint).substr(colon + 1), text)) {
            if (!missing.contains(footprint)) {
                missing.insert(footprint);
                diags.report(DiagId::Footprint, c.span,
                             std::format("footprint '{}' is in no --footprint-dir; its pads "
                                         "were not checked",
                                         footprint));
            }
            continue;
        }

        std::vector<std::string> pads = kicadPadNames(text);
        auto isPad = [&](const std::string& name) {
            return std::find(pads.begin(), pads.end(), name) != pads.end();
        };
        for (const ComponentPin& pin : c.pins) {
            if (isPad(pin.physical)) continue;
            diags.report(DiagId::Pad, c.span,
                         std::format("part '{}' declares pin {} as pad '{}', which footprint "
                                     "'{}' does not have; it would connect nothing",
                                     c.partName, pin.logical, pin.physical, footprint));
        }
        // One warning naming them all: a part written against the wrong
        // footprint would otherwise bury its errors under a line per pad.
        std::string undeclared;
        for (const std::string& pad : pads) {
            bool declared = std::any_of(c.pins.begin(), c.pins.end(),
                                        [&](const ComponentPin& p) { return p.physical == pad; });
            if (declared) continue;
            if (!undeclared.empty()) undeclared += ", ";
            undeclared += pad;
        }
        if (!undeclared.empty()) {
            diags.report(DiagId::UndeclaredPad, c.span,
                         std::format("footprint '{}' has pads part '{}' does not declare: {}",
                                     footprint, c.partName, undeclared));
        }
    }
}

}  // namespace manta
