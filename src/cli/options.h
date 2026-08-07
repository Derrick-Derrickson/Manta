// Command-line parsing for the six subcommands of spec 15.5.
#pragma once

#include <string>
#include <vector>

#include "diag/engine.h"

namespace manta {

enum class Command { None, Compile, Link, Check, Annotate, Format, Export };

// Exit codes, fixed by spec 15.7.
enum ExitCode : int {
    kExitSuccess = 0,   // warnings may have been emitted
    kExitError = 1,     // one or more errors; no output written
    kExitUsage = 2,     // bad option, missing argument, unreadable input
    kExitInternal = 3,  // internal error
};

struct Options {
    Command command = Command::None;
    std::vector<std::string> inputs;

    // Common (spec 15.5).
    SeverityPolicy severity;
    bool jsonDiagnostics = false;
    bool quiet = false;
    bool verbose = false;
    bool colour = false;
    bool showHelp = false;
    bool showVersion = false;

    // compile
    std::string output;
    std::vector<std::string> includeDirs;
    std::string revision;
    bool emitAst = false;

    // link / check
    std::string top;
    std::vector<std::string> libraryDirs;
    std::string bomPath;
    std::string mapPath;
    bool noErc = false;
    bool noEmit = false;

    // annotate
    std::string netlistPath;
    std::vector<std::string> reopenPrefixes;
    std::vector<std::pair<std::string, std::int64_t>> startAt;
    bool applySwaps = false;
    bool dryRun = false;

    // fmt
    bool checkOnly = false;
    bool toStdout = false;
    bool showDiff = false;

    // export
    std::string format;
    std::string constraintsPath;
    std::string flatFormat;
};

struct ParseOutcome {
    bool ok = true;
    int exitCode = kExitSuccess;
    std::string error;  // set when ok is false
};

// Parses argv (already UTF-8). `args` includes argv[0].
ParseOutcome parseOptions(const std::vector<std::string>& args, Options& out);

[[nodiscard]] std::string helpText(Command command);
[[nodiscard]] std::string versionText();

}  // namespace manta
