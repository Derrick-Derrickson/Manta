// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Chooses the shape a component is drawn as.
#pragma once

#include "link/netlist.h"

namespace manta::render {

// The skeleton knows two shapes. Classic two-terminal symbols (resistor,
// capacitor, inductor, diode, ...) are added here, keyed on Component::type,
// without touching the callers: everything downstream switches on the kind.
enum class SymbolKind : std::uint8_t {
    Generic,    // rectangular IC body, pins sorted onto sides
    Connector,  // numbered rows, every pin on the right
};

[[nodiscard]] SymbolKind classifySymbol(const Component& c);

}  // namespace manta::render
