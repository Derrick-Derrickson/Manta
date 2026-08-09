// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link/symbols.h"

#include <format>

#include "obj/mantao.h"

namespace manta {

bool Revision::parse(std::string_view text, Revision& out) {
    std::size_t dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == text.size()) return false;

    // Zeroed explicitly: the default Revision is 1.0, and accumulating digits
    // onto a 1 would read "1.1" as 11.1.
    Revision parsed{0, 0};
    for (char c : text.substr(0, dot)) {
        if (c < '0' || c > '9') return false;
        parsed.major = parsed.major * 10 + static_cast<std::uint32_t>(c - '0');
    }
    for (char c : text.substr(dot + 1)) {
        if (c < '0' || c > '9') return false;
        parsed.minor = parsed.minor * 10 + static_cast<std::uint32_t>(c - '0');
    }
    out = parsed;
    return true;
}

Revision Revision::toolchain() {
    Revision r;
    (void)parse(kLanguageVersion, r);
    return r;
}

bool versionSatisfied(const VersionConstraint& c, Revision toolchain) {
    if (c.hasLo) {
        Revision lo{c.loMajor, c.loMinor};
        if (toolchain < lo) return false;
    }
    if (c.hasHi) {
        Revision hi{c.hiMajor, c.hiMinor};
        if (toolchain > hi) return false;
    }
    return true;
}

void SymbolTable::addDeclaration(const Item* item, std::uint32_t objectIndex) {
    if (!item || !valid(item->name.symbol)) return;

    // Spec 4.2: a static declaration is keyed by its object, so two libraries
    // may each declare "static part house-resistor-0603" without conflict.
    DeclKey key{item->name.symbol, item->isStatic ? objectIndex : 0u};

    Declaration decl{item, objectIndex, item->isStatic};
    auto [slot, inserted] = decls_.insert(key, decl);
    if (!inserted) {
        // Spec 4.1: "A name declared twice is error E-30." Static declarations
        // never reach here across objects, but a name declared twice *within*
        // one object is still an error however it is spelled.
        diags_.report(DiagId::E30, item->nameSpan, interner_.text(item->name.symbol),
                      slot->item ? "another object" : "here")
            .note(slot->item->nameSpan, "first declared here");
    } else {
        ordered_.emplace_back(key, decl);
    }

    // Nested declarations are visible design-wide too: spec 4.1 says a file
    // "is a collection of named declarations" with no implicit file-level
    // block, and a block body may contain further items (spec 19).
    for (const BodyEntry& e : item->body) {
        if (e.kind == BodyKind::Item) addDeclaration(e.item, objectIndex);
    }
}

void SymbolTable::addObject(const LinkedObject& object) {
    for (const Item* item : object.unit.items) addDeclaration(item, object.index);
}

const Declaration* SymbolTable::find(SymbolId name, std::uint32_t fromObject) const {
    // Internal linkage first: a static declaration shadows an external one of
    // the same name within its own object.
    if (const Declaration* d = decls_.find(DeclKey{name, fromObject})) {
        if (d->isStatic) return d;
    }
    return decls_.find(DeclKey{name, 0});
}

const Declaration* SymbolTable::require(SymbolId name, std::uint32_t fromObject, Span at) {
    const Declaration* d = find(name, fromObject);
    if (!d) diags_.report(DiagId::E31, at, interner_.text(name));
    return d;
}

void SymbolTable::checkVersions(Revision toolchain) {
    for (const auto& [key, decl] : ordered_) {
        for (const BodyEntry& e : decl.item->body) {
            const FieldDecl* f = nullptr;
            if (e.kind == BodyKind::Field) f = e.field;
            else if (e.kind == BodyKind::Stmt && e.stmt->kind == StmtKind::Field) f = e.stmt->field;
            if (!f || f->ns != FieldNamespace::System) continue;
            if (interner_.text(f->name.symbol) != "VERSION") continue;
            if (!f->value || f->value->kind != ValueKind::Version) continue;

            if (!versionSatisfied(f->value->version, toolchain)) {
                diags_.report(DiagId::E36, f->value->span,
                              interner_.text(f->value->text), toolchain.text());
            }
        }
    }
}

}  // namespace manta
