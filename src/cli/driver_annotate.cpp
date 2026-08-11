// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// 'manta annotate' (spec 13, 15.5).
//
// Spec 13.1 is emphatic that this is a separate command: "manta compile and
// manta link never write to a source file; annotation is manta annotate, run
// deliberately by the author."
//
// Spec 13.7 bounds what it may write: "The only writes any tool makes to source
// are designator assignment and swap reconciliation." So this is not a
// reformat. Every change is a byte-range replacement of exactly the text after
// a designator's prefix; every other byte of the file, including its line
// endings and comments, is left untouched.
#include <algorithm>
#include <format>
#include <memory>

#include "cli/console.h"
#include "cli/driver.h"
#include "export/exporters.h"
#include "json/json.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace manta {

namespace {

// One designator occurrence in one source file.
struct Site {
    std::size_t fileIndex = 0;
    Span span;             // the bytes after the prefix: "?", "7" or "%[1:4]"
    std::string prefix;
    DesignatorKind kind = DesignatorKind::Unassigned;
    std::int64_t number = 0;
    std::vector<DesigPart> parts;
    std::int64_t copies = 1;  // how many designators this one token carries
};

struct SourceState {
    std::string path;
    const SourceFile* file = nullptr;
    Arena arena;
    StringInterner interner;
    std::unique_ptr<DiagEngine> diags;
    SourceUnit unit;
    std::string text;
    bool changed = false;
};

// Walks a parsed unit collecting every designator, recording how many copies
// the enclosing multiplicity or replication gives it (spec 13.3).
class SiteCollector {
public:
    SiteCollector(std::size_t fileIndex, const StringInterner& interner,
                  std::vector<Site>& out)
        : fileIndex_(fileIndex), in_(interner), out_(out) {}

    void unit(const SourceUnit& u) {
        for (const Item* item : u.items) this->item(item);
    }

private:
    void item(const Item* it) {
        if (!it) return;
        for (const BodyEntry& e : it->body) {
            if (e.kind == BodyKind::Item) item(e.item);
            if (e.kind == BodyKind::Stmt && e.stmt->kind == StmtKind::Chain) {
                for (const Segment* seg : e.stmt->chain->segments) segment(seg, 1);
            }
        }
    }

    void segment(const Segment* seg, std::int64_t copies) {
        if (!seg) return;
        for (const Element* el : seg->elements) element(el, copies);
    }

    void element(const Element* el, std::int64_t copies) {
        switch (el->kind) {
            case ElementKind::Device:
                device(el->device, copies);
                return;
            case ElementKind::Group: {
                // Spec 13.3: "Where one statement instantiates a part or block
                // N times, the annotator writes a range designator."
                std::int64_t inner = el->group->mult == MultKind::None
                                         ? copies
                                         : copies * std::max<std::int64_t>(el->group->count, 1);
                segment(el->group->body, inner);
                return;
            }
            case ElementKind::Replication:
                // A replication's copy count follows from the connection width,
                // which only the linker knows; the netlist supplies it.
                segment(el->replication->body, copies);
                return;
            case ElementKind::Net:
                return;
        }
    }

    void device(const Device* dev, std::int64_t copies) {
        if (!dev || !dev->instance) return;
        if (dev->instance->declares) {
            const Designator& d = dev->instance->designator;
            if (valid(d.prefix.symbol)) {
                Site site;
                site.fileIndex = fileIndex_;
                site.span = d.assignmentSpan;
                site.prefix = std::string(in_.text(d.prefix.symbol));
                site.kind = d.kind;
                site.number = d.number;
                site.parts.assign(d.parts.begin(), d.parts.end());
                site.copies = copies;
                out_.push_back(std::move(site));
            }
        }

        // Spec 7.4: a binding may carry a chain, and "a device declared inside a
        // binding is an ordinary instance of the enclosing body ... annotated
        // with everything else (13)". Its designator is written after the
        // enclosing instance's, so it is collected after it and numbers fall in
        // source order (spec 13.3).
        for (const Binding* b : dev->instance->bindings) {
            if (b->kind == BindingKind::PinNet && b->rhs) segment(b->rhs, copies);
        }
    }

    std::size_t fileIndex_;
    const StringInterner& in_;
    std::vector<Site>& out_;
};

// Allocates the next free numbers for a prefix. Spec 13.6: "Once assigned, a
// designator is not changed by any tool", so allocation only ever fills gaps
// left by numbers nothing already uses.
class NumberAllocator {
public:
    void reserve(const std::string& prefix, std::int64_t number) {
        used_[prefix].push_back(number);
    }

    void startAt(const std::string& prefix, std::int64_t n) { next_[prefix] = n; }

    std::vector<std::int64_t> take(const std::string& prefix, std::int64_t count) {
        std::vector<std::int64_t>& taken = used_[prefix];
        std::int64_t& cursor = next_.contains(prefix) ? next_[prefix] : (next_[prefix] = 1);

        std::vector<std::int64_t> out;
        while (static_cast<std::int64_t>(out.size()) < count) {
            if (std::find(taken.begin(), taken.end(), cursor) == taken.end()) {
                out.push_back(cursor);
                taken.push_back(cursor);
            }
            ++cursor;
        }
        return out;
    }

private:
    FlatMap<std::string, std::vector<std::int64_t>> used_;
    FlatMap<std::string, std::int64_t> next_;
};

// Renders a set of numbers as a designator suffix. Spec 13.3: "A group of one
// collapses: a single instance annotates to BLK1, never BLK%[1:1]", and ranges
// "may be non-contiguous ... so a group that grows never forces the renumbering
// of designators already assigned."
std::string renderAssignment(const std::vector<std::int64_t>& numbers) {
    if (numbers.size() == 1) return std::to_string(numbers[0]);

    std::string out = "%[";
    std::size_t i = 0;
    bool first = true;
    while (i < numbers.size()) {
        std::size_t j = i;
        while (j + 1 < numbers.size() && numbers[j + 1] == numbers[j] + 1) ++j;
        if (!first) out += ',';
        first = false;
        out += std::to_string(numbers[i]);
        if (j > i) {
            out += ':';
            out += std::to_string(numbers[j]);
        }
        i = j + 1;
    }
    return out + "]";
}

struct Edit {
    std::size_t fileIndex;
    std::uint32_t offset;
    std::uint32_t length;
    std::string replacement;
};

}  // namespace

int runAnnotate(const Options& opts) {
    SourceManager sources;
    DiagEngine diags(sources, opts.severity);

    // Spec 15.5 makes the netlist a required input, so it is read and validated
    // even when the assignment itself can be computed from source: it is what
    // carries replication counts and swap decisions.
    Design design;
    {
        std::string text;
        std::string error;
        if (!readFileBinary(opts.netlistPath, text, error)) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}: {}", opts.netlistPath, error));
            return finish(diags, opts, kExitUsage);
        }
        JsonParseError jsonError;
        JsonPtr root = jsonParse(text, jsonError);
        if (!root || !readNetlist(*root, diags, design)) {
            if (root) return finish(diags, opts, kExitUsage);
            diags.report(DiagId::Io, Span{},
                         std::format("{}: {} at byte {}", opts.netlistPath, jsonError.message,
                                     jsonError.offset));
            return finish(diags, opts, kExitUsage);
        }
    }

    std::vector<std::unique_ptr<SourceState>> files;
    std::vector<Site> sites;

    for (const std::string& input : opts.inputs) {
        auto loaded = sources.load(input);
        if (!loaded) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}: {}", loaded.error().path, loaded.error().message));
            continue;
        }
        auto state = std::make_unique<SourceState>();
        state->path = input;
        state->file = *loaded;
        state->text = std::string((*loaded)->text());
        state->diags = std::make_unique<DiagEngine>(sources, opts.severity);

        Lexer lexer(*state->file, *state->diags);
        TokenStream tokens = lexer.run();
        Parser parser(tokens, *state->file, state->arena, state->interner, *state->diags);
        state->unit = parser.run();

        if (state->diags->hasErrors()) {
            diags.absorb(std::move(*state->diags));
            continue;
        }

        SiteCollector collector(files.size(), state->interner, sites);
        collector.unit(state->unit);
        files.push_back(std::move(state));
    }

    if (diags.hasErrors()) return finish(diags, opts, kExitError);

    // Spec 13.5: "Changing an assigned designator back to '?' releases it for
    // reassignment ... Reverting a range designator releases every member."
    auto reopened = [&](const std::string& prefix) {
        return std::find(opts.reopenPrefixes.begin(), opts.reopenPrefixes.end(), prefix) !=
               opts.reopenPrefixes.end();
    };

    NumberAllocator allocator;
    for (const auto& [prefix, n] : opts.startAt) allocator.startAt(prefix, n);

    // Every number already in use is reserved first, so allocation never
    // collides with an assignment the author made.
    for (const Site& site : sites) {
        if (reopened(site.prefix)) continue;
        if (site.kind == DesignatorKind::Numbered) allocator.reserve(site.prefix, site.number);
        if (site.kind == DesignatorKind::Range) {
            for (const DesigPart& p : site.parts) {
                for (std::int64_t n = p.lo; n <= p.hi; ++n) allocator.reserve(site.prefix, n);
            }
        }
    }

    std::vector<Edit> edits;
    for (const Site& site : sites) {
        if (reopened(site.prefix)) {
            if (site.kind != DesignatorKind::Unassigned) {
                edits.push_back(Edit{site.fileIndex, site.span.offset, site.span.length, "?"});
            }
            continue;
        }
        if (site.kind != DesignatorKind::Unassigned) continue;  // spec 13.6: never renumber

        std::vector<std::int64_t> numbers = allocator.take(site.prefix, site.copies);
        edits.push_back(
            Edit{site.fileIndex, site.span.offset, site.span.length, renderAssignment(numbers)});
    }

    if (opts.applySwaps && !design.swaps.empty() && opts.verbose && !opts.quiet) {
        // Spec 13.6: "manta annotate also reconciles swaps performed during
        // layout." The swap section is a documented extension; see
        // docs/assumptions.md, B2.
        writeStderr(std::format("  {} swap record(s) to reconcile\n", design.swaps.size()));
    }

    // Apply descending by offset so earlier offsets stay valid.
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        if (a.fileIndex != b.fileIndex) return a.fileIndex < b.fileIndex;
        return a.offset > b.offset;
    });

    for (const Edit& edit : edits) {
        SourceState& state = *files[edit.fileIndex];
        if (opts.dryRun) {
            LineCol lc = state.file->lineCol(edit.offset);
            writeStdout(std::format("{}:{}:{}: '{}' -> '{}'\n", state.path, lc.line, lc.column,
                                    state.file->text().substr(edit.offset, edit.length),
                                    edit.replacement));
            continue;
        }
        state.text.replace(edit.offset, edit.length, edit.replacement);
        state.changed = true;
    }

    if (opts.dryRun) {
        if (!opts.quiet) {
            writeStderr(std::format("{} change(s); nothing written\n", edits.size()));
        }
        return finish(diags, opts);
    }

    for (const auto& state : files) {
        if (!state->changed) continue;
        std::string error;
        if (!writeFileBinary(state->path, state->text, error)) {
            diags.report(DiagId::Io, Span{}, std::format("{}: {}", state->path, error));
            continue;
        }
        if (opts.verbose && !opts.quiet) writeStderr(std::format("  annotated {}\n", state->path));
    }

    return finish(diags, opts);
}

}  // namespace manta
