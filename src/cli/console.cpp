// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli/console.h"

#include <cstdio>
#include <filesystem>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <fcntl.h>
#    include <io.h>
#    include <shellapi.h>
#else
#    include <unistd.h>
#endif

namespace manta {

bool stderrIsTerminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

bool stdoutIsTerminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

void enableAnsiEscapes() {
#if defined(_WIN32)
    // Windows 10 1511 and later understand ANSI escapes, but only once
    // ENABLE_VIRTUAL_TERMINAL_PROCESSING is set on the handle.
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return;
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

void setStdoutBinary() {
#if defined(_WIN32)
    // Without this, every '\n' written to stdout becomes "\r\n" and a piped
    // artifact would differ from the Linux build byte for byte (spec 15.8).
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

std::vector<std::string> commandLineUtf8(int argc, char** argv) {
    std::vector<std::string> out;
#if defined(_WIN32)
    // The narrow argv is in the active code page and mangles non-ASCII paths,
    // so the wide command line is the only faithful source.
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        out.reserve(static_cast<std::size_t>(wargc));
        for (int i = 0; i < wargc; ++i) {
            int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s;
            if (need > 1) {
                s.resize(static_cast<std::size_t>(need - 1));
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), need, nullptr, nullptr);
            }
            out.push_back(std::move(s));
        }
        LocalFree(wargv);
        return out;
    }
#endif
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

void writeStderr(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

void writeStdout(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

bool writeFileBinary(const std::string& path, std::string_view content, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        error = "cannot open for writing";
        return false;
    }
    std::size_t written = std::fwrite(content.data(), 1, content.size(), f);
    bool bad = written != content.size() || std::ferror(f) != 0;
    if (std::fclose(f) != 0) bad = true;
    if (bad) {
        error = "write failed";
        return false;
    }
    return true;
}

bool readFileBinary(const std::string& path, std::string& content, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open file";
        return false;
    }
    content.clear();
    char buf[64 * 1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        error = "read failed";
        return false;
    }
    return true;
}

bool ensureDirectory(const std::string& path, std::string& error) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return true;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

}  // namespace manta
