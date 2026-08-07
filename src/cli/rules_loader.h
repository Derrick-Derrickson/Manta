// Loading .mantaRules files for the commands that accept --rules.
//
// Shared by 'compile' and 'link' because both need the same three steps: parse
// the files, collect the checks, and resolve the -W names that could not be
// validated when options were parsed.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "base/arena.h"
#include "base/intern.h"
#include "cli/options.h"
#include "diag/engine.h"
#include "rules/rules_eval.h"
#include "source/source_manager.h"

namespace manta {

struct LoadedRuleFiles {
    RuleFile parsed;
    LoadedRules rules;
    std::vector<RuleFile> files;
    bool ok = true;
};

// Parses every --rules file and collects its checks. `severity` is updated with
// the pending -W names that a rule claims; one that no rule claims is a usage
// error, which is the latest point at which it can be caught.
LoadedRuleFiles loadRuleFiles(const Options& opts, SourceManager& sources, Arena& arena,
                              StringInterner& interner, DiagEngine& diags,
                              SeverityPolicy& severity);

}  // namespace manta
