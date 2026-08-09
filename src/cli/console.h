// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Console and process-entry portability.
//
// Three things differ between Linux and Windows and are confined here:
//
//  * ANSI colour. Windows 10+ supports it but only after the console mode is
//    switched on explicitly.
//  * The command line. Windows argv is in the active code page, which loses
//    non-ASCII paths; the wide command line is fetched and converted to UTF-8.
//  * stdout in binary mode, so that written artifacts are byte-identical across
//    platforms (spec 15.8) rather than gaining CRLF on Windows.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace manta {

// True when the stream is an interactive terminal that can render ANSI colour.
[[nodiscard]] bool stderrIsTerminal();
[[nodiscard]] bool stdoutIsTerminal();

// Switches the console into a mode that understands ANSI escapes. A no-op where
// that is already true.
void enableAnsiEscapes();

// Puts stdout into binary mode so no newline translation occurs.
void setStdoutBinary();

// The process arguments as UTF-8, including argv[0].
[[nodiscard]] std::vector<std::string> commandLineUtf8(int argc, char** argv);

// Writes to stderr without going through iostreams.
void writeStderr(std::string_view text);
void writeStdout(std::string_view text);

// Whole-file IO, always binary.
[[nodiscard]] bool writeFileBinary(const std::string& path, std::string_view content,
                                   std::string& error);
[[nodiscard]] bool readFileBinary(const std::string& path, std::string& content,
                                  std::string& error);

// Creates a directory and any missing parents. False on failure.
[[nodiscard]] bool ensureDirectory(const std::string& path, std::string& error);

}  // namespace manta
