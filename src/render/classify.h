// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Chooses the shape a component is drawn as.
#pragma once

#include <initializer_list>
#include <string_view>

#include "link/netlist.h"

namespace manta::render {

// Everything downstream switches on the kind: geometry, pin text, and the
// body outline all follow from it. The classic kinds are only ever chosen
// when the part's pins prove the symbol honest -- a diode whose pins cannot
// be told apart is drawn as the generic box rather than guessed at.
enum class SymbolKind : std::uint8_t {
    Generic,    // rectangular IC body, pins sorted onto sides
    Connector,  // numbered rows, every pin on the right
    Resistor,
    Capacitor,
    CapacitorPolarised,
    Inductor,
    Ferrite,
    Diode,
    Zener,
    Tvs,
    Led,
    Crystal,
    Nmos,
    Pmos,
    Mosfet,  // channel unknown: drawn as the N outline, arrow omitted
    Npn,
    Pnp,
    OpAmp,
    Switch,
    Fuse,
    TestPoint,
    Wire,   // a cable conductor: a straight segment
    Crimp,  // a wire-end terminal: a filled dot
};

// Finds the pin whose logical name matches one of `names`, case-insensitively.
// Returns -1 when no pin, or more than one, matches: an ambiguous name is as
// useless to a symbol as a missing one. Shared between the classifier (which
// gates on it) and the geometry builder (which places by it).
[[nodiscard]] int findPinByNames(const Component& c,
                                 std::initializer_list<std::string_view> names);

[[nodiscard]] SymbolKind classifySymbol(const Component& c);

}  // namespace manta::render
