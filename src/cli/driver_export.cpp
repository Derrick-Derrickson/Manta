// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// 'manta export' (spec 15.5).
#include <format>

#include "cli/console.h"
#include "cli/driver.h"
#include "export/exporters.h"
#include "export/pad_check.h"

namespace manta {

int runExport(const Options& opts) {
    SourceManager sources;
    DiagEngine diags(sources, opts.severity);

    ExportFormat format{};
    if (!parseExportFormat(opts.format, format)) {
        writeStderr(std::format(
            "manta: unknown format '{}'; expected kicad, altium, orcad or allegro\n",
            opts.format));
        return kExitUsage;
    }

    const std::string& input = opts.inputs.front();
    std::string text;
    std::string error;
    if (!readFileBinary(input, text, error)) {
        diags.report(DiagId::Io, Span{}, std::format("{}: {}", input, error));
        return finish(diags, opts, kExitUsage);
    }

    JsonParseError jsonError;
    JsonPtr root = jsonParse(text, jsonError);
    if (!root) {
        diags.report(DiagId::Io, Span{},
                     std::format("{}: {} at byte {}", input, jsonError.message, jsonError.offset));
        return finish(diags, opts, kExitUsage);
    }

    Design design;
    if (!readNetlist(*root, diags, design)) return finish(diags, opts, kExitUsage);

    // Loaded before anything is rendered: a map file with a bad line is a
    // mistake in what the board is meant to be, not a detail to discover
    // halfway through writing the output.
    FootprintMap footprints;
    if (!opts.footprintMapPath.empty()) {
        std::string mapText;
        if (!readFileBinary(opts.footprintMapPath, mapText, error)) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}: {}", opts.footprintMapPath, error));
            return finish(diags, opts, kExitUsage);
        }
        if (!loadFootprintMap(mapText, opts.footprintMapPath, diags, footprints)) {
            return finish(diags, opts, kExitUsage);
        }
    }

    ExportOptions exportOptions;
    exportOptions.flatFormat = opts.flatFormat;
    exportOptions.footprints = opts.footprintMapPath.empty() ? nullptr : &footprints;
    exportOptions.footprintLib = opts.footprintLib;

    std::string rendered = exportDesign(design, format, exportOptions, diags);

    if (!opts.footprintDirs.empty()) {
        auto load = [&](std::string_view library, std::string_view footprint,
                        std::string& module) {
            std::string ignored;
            for (const std::string& dir : opts.footprintDirs) {
                std::string path = std::format("{}/{}.pretty/{}.kicad_mod", dir, library, footprint);
                if (readFileBinary(path, module, ignored)) return true;
            }
            return false;
        };
        checkPads(design, exportOptions, load, diags);
    }
    // Errors mean no output. That covers a pin that is no pad, which would
    // connect nothing, and a warning promoted by -Werror.
    if (diags.hasErrors()) return finish(diags, opts, kExitError);

    std::string outputPath = opts.output.empty()
                                 ? withExtension(input, exportExtension(format))
                                 : opts.output;
    if (!writeFileBinary(outputPath, rendered, error)) {
        diags.report(DiagId::Io, Span{}, std::format("{}: {}", outputPath, error));
        return finish(diags, opts, kExitError);
    }
    if (opts.verbose && !opts.quiet) {
        writeStderr(std::format("  {} -> {} ({})\n", input, outputPath, opts.format));
    }

    if (!opts.constraintsPath.empty()) {
        std::string constraints = exportConstraints(design);
        if (!writeFileBinary(opts.constraintsPath, constraints, error)) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}: {}", opts.constraintsPath, error));
        }
    }

    return finish(diags, opts);
}

}  // namespace manta
