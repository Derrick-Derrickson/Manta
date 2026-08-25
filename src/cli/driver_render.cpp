// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// 'manta render': a netlist as a clickable HTML schematic, optionally printed
// to PDF through a headless Chromium.
#include <cstdlib>
#include <filesystem>
#include <format>

#include "cli/console.h"
#include "cli/driver.h"
#include "export/exporters.h"
#include "render/render.h"

namespace manta {

namespace {

// The browsers tools/make-pdf.py accepts, in the same order.
constexpr std::string_view kBrowsers[] = {"chromium", "chromium-browser", "google-chrome",
                                          "google-chrome-stable"};

std::string findBrowser() {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return {};
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string_view path(pathEnv);
    std::size_t at = 0;
    while (at <= path.size()) {
        std::size_t end = path.find(sep, at);
        if (end == std::string_view::npos) end = path.size();
        std::string_view dir = path.substr(at, end - at);
        if (!dir.empty()) {
            for (std::string_view name : kBrowsers) {
                std::filesystem::path candidate = std::filesystem::path(dir) / name;
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec)) return candidate.string();
            }
        }
        at = end + 1;
    }
    return {};
}

// The PDF is explicitly OUTSIDE the determinism guarantee: it is whatever the
// installed browser makes of the HTML. The HTML itself is inside it.
bool printToPdf(const std::string& browser, const std::string& htmlPath,
                const std::string& pdfPath) {
    // The flag recipe is tools/make-pdf.py's, which is known to survive both
    // sandboxed CI and snap-confined Chromium.
    std::string cmd = std::format(
        "\"{}\" --headless --disable-gpu --no-sandbox --no-pdf-header-footer"
        " --run-all-compositor-stages-before-draw --virtual-time-budget=10000"
        " \"--print-to-pdf={}\" \"{}\"",
        browser, pdfPath, htmlPath);
#ifndef _WIN32
    cmd += " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

}  // namespace

int runRender(const Options& opts) {
    SourceManager sources;
    DiagEngine diags(sources, opts.severity);

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

    render::RenderOptions renderOptions;
    renderOptions.title = opts.title;
    std::vector<std::string> renderWarnings;
    renderOptions.warnings = &renderWarnings;
    std::string html = render::renderSchematic(design, renderOptions);
    for (const std::string& w : renderWarnings) {
        diags.report(DiagId::Render, Span{}, w);
    }

    std::string outputPath =
        opts.output.empty() ? withExtension(input, ".html") : opts.output;
    if (!writeFileBinary(outputPath, html, error)) {
        diags.report(DiagId::Io, Span{}, std::format("{}: {}", outputPath, error));
        return finish(diags, opts, kExitError);
    }
    if (opts.verbose && !opts.quiet) {
        writeStderr(std::format("  {} -> {}\n", input, outputPath));
    }

    // The HTML above is already on disk whatever happens here: a missing
    // browser costs the PDF, never the render.
    if (!opts.pdfPath.empty()) {
        std::string browser = findBrowser();
        if (browser.empty()) {
            diags.report(DiagId::Io, Span{},
                         std::string("--pdf: no chromium, chromium-browser, google-chrome or "
                                     "google-chrome-stable on PATH"));
        } else if (!printToPdf(browser, outputPath, opts.pdfPath)) {
            diags.report(DiagId::Io, Span{},
                         std::format("--pdf: {} failed to print {}", browser, outputPath));
        } else if (std::error_code ec;
                   !std::filesystem::exists(std::filesystem::path(opts.pdfPath), ec)) {
            // A snap-confined browser exits 0 having written into its own
            // private filesystem when the path is one its confinement blocks
            // (a dot-directory, /tmp). Only the file's existence proves it
            // printed.
            diags.report(DiagId::Io, Span{},
                         std::format("--pdf: {} reported success but '{}' was not written; a "
                                     "sandboxed browser may be unable to write there",
                                     browser, opts.pdfPath));
        } else if (opts.verbose && !opts.quiet) {
            writeStderr(std::format("  {} -> {}\n", outputPath, opts.pdfPath));
        }
    }

    return finish(diags, opts);
}

}  // namespace manta
