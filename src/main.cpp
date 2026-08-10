// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// manta -- the Manta Schematic Definition Language compiler.
//
// Exit codes are fixed by spec 15.7: 0 success, 1 errors, 2 usage, 3 internal.
#include <exception>

#include "cli/console.h"
#include "cli/driver.h"
#include "cli/options.h"

int main(int argc, char** argv) {
    using namespace manta;

    setStdoutBinary();

    std::vector<std::string> args = commandLineUtf8(argc, argv);

    Options opts;
    ParseOutcome parsed = parseOptions(args, opts);

    if (opts.colour) enableAnsiEscapes();

    if (!parsed.ok) {
        writeStderr("manta: " + parsed.error + "\n");
        writeStderr("Run 'manta --help' for usage.\n");
        return parsed.exitCode;
    }
    if (opts.showVersion) {
        writeStdout(versionText());
        return kExitSuccess;
    }
    if (opts.showHelp) {
        writeStdout(helpText(opts.command));
        return kExitSuccess;
    }

    // An internal fault is exit 3 and never a crash: the caller of a build tool
    // needs to tell "your design is wrong" from "the compiler is wrong".
    try {
        switch (opts.command) {
            case Command::Compile: return runCompile(opts);
            case Command::Link:
            case Command::Check: return runLink(opts);
            case Command::Annotate: return runAnnotate(opts);
            case Command::Format: return runFormat(opts);
            case Command::Export: return runExport(opts);
            case Command::Render: return runRender(opts);
            case Command::None: break;
        }
        writeStdout(helpText(Command::None));
        return kExitSuccess;
    } catch (const std::exception& e) {
        writeStderr(std::string("manta: internal error: ") + e.what() + "\n");
        return kExitInternal;
    } catch (...) {
        writeStderr("manta: internal error\n");
        return kExitInternal;
    }
}
