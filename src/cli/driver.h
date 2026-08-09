// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The subcommand drivers.
#pragma once

#include <string>
#include <vector>

#include "cli/options.h"
#include "diag/engine.h"
#include "source/source_manager.h"

namespace manta {

// Shared by every subcommand: renders buffered diagnostics to stderr in the
// form of spec 15.6 and maps the result onto the exit codes of spec 15.7.
int finish(const DiagEngine& diags, const Options& opts, int successCode = kExitSuccess);

// Runs the parse pipeline over the named sources in parallel, merging
// diagnostics in command-line order so that -j never perturbs stderr
// (spec 15.8). Used by 'compile' and, over source rather than objects, by 'fmt'
// and 'annotate'.
int runCompile(const Options& opts);
int runLink(const Options& opts);
int runAnnotate(const Options& opts);
int runFormat(const Options& opts);
int runExport(const Options& opts);
int runRender(const Options& opts);

// Resolves an input name against the -I search path, returning the first
// existing candidate or the name unchanged.
[[nodiscard]] std::string resolveInput(const std::string& name,
                                       const std::vector<std::string>& searchDirs);

// Replaces a path's extension: "src/a.manta" -> "src/a.mantaO".
[[nodiscard]] std::string withExtension(std::string_view path, std::string_view newExtension);
[[nodiscard]] std::string baseName(std::string_view path);
[[nodiscard]] bool isDirectory(const std::string& path);

}  // namespace manta
