// Name resolution across objects (spec 4.1, 4.2, 4.3).
//
// Spec 4.1: "Names are resolved at link across every object supplied to the
// linker. A name declared twice is error E-30. A name referenced but never
// declared is error E-31."
//
// Spec 4.2 makes 'static' internal linkage: "Visible only within its own object,
// and exempt from E-30", so two libraries may each declare a static part of the
// same name without conflict.
#pragma once

#include <string>
#include <vector>

#include "ast/ast.h"
#include "base/flat_map.h"
#include "base/intern.h"
#include "diag/engine.h"

namespace manta {

// One compiled object presented to the linker.
struct LinkedObject {
    SourceUnit unit;
    std::string path;        // the .mantaO path, for diagnostics
    std::string sourcePath;  // the .manta it came from
    std::uint32_t index = 0;
};

struct Declaration {
    const Item* item = nullptr;
    std::uint32_t objectIndex = 0;
    bool isStatic = false;
};

// A key that keeps static declarations from colliding: an external name is
// global, an internal one is scoped to its object.
struct DeclKey {
    SymbolId name = SymbolId::kInvalid;
    std::uint32_t object = 0;  // meaningful only for static declarations

    friend bool operator==(const DeclKey&, const DeclKey&) = default;
};

}  // namespace manta

template <>
struct manta::DefaultHash<manta::DeclKey> {
    std::uint64_t operator()(const manta::DeclKey& k) const noexcept {
        return hashCombine(mix64(raw(k.name)), mix64(k.object));
    }
};

namespace manta {

// The language revision the toolchain implements, for @VERSION (spec 4.3).
struct Revision {
    std::uint32_t major = 1;
    std::uint32_t minor = 0;

    [[nodiscard]] std::string text() const {
        return std::to_string(major) + "." + std::to_string(minor);
    }
    friend auto operator<=>(const Revision&, const Revision&) = default;
};

class SymbolTable {
public:
    SymbolTable(StringInterner& interner, DiagEngine& diags)
        : interner_(interner), diags_(diags) {}

    // Registers every top-level and nested declaration in an object. Raises
    // E-30 on a duplicate external name.
    void addObject(const LinkedObject& object);

    // Resolves a name as seen from `fromObject`: a static declaration in that
    // object wins, then any external declaration. Returns nullptr if unknown.
    [[nodiscard]] const Declaration* find(SymbolId name, std::uint32_t fromObject) const;

    // Resolves and reports E-31 when the name is unknown.
    [[nodiscard]] const Declaration* require(SymbolId name, std::uint32_t fromObject, Span at);

    // Checks every declaration's @VERSION against the toolchain revision,
    // raising E-36 where it cannot be satisfied.
    void checkVersions(Revision toolchain);

    [[nodiscard]] std::size_t size() const noexcept { return decls_.size(); }

    // Every declaration, in the order registered, for whole-design passes.
    [[nodiscard]] std::span<const std::pair<DeclKey, Declaration>> all() const {
        return ordered_;
    }

private:
    void addDeclaration(const Item* item, std::uint32_t objectIndex);

    StringInterner& interner_;
    DiagEngine& diags_;
    FlatMap<DeclKey, Declaration> decls_;
    std::vector<std::pair<DeclKey, Declaration>> ordered_;
};

// Parses "1.2", "1.2+", "1.2-" or "0.2-1.2" into a satisfiability test.
[[nodiscard]] bool versionSatisfied(const VersionConstraint& c, Revision toolchain);

}  // namespace manta
