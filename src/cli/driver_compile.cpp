// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// 'manta compile' (spec 15.2, 15.5).
//
// Per source file: lex and parse, validate locally, emit a .mantaO object with
// external names unresolved and substitutions unevaluated.
//
// Files are parsed in parallel because each is fully independent -- separate
// arena, separate interner, separate diagnostic buffer. The buffers are merged
// back in command-line order, so the number of threads can never change stderr
// (spec 15.8).
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <memory>
#include <thread>
#include <vector>

#include "cli/console.h"
#include "cli/driver.h"
#include "lex/lexer.h"
#include "obj/mantao.h"
#include "parse/parser.h"
#include "sema/local_check.h"

namespace manta {

std::string withExtension(std::string_view path, std::string_view newExtension) {
    std::size_t slash = path.find_last_of("/\\");
    std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return std::string(path) + std::string(newExtension);
    }
    return std::string(path.substr(0, dot)) + std::string(newExtension);
}

std::string baseName(std::string_view path) {
    std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

bool isDirectory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::string resolveInput(const std::string& name, const std::vector<std::string>& searchDirs) {
    std::error_code ec;
    if (std::filesystem::exists(name, ec)) return name;
    for (const std::string& dir : searchDirs) {
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }
    return name;
}

int finish(const DiagEngine& diags, const Options& opts, int successCode) {
    std::string rendered;
    RenderOptions render;
    render.json = opts.jsonDiagnostics;
    render.colour = opts.colour && !opts.jsonDiagnostics;
    render.showSource = !opts.jsonDiagnostics;
    renderDiagnostics(diags, render, rendered);
    if (!rendered.empty()) writeStderr(rendered);

    if (diags.hasErrors()) return kExitError;

    if (!opts.quiet && opts.verbose && diags.warningCount() > 0) {
        writeStderr(std::format("{} warning(s)\n", diags.warningCount()));
    }
    return successCode;
}

namespace {

// One source file's worth of compilation state. Each is independent, which is
// what makes the parse phase trivially parallel.
struct CompileUnit {
    std::string inputPath;
    const SourceFile* file = nullptr;
    Arena arena;
    StringInterner interner;
    std::unique_ptr<DiagEngine> diags;
    SourceUnit ast;
    std::string objectText;
    bool parsed = false;
};

void compileOne(CompileUnit& u) {
    Lexer lexer(*u.file, *u.diags);
    TokenStream tokens = lexer.run();

    Parser parser(tokens, *u.file, u.arena, u.interner, *u.diags);
    u.ast = parser.run();

    LocalChecker checker(*u.file, u.interner, *u.diags);
    checker.run(u.ast);

    u.parsed = true;
    if (!u.diags->hasErrors()) {
        writeObject(u.ast, u.interner, u.inputPath, u.objectText);
    }
}

// Runs `work` over [0, n) across the hardware threads, in a deterministic
// partition. Falls back to serial execution for small inputs, where the thread
// setup would cost more than it saves.
template <typename F>
void parallelFor(std::size_t n, F&& work) {
    unsigned hw = std::thread::hardware_concurrency();
    std::size_t workers = std::min<std::size_t>(n, hw == 0 ? 1 : hw);
    if (workers <= 1 || n < 4) {
        for (std::size_t i = 0; i < n; ++i) work(i);
        return;
    }

    std::atomic<std::size_t> next{0};
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t t = 0; t < workers; ++t) {
        threads.emplace_back([&] {
            for (;;) {
                std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) return;
                work(i);
            }
        });
    }
}

}  // namespace

int runCompile(const Options& opts) {
    SourceManager sources;
    DiagEngine top(sources, opts.severity);

    // Loading is serial: it mutates the SourceManager, and it is IO bound.
    std::vector<std::unique_ptr<CompileUnit>> units;
    units.reserve(opts.inputs.size());

    for (const std::string& input : opts.inputs) {
        std::string path = resolveInput(input, opts.includeDirs);
        auto loaded = sources.load(path);
        if (!loaded) {
            top.report(DiagId::Io, Span{},
                       std::format("{}: {}", loaded.error().path, loaded.error().message));
            continue;
        }
        auto u = std::make_unique<CompileUnit>();
        u->inputPath = path;
        u->file = *loaded;
        u->diags = std::make_unique<DiagEngine>(sources, opts.severity);
        units.push_back(std::move(u));
    }

    if (top.hasErrors()) return finish(top, opts, kExitUsage);

    if (opts.verbose && !opts.quiet) {
        writeStderr(std::format("compiling {} file(s)\n", units.size()));
    }

    parallelFor(units.size(), [&](std::size_t i) { compileOne(*units[i]); });

    // Merge in command-line order, not completion order.
    for (auto& u : units) top.absorb(std::move(*u->diags));

    if (top.hasErrors()) return finish(top, opts);

    // ---- decide output paths --------------------------------------------
    // Spec 15.5: "-o <path>: Object output. A file for one source, a directory
    // for several. Default: alongside each source."
    bool outputIsDirectory = false;
    if (!opts.output.empty()) {
        outputIsDirectory = units.size() > 1 || isDirectory(opts.output) ||
                            opts.output.back() == '/' || opts.output.back() == '\\';
        if (outputIsDirectory) {
            std::string error;
            if (!ensureDirectory(opts.output, error)) {
                top.report(DiagId::Io, Span{},
                           std::format("{}: {}", opts.output, error));
                return finish(top, opts, kExitUsage);
            }
        }
    }

    for (auto& u : units) {
        std::string target;
        if (opts.output.empty()) {
            target = withExtension(u->inputPath, ".mantaO");
        } else if (outputIsDirectory) {
            target = (std::filesystem::path(opts.output) /
                      withExtension(baseName(u->inputPath), ".mantaO"))
                         .string();
        } else {
            target = opts.output;
        }

        std::string error;
        if (!writeFileBinary(target, u->objectText, error)) {
            top.report(DiagId::Io, Span{}, std::format("{}: {}", target, error));
            continue;
        }
        if (opts.verbose && !opts.quiet) {
            writeStderr(std::format("  {} -> {}\n", u->inputPath, target));
        }

        if (opts.emitAst) {
            // The AST dump is the same tree in the same encoding; the object
            // *is* the parse tree (spec 15.2), so a separate serialiser would
            // only be a second thing to keep in step.
            std::string astPath = opts.output.empty() || !outputIsDirectory
                                      ? withExtension(u->inputPath, ".ast.json")
                                      : (std::filesystem::path(opts.output) /
                                         withExtension(baseName(u->inputPath), ".ast.json"))
                                            .string();
            if (!writeFileBinary(astPath, u->objectText, error)) {
                top.report(DiagId::Io, Span{}, std::format("{}: {}", astPath, error));
            }
        }
    }

    return finish(top, opts);
}

}  // namespace manta
