// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Electrical rule checking (spec 16).
//
// Spec 16 opens by explaining why these rules can be checked at all: "Manta's
// explicit intent markers -- '=*', '*=', '!', '&STUB', '&NET=?', '&CASUAL',
// '&!' -- let the checker distinguish deliberate constructs from mistakes."
//
// ERC runs at link, not at compile, because no-driver, no-source,
// multiple-driver and unpowered-net are whole-design properties (spec 15.3).
#pragma once

#include "base/intern.h"
#include "diag/engine.h"
#include "link/netlist.h"

namespace manta {

class ErcChecker {
public:
    ErcChecker(Design& design, StringInterner& interner, DiagEngine& diags)
        : design_(design), interner_(interner), diags_(diags) {}

    void run();

private:
    // Section 16.1 -- errors.
    void checkDrivers();          // E-01, E-02
    void checkFootprints();       // E-20
    void checkGroundDeclared();   // E-24
    void checkNotConnected();     // E-25
    void checkSingleReference();  // E-26, E-33
    void checkPower();            // E-27, E-28, W-09

    // Section 16.2 -- warnings.
    void checkUnusedPins();       // W-01
    void checkShortedDevices();   // W-02
    void checkCapacitors();       // W-03, W-04
    void checkSimilarNames();     // W-07
    void checkSwapGroups();       // W-08

    // Spec 16.3: "The checker shall treat nets downstream of an unfitted series
    // part as intentionally open and suppress E-02 across that boundary."
    void markDnpIsolated();

    // See docs/assumptions.md: the specification never defines how a capacitor
    // is recognised, and W-03 and W-04 both need to.
    [[nodiscard]] bool isCapacitor(const Component& c) const;

    [[nodiscard]] std::string_view nameOf(const Component& c) const;

    Design& design_;
    StringInterner& interner_;
    DiagEngine& diags_;
    std::vector<bool> dnpIsolated_;
};

}  // namespace manta
