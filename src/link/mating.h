// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Connectors, cables and what happens where they meet (spec 12A).
//
// A board says where a loom plugs in; a loom says what it plugs into. Neither
// alone can tell you whether the two fit, and a designer finds out on the bench.
// This is what checks it at link time instead.
//
// Only one board is ever linked at a time. A cable named by '@mate' is compiled
// on its own -- it is its own deliverable, with its own netlist and BOM -- and
// then the two are laid against each other. Nothing here elaborates a second
// board, and nothing here changes the board's netlist.
#pragma once

#include <string>
#include <vector>

#include "diag/engine.h"
#include "link/elaborate.h"
#include "link/netlist.h"
#include "link/symbols.h"

namespace manta {

// One board connector and the cable fitted to it, resolved.
struct MatedCable {
    std::string cableName;
    std::uint32_t boardComponent = 0;  // index into the board's components
    // The cable, elaborated on its own. Emitted separately under '--assembly'.
    Design design;
    // The index, within `design.components`, of the cable connector that mates
    // with this board connector.
    std::uint32_t nearConnector = 0;
    // The connector at the far end, when the cable has one. A loom with a
    // single housing -- a pigtail ending in bare wires -- has none.
    bool hasFarConnector = false;
    std::uint32_t farConnector = 0;
    // The far end plugs back into a connector on this same board: the design
    // plugs into another copy of itself.
    bool selfPlug = false;
    std::uint32_t farBoardComponent = 0;
    Span at;
};

// Resolves every '@mate' on the board and checks what it finds.
//
// `elaborateCable` is supplied by the caller rather than constructed here, so
// this stays free of the linker's object list and stays testable.
class MateChecker {
public:
    using ElaborateFn = Design (*)(void* context, SymbolId cableName, Span at);

    MateChecker(Design& board, const SymbolTable& symbols, StringInterner& interner,
                DiagEngine& diags)
        : board_(board), symbols_(symbols), interner_(interner), diags_(diags) {}

    // Reports E-44 when a cable holds something that is not a cable part. Safe
    // to call on a board, where it does nothing.
    void checkCableContents();

    // Resolves '@mate', checks the fit, and traces each conductor through.
    void run(ElaborateFn elaborate, void* context);

    [[nodiscard]] const std::vector<MatedCable>& mated() const noexcept { return mated_; }

private:
    // The pin correspondence between two mating connectors: near pin index ->
    // far pin index, both into their own component's `pins`.
    struct PinPairing {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        bool ok = false;
    };

    [[nodiscard]] const Item* partDecl(const Component& c) const;
    [[nodiscard]] std::string fieldOf(const Component& c, std::string_view name) const;

    bool resolveConnectors(MatedCable& m);
    PinPairing pairPins(const Component& near, const Component& far, Span at,
                        std::string_view nearName, std::string_view farName);
    void checkElectrical(const MatedCable& m, const PinPairing& pairing);

    Design& board_;
    const SymbolTable& symbols_;
    StringInterner& interner_;
    DiagEngine& diags_;
    std::vector<MatedCable> mated_;
};

}  // namespace manta
