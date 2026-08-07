// Per-file semantic checks, run at compile.
//
// Spec 15.2 splits validation in two: compile does "well-formedness, and every
// rule requiring no external name", while spec 15.3 keeps ERC at link because
// "no-driver, no-source, multiple-driver and unpowered-net are whole-design
// properties and cannot be evaluated one object at a time".
//
// The rules that land here are exactly those decidable from one file:
//
//   E-08  a pin used both as a chain terminal and in the binding list
//   E-10  an unknown '@' field name
//   E-13  an unknown '&' directive name
//   E-18  a match tolerance written as a length
//   E-32  a block port carrying no direction arrow
//   E-34  a fixed-set value not written in upper case
//   E-38  a harness member list whose length differs from its pin range
//   E-43  a 'part' declaration exporting a field
//   E-04  a pin map whose physical and logical widths differ
//
// E-09, E-17 and E-37 are raised in the lexer and parser, where the malformed
// construct is in hand.
#pragma once

#include "ast/ast.h"
#include "base/intern.h"
#include "diag/engine.h"
#include "sema/registry.h"
#include "source/source_manager.h"

namespace manta {

class LocalChecker {
public:
    LocalChecker(const SourceFile& file, StringInterner& interner, DiagEngine& diags)
        : file_(file), interner_(interner), diags_(diags) {}

    void run(const SourceUnit& unit);

private:
    void checkItem(const Item* item);
    void checkBlockBody(const Item* block);
    void checkPartBody(const Item* part);
    void checkHarnessBody(const Item* harness);
    void checkNetclassBody(const Item* netclass);
    void checkMatchBody(const Item* group);

    void checkStatement(const Stmt* stmt);
    void checkChain(const Chain* chain);
    void checkSegment(const Segment* seg);
    void checkElement(const Element* el);
    void checkDevice(const Device* dev);
    void checkPinMap(const PinMap* pin);

    void checkDirective(const Directive* d, std::uint8_t context);
    void checkFieldDecl(const FieldDecl* f, bool inPart, bool inMatch);
    void checkValueType(const Directive* d, ValueType expected);

    // Interpolated names cannot be checked until link, when the substitution
    // has a value. text() returns empty for those, and every caller treats an
    // empty name as "defer".
    [[nodiscard]] std::string_view text(const Name& n) const;
    [[nodiscard]] std::int64_t rangeWidth(const Range& r) const;

    const SourceFile& file_;
    StringInterner& interner_;
    DiagEngine& diags_;
};

}  // namespace manta
