// 'manta fmt' (spec 15.5, 17).
#include <format>

#include "cli/console.h"
#include "cli/driver.h"
#include "fmt/formatter.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace manta {

int runFormat(const Options& opts) {
    SourceManager sources;
    DiagEngine diags(sources, opts.severity);

    bool anyChanged = false;

    for (const std::string& input : opts.inputs) {
        auto loaded = sources.load(input);
        if (!loaded) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}: {}", loaded.error().path, loaded.error().message));
            continue;
        }
        const SourceFile* file = *loaded;

        Arena arena;
        StringInterner interner;
        DiagEngine fileDiags(sources, opts.severity);

        Lexer lexer(*file, fileDiags);
        TokenStream tokens = lexer.run();
        Parser parser(tokens, *file, arena, interner, fileDiags);
        SourceUnit unit = parser.run();

        if (fileDiags.hasErrors()) {
            // Spec 17 is normative about what the formatter emits, which
            // presupposes a parseable file. Reformatting a broken one would
            // destroy the very text the author needs to fix.
            diags.absorb(std::move(fileDiags));
            continue;
        }

        std::string formatted = formatUnit(unit, tokens, *file, interner);
        // Spec 1.4: "Line endings may be LF or CRLF, and the formatter
        // normalises them to LF." The printer emits LF throughout, so a file
        // that differs only in line endings is still a change.
        bool changed = formatted != file->text();
        if (changed) anyChanged = true;

        if (opts.showDiff) {
            std::string diff = unifiedDiff(file->text(), formatted, input);
            if (!diff.empty()) writeStdout(diff);
            continue;
        }
        if (opts.toStdout) {
            writeStdout(formatted);
            continue;
        }
        if (opts.checkOnly) {
            if (changed && !opts.quiet) {
                writeStderr(std::format("{}: would be reformatted\n", input));
            }
            continue;
        }
        if (!changed) continue;

        std::string error;
        if (!writeFileBinary(input, formatted, error)) {
            diags.report(DiagId::Io, Span{}, std::format("{}: {}", input, error));
            continue;
        }
        if (opts.verbose && !opts.quiet) writeStderr(std::format("  formatted {}\n", input));
    }

    // "--check: Exit non-zero if any file would change. Write nothing."
    int success = (opts.checkOnly && anyChanged) ? kExitError : kExitSuccess;
    return finish(diags, opts, success);
}

}  // namespace manta
