// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/classify.h"

#include "lex/dimensioned.h"

namespace manta::render {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// '@type' is lowercase by convention but an open set, so match forgivingly.
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool isAny(std::string_view t, std::initializer_list<std::string_view> words) {
    for (std::string_view w : words) {
        if (iequals(t, w)) return true;
    }
    return false;
}

const std::string* userField(const Component& c, std::string_view name) {
    for (const auto& [key, value] : c.fields) {
        if (key == name) return &value;
    }
    return nullptr;
}

bool twoPin(const Component& c) { return c.pins.size() == 2; }

// A diode is only drawn as one when its pins say which end is which.
bool diodePinsKnown(const Component& c) {
    return twoPin(c) && findPinByNames(c, {"A", "ANODE"}) >= 0 &&
           findPinByNames(c, {"K", "CATHODE"}) >= 0;
}

bool mosfetPinsKnown(const Component& c) {
    return c.pins.size() == 3 && findPinByNames(c, {"G", "GATE"}) >= 0 &&
           findPinByNames(c, {"D", "DRAIN"}) >= 0 && findPinByNames(c, {"S", "SOURCE"}) >= 0;
}

bool bjtPinsKnown(const Component& c) {
    return c.pins.size() == 3 && findPinByNames(c, {"B", "BASE"}) >= 0 &&
           findPinByNames(c, {"C", "COLLECTOR"}) >= 0 && findPinByNames(c, {"E", "EMITTER"}) >= 0;
}

// The triangle needs its three signal pins; power pins are optional but must
// be recognisable, or the whole part falls back to the box.
bool opampPinsKnown(const Component& c) {
    if (findPinByNames(c, {"IN+", "INP", "+"}) < 0) return false;
    if (findPinByNames(c, {"IN-", "INN", "-"}) < 0) return false;
    if (findPinByNames(c, {"OUT", "OUTPUT"}) < 0) return false;
    for (const ComponentPin& p : c.pins) {
        if (isAny(p.logical, {"IN+", "INP", "+", "IN-", "INN", "-", "OUT", "OUTPUT", "V+", "VCC",
                              "VDD", "V-", "VEE", "VSS"})) {
            continue;
        }
        return false;
    }
    return true;
}

// Maps one classification word to a symbol, gated on the pins backing it up.
// Returns false when the word is not a classification this renderer knows;
// returns true with Generic when it is one but the pins cannot honestly carry
// the symbol -- the word spoke, so later tiers must not reinterpret it.
bool kindFromWord(const Component& c, std::string_view t, SymbolKind& out) {
    out = SymbolKind::Generic;
    if (iequals(t, "resistor")) {
        if (twoPin(c)) out = SymbolKind::Resistor;
    } else if (iequals(t, "capacitor")) {
        if (twoPin(c)) out = SymbolKind::Capacitor;
    } else if (isAny(t, {"capacitor-polarised", "capacitor-polarized", "electrolytic",
                         "tantalum"})) {
        if (twoPin(c)) out = SymbolKind::CapacitorPolarised;
    } else if (iequals(t, "inductor")) {
        if (twoPin(c)) out = SymbolKind::Inductor;
    } else if (isAny(t, {"ferrite", "bead", "ferrite-bead"})) {
        if (twoPin(c)) out = SymbolKind::Ferrite;
    } else if (iequals(t, "diode")) {
        if (diodePinsKnown(c)) out = SymbolKind::Diode;
    } else if (iequals(t, "zener")) {
        if (diodePinsKnown(c)) out = SymbolKind::Zener;
    } else if (iequals(t, "tvs")) {
        if (diodePinsKnown(c)) out = SymbolKind::Tvs;
    } else if (iequals(t, "led")) {
        if (diodePinsKnown(c)) out = SymbolKind::Led;
    } else if (isAny(t, {"crystal", "resonator", "oscillator"})) {
        if (twoPin(c)) out = SymbolKind::Crystal;
    } else if (iequals(t, "nmos")) {
        if (mosfetPinsKnown(c)) out = SymbolKind::Nmos;
    } else if (iequals(t, "pmos")) {
        if (mosfetPinsKnown(c)) out = SymbolKind::Pmos;
    } else if (iequals(t, "mosfet")) {
        // '@type=mosfet' leaves the channel to a '#channel' field. Without
        // one, the symbol is still a MOSFET -- just drawn without the arrow
        // that would assert a polarity nobody stated.
        if (mosfetPinsKnown(c)) {
            out = SymbolKind::Mosfet;
            if (const std::string* ch = userField(c, "channel")) {
                if (isAny(*ch, {"n", "nmos", "n-channel"})) out = SymbolKind::Nmos;
                else if (isAny(*ch, {"p", "pmos", "p-channel"})) out = SymbolKind::Pmos;
            }
        }
    } else if (iequals(t, "npn")) {
        if (bjtPinsKnown(c)) out = SymbolKind::Npn;
    } else if (iequals(t, "pnp")) {
        if (bjtPinsKnown(c)) out = SymbolKind::Pnp;
    } else if (iequals(t, "transistor")) {
        // A BJT symbol is its emitter arrow: with no '#polarity' (npn/pnp)
        // there is nothing honest to draw, so the box it stays.
        if (bjtPinsKnown(c)) {
            if (const std::string* pol = userField(c, "polarity")) {
                if (iequals(*pol, "npn")) out = SymbolKind::Npn;
                else if (iequals(*pol, "pnp")) out = SymbolKind::Pnp;
            }
        }
    } else if (isAny(t, {"opamp", "comparator"})) {
        if (opampPinsKnown(c)) out = SymbolKind::OpAmp;
    } else if (isAny(t, {"switch", "button"})) {
        if (twoPin(c)) out = SymbolKind::Switch;
    } else if (isAny(t, {"fuse", "ptc"})) {
        if (twoPin(c)) out = SymbolKind::Fuse;
    } else if (iequals(t, "testpoint")) {
        out = SymbolKind::TestPoint;
    } else if (iequals(t, "wire")) {
        if (twoPin(c)) out = SymbolKind::Wire;
    } else if (iequals(t, "crimp")) {
        if (twoPin(c)) out = SymbolKind::Crimp;
    } else {
        return false;
    }
    return true;
}

}  // namespace

int findPinByNames(const Component& c, std::initializer_list<std::string_view> names) {
    int found = -1;
    for (std::size_t i = 0; i < c.pins.size(); ++i) {
        if (!isAny(c.pins[i].logical, names)) continue;
        if (found >= 0) return -1;  // ambiguous: two pins claim the same role
        found = static_cast<int>(i);
    }
    return found;
}

// Three tiers, mirroring ErcChecker::isCapacitor: '@type', then the legacy
// '#type' user field, then the unit of '#value' for an untyped 2-pin part.
SymbolKind classifySymbol(const Component& c) {
    SymbolKind k = SymbolKind::Generic;
    if (!c.type.empty() && kindFromWord(c, c.type, k)) return k;

    // A design written before 'type' moved to the system namespace is still a
    // valid design (see erc.cpp), so its '#type' field is still honoured.
    if (const std::string* legacy = userField(c, "type"); legacy && kindFromWord(c, *legacy, k)) {
        return k;
    }

    switch (c.partType) {
        case PartType::BoardConnector:
        case PartType::CableConnector: return SymbolKind::Connector;
        default: break;
    }

    if (twoPin(c)) {
        if (const std::string* value = userField(c, "value")) {
            Dimensioned d;
            if (parseDimensioned(*value, d)) {
                switch (d.unit) {
                    case Unit::Ohm: return SymbolKind::Resistor;
                    case Unit::Farad: return SymbolKind::Capacitor;
                    case Unit::Henry: return SymbolKind::Inductor;
                    case Unit::Hertz: return SymbolKind::Crystal;
                    default: break;
                }
            }
        }
    }

    // A single pin has nothing to be but a place to probe.
    if (c.pins.size() == 1) return SymbolKind::TestPoint;

    return SymbolKind::Generic;
}

}  // namespace manta::render
