// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli/rules_loader.h"

#include <format>

#include "lex/lexer.h"
#include "rules/rules_parser.h"

namespace manta {

LoadedRuleFiles loadRuleFiles(const Options& opts, SourceManager& sources, Arena& arena,
                              StringInterner& interner, DiagEngine& diags,
                              SeverityPolicy& severity) {
    LoadedRuleFiles out;

    for (const std::string& path : opts.ruleFiles) {
        auto loaded = sources.load(path);
        if (!loaded) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}: {}", loaded.error().path, loaded.error().message));
            out.ok = false;
            continue;
        }
        const SourceFile* file = *loaded;

        Lexer lexer(*file, diags);
        TokenStream tokens = lexer.run();
        RulesParser parser(tokens, *file, arena, interner, diags);
        out.files.push_back(parser.run());
    }

    for (const RuleFile& file : out.files) {
        LoadedRules loaded = loadRules(file, interner, diags);
        for (const RuleCheck* c : loaded.checks) out.rules.checks.push_back(c);
        for (const auto& [name, decl] : loaded.fieldTypes) out.rules.fieldTypes.set(name, decl);
    }

    // Resolve the -W names held back at option-parsing time. A name that no
    // rule claims is a usage error: silently ignoring it would let a typo in
    // "-Wno-drive-hgih" quietly leave the check switched on.
    for (const auto& [name, level] : opts.pendingSeverities) {
        bool claimed = false;
        for (const RuleCheck* c : out.rules.checks) {
            if (interner.text(c->name) == name) {
                claimed = true;
                break;
            }
        }
        if (claimed) {
            severity.setUser(name, level);
        } else {
            diags.report(DiagId::Usage, Span{},
                         std::format("unknown diagnostic '{}'; it is not a built-in code, a "
                                     "mnemonic, or a check in any --rules file",
                                     name));
            out.ok = false;
        }
    }

    if (diags.hasErrors()) out.ok = false;
    return out;
}

}  // namespace manta
