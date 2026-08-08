#include "cli/options.h"

#include <charconv>
#include <format>

#include "cli/console.h"
#include "obj/mantao.h"

namespace manta {

namespace {

struct Cursor {
    const std::vector<std::string>& args;
    std::size_t i;

    [[nodiscard]] bool done() const { return i >= args.size(); }
    [[nodiscard]] const std::string& cur() const { return args[i]; }
};

// Accepts "--name value" and "--name=value", and for short options both
// "-o value" and "-ovalue".
bool takeValue(Cursor& c, std::string_view longName, std::string_view shortName,
               std::string& out, std::string& error) {
    const std::string& a = c.args[c.i];

    if (!longName.empty() && a.starts_with(longName)) {
        if (a.size() == longName.size()) {
            if (c.i + 1 >= c.args.size()) {
                error = std::format("option '{}' requires a value", longName);
                return false;
            }
            out = c.args[++c.i];
            return true;
        }
        if (a[longName.size()] == '=') {
            out = a.substr(longName.size() + 1);
            return true;
        }
    }

    if (!shortName.empty() && a.starts_with(shortName)) {
        if (a.size() == shortName.size()) {
            if (c.i + 1 >= c.args.size()) {
                error = std::format("option '{}' requires a value", shortName);
                return false;
            }
            out = c.args[++c.i];
            return true;
        }
        out = a.substr(shortName.size());
        return true;
    }

    return false;
}

Command commandFromName(std::string_view name) {
    if (name == "compile") return Command::Compile;
    if (name == "link") return Command::Link;
    if (name == "check") return Command::Check;
    if (name == "annotate") return Command::Annotate;
    if (name == "fmt") return Command::Format;
    if (name == "export") return Command::Export;
    return Command::None;
}

std::string_view commandName(Command c) {
    switch (c) {
        case Command::Compile: return "compile";
        case Command::Link: return "link";
        case Command::Check: return "check";
        case Command::Annotate: return "annotate";
        case Command::Format: return "fmt";
        case Command::Export: return "export";
        case Command::None: return "";
    }
    return "";
}

// -W<name>, -Wno-<name>, --error=<code>, --warn=<code>.
bool applySeverityOption(const std::string& a, Options& out, std::string& error) {
    auto resolve = [&](std::string_view name, Severity s) {
        DiagId id{};
        if (lookupDiag(name, id)) {
            out.severity.set(id, s);
            return true;
        }
        // Not a built-in. It may name a check in a rules file, which has not
        // been read yet -- options are parsed first. Hold it, and fail later if
        // no rule claims it.
        out.pendingSeverities.emplace_back(std::string(name), s);
        (void)error;
        return true;
    };

    if (a == "-Werror") {
        out.severity.setWerror(true);
        return true;
    }
    if (a.starts_with("-Wno-")) return resolve(std::string_view(a).substr(5), Severity::Ignored);
    if (a.starts_with("--error=")) return resolve(std::string_view(a).substr(8), Severity::Error);
    if (a.starts_with("--warn=")) return resolve(std::string_view(a).substr(7), Severity::Warning);
    if (a.starts_with("-W") && a.size() > 2) {
        return resolve(std::string_view(a).substr(2), Severity::Warning);
    }
    return true;
}

bool isSeverityOption(const std::string& a) {
    return a.starts_with("-W") || a.starts_with("--error=") || a.starts_with("--warn=");
}

}  // namespace

ParseOutcome parseOptions(const std::vector<std::string>& args, Options& out) {
    ParseOutcome result;

    // Colour defaults to on for an interactive stderr and off otherwise, which
    // keeps redirected output clean without needing --no-colour (spec 15.5).
    out.colour = stderrIsTerminal();

    if (args.size() < 2) {
        out.showHelp = true;
        return result;
    }

    std::size_t start = 1;
    // "manta --help" and "manta --version" work without a subcommand.
    if (args[1] == "--help" || args[1] == "-h") {
        out.showHelp = true;
        return result;
    }
    if (args[1] == "--version") {
        out.showVersion = true;
        return result;
    }

    out.command = commandFromName(args[1]);
    if (out.command == Command::None) {
        result.ok = false;
        result.exitCode = kExitUsage;
        result.error = std::format("unknown command '{}'", args[1]);
        return result;
    }
    ++start;

    Cursor c{args, start};
    bool noMoreOptions = false;

    for (; !c.done(); ++c.i) {
        const std::string& a = c.cur();

        if (noMoreOptions || a == "-" || !a.starts_with("-")) {
            out.inputs.push_back(a);
            continue;
        }
        if (a == "--") {
            noMoreOptions = true;
            continue;
        }

        // ---- common options ------------------------------------------------
        if (a == "--help" || a == "-h") {
            out.showHelp = true;
            return result;
        }
        if (a == "--version") {
            out.showVersion = true;
            return result;
        }
        if (isSeverityOption(a)) {
            if (!applySeverityOption(a, out, result.error)) {
                result.ok = false;
                result.exitCode = kExitUsage;
                return result;
            }
            continue;
        }
        if (a == "--json-diagnostics") { out.jsonDiagnostics = true; continue; }
        if (a == "-q" || a == "--quiet") { out.quiet = true; continue; }
        if (a == "-v" || a == "--verbose") { out.verbose = true; continue; }
        // Both spellings, because a British-spelled tool used from an American
        // keyboard should still work.
        if (a == "--no-colour" || a == "--no-color") { out.colour = false; continue; }

        std::string value;
        std::string err;

        // ---- per-command options -------------------------------------------
        switch (out.command) {
            case Command::Compile:
                if (takeValue(c, "--output", "-o", value, err)) { out.output = value; continue; }
                if (!err.empty()) break;
                if (takeValue(c, "--include", "-I", value, err)) {
                    out.includeDirs.push_back(value);
                    continue;
                }
                if (!err.empty()) break;
                if (takeValue(c, "--revision", "", value, err)) { out.revision = value; continue; }
                if (!err.empty()) break;
                if (a == "--emit-ast") { out.emitAst = true; continue; }
                break;

            case Command::Link:
            case Command::Check:
                if (takeValue(c, "--rules", "", value, err)) {
                    out.ruleFiles.push_back(value);
                    continue;
                }
                if (!err.empty()) break;
                if (takeValue(c, "--top", "-t", value, err)) { out.top = value; continue; }
                if (!err.empty()) break;
                if (takeValue(c, "--output", "-o", value, err)) { out.output = value; continue; }
                if (!err.empty()) break;
                if (takeValue(c, "--library", "-L", value, err)) {
                    out.libraryDirs.push_back(value);
                    continue;
                }
                if (!err.empty()) break;
                if (takeValue(c, "--bom", "", value, err)) { out.bomPath = value; continue; }
                if (!err.empty()) break;
                if (takeValue(c, "--map", "", value, err)) { out.mapPath = value; continue; }
                if (!err.empty()) break;
                if (a == "--no-erc") { out.noErc = true; continue; }
                if (a == "--no-emit") { out.noEmit = true; continue; }
                break;

            case Command::Annotate:
                if (takeValue(c, "--netlist", "-n", value, err)) {
                    out.netlistPath = value;
                    continue;
                }
                if (!err.empty()) break;
                if (takeValue(c, "--reopen", "", value, err)) {
                    out.reopenPrefixes.push_back(value);
                    continue;
                }
                if (!err.empty()) break;
                if (takeValue(c, "--start", "", value, err)) {
                    // "--start <prefix>=<n>"
                    std::size_t eq = value.find('=');
                    if (eq == std::string::npos) {
                        result.ok = false;
                        result.exitCode = kExitUsage;
                        result.error = "--start takes <prefix>=<n>";
                        return result;
                    }
                    std::string prefix = value.substr(0, eq);
                    std::string_view digits(value);
                    digits.remove_prefix(eq + 1);
                    std::int64_t n = 0;
                    auto [ptr, ec] =
                        std::from_chars(digits.data(), digits.data() + digits.size(), n);
                    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
                        result.ok = false;
                        result.exitCode = kExitUsage;
                        result.error = std::format("--start: '{}' is not a number", digits);
                        return result;
                    }
                    out.startAt.emplace_back(std::move(prefix), n);
                    continue;
                }
                if (!err.empty()) break;
                if (a == "--swaps") { out.applySwaps = true; continue; }
                if (a == "--dry-run" || a == "-n") { out.dryRun = true; continue; }
                break;

            case Command::Format:
                if (a == "--check") { out.checkOnly = true; continue; }
                if (a == "--stdout") { out.toStdout = true; continue; }
                if (a == "--diff") { out.showDiff = true; continue; }
                break;

            case Command::Export:
                if (takeValue(c, "--format", "-f", value, err)) { out.format = value; continue; }
                if (!err.empty()) break;
                if (takeValue(c, "--output", "-o", value, err)) { out.output = value; continue; }
                if (!err.empty()) break;
                if (takeValue(c, "--constraints", "", value, err)) {
                    out.constraintsPath = value;
                    continue;
                }
                if (!err.empty()) break;
                if (takeValue(c, "--flat-format", "", value, err)) {
                    out.flatFormat = value;
                    continue;
                }
                if (!err.empty()) break;
                break;

            case Command::None:
                break;
        }

        result.ok = false;
        result.exitCode = kExitUsage;
        result.error = err.empty()
                           ? std::format("unknown option '{}' for 'manta {}'", a,
                                         commandName(out.command))
                           : err;
        return result;
    }

    // ---- required arguments ------------------------------------------------
    if ((out.command == Command::Link || out.command == Command::Check) && out.top.empty()) {
        result.ok = false;
        result.exitCode = kExitUsage;
        result.error = "'--top <block>' is required";
        return result;
    }
    if (out.command == Command::Annotate && out.netlistPath.empty()) {
        result.ok = false;
        result.exitCode = kExitUsage;
        result.error = "'--netlist <file>' is required";
        return result;
    }
    if (out.command == Command::Export && out.format.empty()) {
        result.ok = false;
        result.exitCode = kExitUsage;
        result.error = "'--format <target>' is required";
        return result;
    }
    // 'link' and 'check' may take their objects entirely from -L: spec 20.8
    // builds with "manta link --top power-and-signal -L build/ ..." and no
    // positional arguments at all.
    bool inputsOptional = (out.command == Command::Link || out.command == Command::Check) &&
                          !out.libraryDirs.empty();
    if (out.inputs.empty() && !inputsOptional) {
        result.ok = false;
        result.exitCode = kExitUsage;
        result.error = std::format("'manta {}' needs at least one input file",
                                   commandName(out.command));
        return result;
    }

    // "manta check" is defined as "manta link --no-emit" (spec 15.5).
    if (out.command == Command::Check) out.noEmit = true;

    return result;
}

std::string versionText() {
    return std::format("manta {} (language revision {})\n", MANTA_VERSION, kLanguageVersion);
}

namespace {

constexpr std::string_view kCommonOptions = R"(
Common options:
  -W<name>              Enable warning <name> (a code such as W-02, or its mnemonic).
  -Wno-<name>           Disable warning <name>.
  -Werror               Treat every warning as an error.
  --error=<code>        Promote one diagnostic to an error.
  --warn=<code>         Demote one diagnostic to a warning.
  --json-diagnostics    Emit diagnostics as JSON on stderr.
  -q, --quiet           Suppress non-diagnostic output.
  -v, --verbose         Report each stage.
  --no-colour           Disable ANSI colour.
  -h, --help            Show this help.
  --version             Show the version.
)";

}  // namespace

std::string helpText(Command command) {
    std::string out;
    switch (command) {
        case Command::None:
            out = R"(manta -- the Manta Schematic Definition Language compiler.

Usage: manta <command> [options] <inputs>...

Commands:
  compile    Compile .manta sources to .mantaO objects.
  link       Link objects into a .mantaNets netlist, running ERC.
  check      Link without emitting; equivalent to 'link --no-emit'.
  annotate   Assign designators in source, from a netlist.
  fmt        Format sources in place.
  export     Convert a netlist to a layout tool's format.

Run 'manta <command> --help' for the options of one command.
)";
            break;

        case Command::Compile:
            out = R"(Usage: manta compile [options] <source.manta>...

  -o, --output <path>   Object output. A file for one source, a directory for
                        several. Default: alongside each source.
  -I, --include <dir>   Directory searched for sources named on the command
                        line. Repeatable.
  --revision <rev>      Language revision to check @VERSION against.
                        Default: the implementation's own.
  --emit-ast            Also write the parse tree as JSON, for tooling.
)";
            break;

        case Command::Link:
            out = R"(Usage: manta link [options] --top <block> <object.mantaO>...

  -t, --top <block>     Name of the top-level block. Required.
  -o, --output <file>   Netlist path. Default: <top>.mantaNets
  -L, --library <dir>   Directory of objects to resolve against. Repeatable.
  --bom <file>          Also emit a BOM as CSV.
  --no-erc              Skip ERC and emit regardless.
  --no-emit             Run every stage including ERC, emit nothing.
  --map <file>          Write the elaboration map: instance path to designator.
  --rules <file>        A .mantaRules file of user-defined checks. Repeatable.
                        Every rule runs here, including 'part' rules: a check
                        has the whole design to look at, and there is exactly
                        one place to look for one.
)";
            break;

        case Command::Check:
            out = R"(Usage: manta check [options] --top <block> <object.mantaO>...

Equivalent to 'manta link --no-emit'. Accepts the same options.
)";
            break;

        case Command::Annotate:
            out = R"(Usage: manta annotate [options] <source.manta>...

  -n, --netlist <file>  .mantaNets to take assignments from. Required.
  --reopen <prefix>     Revert every designator with this prefix to '?'.
                        Repeatable.
  --start <prefix>=<n>  Begin assignment for a prefix at n. Repeatable.
  --swaps               Apply swap reconciliation as well as designators.
  --dry-run             Report the changes without writing.
)";
            break;

        case Command::Format:
            out = R"(Usage: manta fmt [options] <source.manta>...

  --check               Exit non-zero if any file would change. Write nothing.
  --stdout              Write to standard output instead of in place.
  --diff                Print a unified diff of the changes.
)";
            break;

        case Command::Export:
            out = R"(Usage: manta export [options] --format <target> <netlist.mantaNets>

  -f, --format <target> kicad, altium, orcad or allegro. Required.
  -o, --output <file>   Output path. Default: derived from the input name.
  --constraints <file>  Write directives to a separate constraint file where
                        the target cannot carry them.
  --flat-format <tmpl>  Override @FLATFORMAT for hierarchical designators.
)";
            break;
    }

    if (command != Command::None) out += kCommonOptions;
    return out;
}

}  // namespace manta
