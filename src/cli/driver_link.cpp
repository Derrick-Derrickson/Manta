// 'manta link' and 'manta check' (spec 15.3, 15.5).
//
// The linker is given a set of objects and the name of the top-level block. It
// resolves every referenced name, elaborates the top block, resolves globals,
// builds the netlist, runs ERC and emits .mantaNets.
#include <filesystem>
#include <format>

#include "cli/console.h"
#include "cli/driver.h"
#include "cli/rules_loader.h"
#include "erc/erc.h"
#include "json/json.h"
#include "link/elaborate.h"
#include "link/mating.h"
#include "obj/mantao.h"

namespace manta {

namespace {

// Collects the objects named on the command line plus every .mantaO in each -L
// directory. Library directories are scanned in sorted order so that the set of
// declarations, and therefore every diagnostic about them, is identical between
// runs and between filesystems (spec 15.8).
std::vector<std::string> gatherObjectPaths(const Options& opts, DiagEngine& diags) {
    std::vector<std::string> paths = opts.inputs;

    for (const std::string& dir : opts.libraryDirs) {
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        if (ec) {
            diags.report(DiagId::Io, Span{}, std::format("{}: {}", dir, ec.message()));
            continue;
        }
        std::vector<std::string> found;
        for (const auto& entry : it) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".mantaO") continue;
            found.push_back(entry.path().string());
        }
        std::sort(found.begin(), found.end());
        for (auto& f : found) paths.push_back(std::move(f));
    }

    // A path named twice -- once explicitly, once via -L -- must not produce
    // E-30 against itself.
    std::vector<std::string> unique;
    for (const std::string& p : paths) {
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(p, ec);
        std::string key = ec ? p : canonical.string();
        bool seen = false;
        for (const std::string& u : unique) {
            std::error_code ec2;
            auto uc = std::filesystem::weakly_canonical(u, ec2);
            if ((ec2 ? u : uc.string()) == key) {
                seen = true;
                break;
            }
        }
        if (!seen) unique.push_back(p);
    }
    return unique;
}

}  // namespace

int runLink(const Options& opts) {
    SourceManager sources;
    DiagEngine diags(sources, opts.severity);
    Arena arena;
    StringInterner interner;

    std::vector<std::string> paths = gatherObjectPaths(opts, diags);
    if (diags.hasErrors()) return finish(diags, opts, kExitUsage);

    std::vector<LinkedObject> objects;
    objects.reserve(paths.size());

    for (const std::string& path : paths) {
        std::string text;
        std::string error;
        if (!readFileBinary(path, text, error)) {
            diags.report(DiagId::Io, Span{}, std::format("{}: {}", path, error));
            continue;
        }

        JsonParseError jsonError;
        JsonPtr root = jsonParse(text, jsonError);
        if (!root) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}: {} at byte {}", path, jsonError.message,
                                     jsonError.offset));
            continue;
        }

        // Spans in an object index the *source* file it came from, so link-time
        // diagnostics can quote the original line (spec 15.6). Load that source
        // where it can still be found; otherwise register a stand-in carrying
        // the right path so at least file, line and column are reported.
        std::string sourcePath(root->str("source"));
        const SourceFile* file = nullptr;
        if (!sourcePath.empty()) {
            if (auto loaded = sources.load(sourcePath)) file = *loaded;
        }
        if (!file) file = sources.addVirtual(sourcePath.empty() ? path : sourcePath, "");

        ObjectFile object;
        if (!readObject(*root, arena, interner, file->id(), diags, Span{file->id(), 0, 0},
                        object)) {
            continue;
        }

        LinkedObject linked;
        linked.unit = object.unit;
        linked.path = path;
        linked.sourcePath = object.sourcePath;
        linked.index = static_cast<std::uint32_t>(objects.size());
        objects.push_back(std::move(linked));
    }

    if (diags.hasErrors()) return finish(diags, opts, kExitError);

    if (opts.verbose && !opts.quiet) {
        writeStderr(std::format("linking {} object(s), top '{}'\n", objects.size(), opts.top));
    }

    // 1. Resolve every referenced name against every supplied object.
    SymbolTable symbols(interner, diags);
    for (const LinkedObject& object : objects) symbols.addObject(object);

    Revision toolchain = Revision::toolchain();
    if (!opts.revision.empty() && !Revision::parse(opts.revision, toolchain)) {
        diags.report(DiagId::Usage, Span{},
                     std::format("--revision: '{}' is not a revision", opts.revision));
        return finish(diags, opts, kExitUsage);
    }
    symbols.checkVersions(toolchain);

    // 2-4. Elaborate, resolve globals, build the netlist.
    ElaborateOptions elabOptions;
    elabOptions.toolchain = toolchain;
    elabOptions.runErc = !opts.noErc;

    Elaborator elaborator(symbols, interner, diags, objects, elabOptions);
    Design design = elaborator.run(interner.intern(opts.top), Span{});

    // 5. ERC. Spec 15.3 keeps it here because no-driver, no-source,
    // multiple-driver and unpowered-net are whole-design properties.
    if (!opts.noErc) {
        ErcChecker erc(design, interner, diags);
        erc.run();
    }

    // 5a. Connectors and cables (spec 12A). A cable named by '@mate' is
    // compiled on its own -- it is its own deliverable -- and the two are then
    // laid against each other. The board's netlist is not touched.
    struct MateContext {
        SymbolTable& symbols;
        StringInterner& interner;
        DiagEngine& diags;
        const std::vector<LinkedObject>& objects;
        const ElaborateOptions& options;
    } mateContext{symbols, interner, diags, objects, elabOptions};

    MateChecker mates(design, symbols, interner, diags);
    mates.checkCableContents();
    mates.run(
        [](void* ctx, SymbolId name, Span at) {
            auto& mc = *static_cast<MateContext*>(ctx);
            Elaborator sub(mc.symbols, mc.interner, mc.diags, mc.objects, mc.options);
            return sub.run(name, at);
        },
        &mateContext);

    // 5b. User rules. These are a project's own checks over the same design,
    // and they run whether or not --no-erc was given: the built-in rules and a
    // project's rules answer different questions.
    if (!opts.ruleFiles.empty()) {
        SeverityPolicy policy = opts.severity;
        LoadedRuleFiles rules =
            loadRuleFiles(opts, sources, arena, interner, diags, policy);
        if (!rules.ok) return finish(diags, opts, kExitUsage);
        diags.setPolicy(std::move(policy));

        RuleEvaluator evaluator(rules.rules, interner, diags);
        evaluator.runOnDesign(design);

        // Part checks run here too, not at compile. Everything a rule can look
        // at lives in one place, there is one interner, and 'manta check' is
        // complete on its own.
        for (const auto& [key, decl] : symbols.all()) {
            if (decl.item->kind != ItemKind::Part) continue;
            PartInfo info = buildPartInfo(decl.item, decl.objectIndex, interner, diags);
            evaluator.runOnPart(info, interner.text(decl.item->name.symbol));
        }

        if (opts.verbose && !opts.quiet) {
            writeStderr(std::format("  {} user rule(s) applied\n", rules.rules.checks.size()));
        }
    }

    // Un-annotated instances. This is a toolchain policy rather than one of the
    // section 16 rules, so it lives here and not in the ERC pass, and it runs
    // even under --no-erc.
    // A block instance is reported too. Its label is not a designator, but it
    // names a level of the hierarchy and so lands in the path of every component
    // beneath it -- and from there in the netlist, the BOM and whatever a layout
    // tool calls the part. 'BLK?7_R1' is no more shippable than a bare '?'.
    for (const UnannotatedBlock& b : design.unannotatedBlocks) {
        diags.report(DiagId::Unannotated, b.span, b.identity);
    }
    for (const Component& c : design.components) {
        if (c.designator.empty()) diags.report(DiagId::Unannotated, c.span, c.identity);
    }

    if (diags.hasErrors()) return finish(diags, opts, kExitError);

    // 6. Emit.
    if (opts.noEmit) return finish(diags, opts);

    std::string netlistPath = opts.output.empty() ? opts.top + ".mantaNets" : opts.output;
    std::string netlist;
    writeNetlist(design, netlist);

    std::string error;
    if (!writeFileBinary(netlistPath, netlist, error)) {
        diags.report(DiagId::Io, Span{}, std::format("{}: {}", netlistPath, error));
        return finish(diags, opts, kExitError);
    }
    if (opts.verbose && !opts.quiet) {
        writeStderr(std::format("  {} component(s), {} net(s) -> {}\n", design.components.size(),
                                design.nets.size(), netlistPath));
    }

    if (!opts.bomPath.empty()) {
        std::string bom;
        writeBom(design, bom);
        if (!writeFileBinary(opts.bomPath, bom, error)) {
            diags.report(DiagId::Io, Span{}, std::format("{}: {}", opts.bomPath, error));
        }
    }

    // Every mated cable, written beside the board and never merged into it: a
    // loom is a separate thing to build, with its own bill of materials, and a
    // KiCad netlist has nowhere to put a wire anyway.
    if (opts.assembly) {
        for (const MatedCable& m : mates.mated()) {
            std::string cableNetlist;
            writeNetlist(m.design, cableNetlist);
            std::string path = m.cableName + ".mantaNets";
            if (!writeFileBinary(path, cableNetlist, error)) {
                diags.report(DiagId::Io, Span{}, std::format("{}: {}", path, error));
                continue;
            }
            if (!opts.bomPath.empty()) {
                std::string cableBom;
                writeBom(m.design, cableBom);
                std::string bomPath = m.cableName + ".bom.csv";
                if (!writeFileBinary(bomPath, cableBom, error)) {
                    diags.report(DiagId::Io, Span{}, std::format("{}: {}", bomPath, error));
                }
            }
            if (opts.verbose && !opts.quiet) {
                writeStderr(std::format("  cable {} -> {}\n", m.cableName, path));
            }
        }
    }

    if (!opts.mapPath.empty()) {
        std::string map;
        writeElaborationMap(design, map);
        if (!writeFileBinary(opts.mapPath, map, error)) {
            diags.report(DiagId::Io, Span{}, std::format("{}: {}", opts.mapPath, error));
        }
    }

    return finish(diags, opts);
}

}  // namespace manta
