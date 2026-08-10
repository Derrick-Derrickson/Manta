// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The .mantaO object format (spec 15.2, 15.4).
//
// Spec 15.2: "A .mantaO object carries declarations, their interfaces, and
// their bodies in intermediate form. It is not a netlist fragment: a block
// cannot be elaborated until instantiated, since [[ ]] derives its count from
// the call site and #! overrides change what is emitted."
//
// So the object is the parse tree, with external names unresolved and
// substitutions unevaluated (spec 14.1). Serialising the AST verbatim keeps
// compilation side-effect free and makes the round trip exact -- which the test
// suite checks by writing, reading and writing again, and comparing bytes.
//
// Source spans travel with every node so that link-time diagnostics can report
// a position in the original file (spec 15.6).
#pragma once

#include <string>
#include <string_view>

#include "ast/ast.h"
#include "base/arena.h"
#include "base/intern.h"
#include "diag/engine.h"
#include "json/json.h"

namespace manta {

// The language revision this implementation targets. 1.1 adds the
// end-of-content marker of spec 2.8; 1.2 adds the 'cable' declaration, the
// '@type' system field and the area unit; 1.3 adds the render section marker
// inside a block body. Each changes what a .manta file may contain.
inline constexpr std::string_view kLanguageVersion = "1.3";

// True when an object's revision is no newer than the toolchain's, so the
// toolchain knows every construct it might contain.
[[nodiscard]] bool revisionAtMost(std::string_view object, std::string_view toolchain);

// Serialises a parsed source file. Output is deterministic: fixed key order, no
// floating point, no timestamps (spec 15.8).
void writeObject(const SourceUnit& unit, const StringInterner& interner,
                 std::string_view sourcePath, std::string& out);

struct ObjectFile {
    SourceUnit unit;
    std::string sourcePath;
    std::string version;
};

// Reads an object back. `file` is the FileId to attribute spans to, which the
// caller has registered with its SourceManager (either the real source, when it
// can be found, or a stand-in carrying just the path).
[[nodiscard]] bool readObject(const JsonValue& root, Arena& arena, StringInterner& interner,
                              FileId file, DiagEngine& diags, Span errorSpan, ObjectFile& out);

}  // namespace manta
