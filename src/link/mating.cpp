// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link/mating.h"

#include <algorithm>
#include <format>

namespace manta {

namespace {

std::string nameOf(const Component& c) {
    if (c.path.size() > 1) return flattenPath(c.path);
    return c.designator.empty() ? c.identity : c.designator;
}

// A pin that sources current or drives a signal. The same reading ERC takes,
// kept in step with it deliberately: a conflict through a cable is the same
// conflict as one on a board, and should read the same way.
bool isDriver(const ComponentPin& pin) {
    if (pin.type == PinType::OpenDrain) return false;
    return pin.direction == PortDir::Out &&
           (pin.type == PinType::Signal || pin.type == PinType::Power);
}

bool isSupply(const ComponentPin& pin) {
    return pin.type == PinType::Power && pin.direction == PortDir::Out;
}

bool isGround(const ComponentPin& pin) { return pin.type == PinType::Ground; }

}  // namespace

void MateChecker::checkCableContents() {
    if (board_.kind != "cable") return;
    for (const Component& c : board_.components) {
        switch (c.partType) {
            case PartType::CableConnector:
            case PartType::Wire:
            case PartType::Crimp:
                continue;
            case PartType::BoardPart:
            case PartType::BoardConnector:
            case PartType::Other:
                break;
        }
        // '@type' is an open set, so report what was actually written rather
        // than the structural role it fell back to: "is a resistor" is useful,
        // "is a board_part" is not.
        diags_.report(DiagId::E44, c.span, nameOf(c),
                      c.type.empty() ? std::string("board_part") : c.type);
    }
}

const Item* MateChecker::partDecl(const Component& c) const {
    if (!valid(c.part)) return nullptr;
    const Declaration* d = symbols_.find(c.part, 0);
    return d ? d->item : nullptr;
}

void MateChecker::run(ElaborateFn elaborate, void* context) {
    if (board_.kind == "cable") return;  // a loom has no looms of its own

    for (std::uint32_t i = 0; i < board_.components.size(); ++i) {
        const Component& c = board_.components[i];
        if (c.partType != PartType::BoardConnector || c.mate.empty()) continue;

        MatedCable m;
        m.cableName = c.mate;
        m.boardComponent = i;
        m.at = c.span;
        m.design = elaborate(context, interner_.intern(c.mate), c.span);
        if (m.design.kind != "cable") {
            // Either the name is not declared -- already E-31 from the
            // elaborator -- or it names something that is not a loom.
            if (!m.design.components.empty() || m.design.kind == "block") {
                diags_.report(DiagId::E45, c.span,
                              std::format("'{}' names '{}', which is not a cable",
                                          nameOf(c), c.mate));
            }
            continue;
        }

        if (!resolveConnectors(m)) continue;

        const Component& near = m.design.components[m.nearConnector];
        PinPairing pairing = pairPins(board_.components[i], near, c.span, nameOf(c),
                                      nameOf(near));
        if (!pairing.ok) continue;

        checkElectrical(m, pairing);
        mated_.push_back(std::move(m));
    }
}

bool MateChecker::resolveConnectors(MatedCable& m) {
    const Component& boardConn = board_.components[m.boardComponent];

    std::vector<std::uint32_t> fits;
    for (std::uint32_t k = 0; k < m.design.components.size(); ++k) {
        const Component& cc = m.design.components[k];
        if (cc.partType != PartType::CableConnector) continue;
        if (std::find(cc.mates.begin(), cc.mates.end(), boardConn.partName) != cc.mates.end()) {
            fits.push_back(k);
        }
    }

    if (fits.empty()) {
        std::string had;
        for (const Component& cc : m.design.components) {
            if (cc.partType != PartType::CableConnector) continue;
            if (!had.empty()) had += ", ";
            had += cc.partName;
        }
        diags_.report(DiagId::E45, m.at,
                      std::format("'{}' is a {} and mates with cable '{}', whose connectors "
                                  "are {}; none of them declares '@mates = {}'",
                                  nameOf(boardConn), boardConn.partName, m.cableName,
                                  had.empty() ? std::string("none") : had,
                                  boardConn.partName));
        return false;
    }

    // With two identical housings on a loom -- which is the common case -- both
    // fit, and either end may be the one plugged in here. Take the first in
    // elaboration order and treat the other as the far end; the two are
    // interchangeable by construction, so there is nothing to choose between.
    m.nearConnector = fits.front();
    for (std::uint32_t k = 0; k < m.design.components.size(); ++k) {
        if (k == m.nearConnector) continue;
        if (m.design.components[k].partType != PartType::CableConnector) continue;
        m.hasFarConnector = true;
        m.farConnector = k;
        break;
    }

    // Does the far end plug back into this same board? That is a board plugged
    // into another copy of itself, and one board's source says so.
    if (m.hasFarConnector) {
        const Component& far = m.design.components[m.farConnector];
        for (std::uint32_t b = 0; b < board_.components.size(); ++b) {
            // Not the connector the loom is already plugged into. A cable has
            // two ends and they go in two sockets; a housing that mates with
            // this part is not evidence that it mates with *this instance*.
            if (b == m.boardComponent) continue;
            const Component& other = board_.components[b];
            if (other.partType != PartType::BoardConnector) continue;
            if (std::find(far.mates.begin(), far.mates.end(), other.partName) ==
                far.mates.end()) {
                continue;
            }
            m.selfPlug = true;
            m.farBoardComponent = b;
            break;
        }
    }
    return true;
}

MateChecker::PinPairing MateChecker::pairPins(const Component& near, const Component& far,
                                              Span at, std::string_view nearName,
                                              std::string_view farName) {
    PinPairing out;

    auto findPhysical = [](const Component& c, const std::string& want) -> std::int32_t {
        for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
            if (c.pins[i].physical == want) return static_cast<std::int32_t>(i);
        }
        return -1;
    };

    // The map may be written on either side; a connector pair needs only one of
    // them to say how it is wired.
    const std::vector<std::pair<std::string, std::string>>* map = nullptr;
    bool reversed = false;
    if (!far.pinMap.empty()) {
        map = &far.pinMap;
        reversed = true;  // written far-to-near
    } else if (!near.pinMap.empty()) {
        map = &near.pinMap;
    }

    if (map) {
        for (const auto& [a, b] : *map) {
            const std::string& nearPin = reversed ? b : a;
            const std::string& farPin = reversed ? a : b;
            std::int32_t ni = findPhysical(near, nearPin);
            std::int32_t fi = findPhysical(far, farPin);
            if (ni < 0 || fi < 0) {
                diags_.report(DiagId::E46, at,
                              std::format("'@map' names pin {} of '{}' and pin {} of '{}'; {}",
                                          nearPin, nearName, farPin, farName,
                                          ni < 0 ? std::format("'{}' has no pin {}", nearName,
                                                               nearPin)
                                                 : std::format("'{}' has no pin {}", farName,
                                                               farPin)));
                return out;
            }
            out.pairs.emplace_back(static_cast<std::uint32_t>(ni),
                                   static_cast<std::uint32_t>(fi));
        }
        out.ok = true;
        return out;
    }

    // No map: pin 1 to pin 1, which is what a connector pair physically does.
    if (near.pins.size() != far.pins.size()) {
        diags_.report(DiagId::E46, at,
                      std::format("'{}' has {} pin(s) and '{}' has {}; a mating with no "
                                  "'@map' joins them one to one",
                                  nearName, near.pins.size(), farName, far.pins.size()));
        return out;
    }
    for (std::uint32_t i = 0; i < near.pins.size(); ++i) {
        std::int32_t fi = findPhysical(far, near.pins[i].physical);
        if (fi < 0) {
            diags_.report(DiagId::E46, at,
                          std::format("'{}' has pin {} and '{}' does not; give the mating an "
                                      "'@map'",
                                      nearName, near.pins[i].physical, farName));
            return out;
        }
        out.pairs.emplace_back(i, static_cast<std::uint32_t>(fi));
    }
    out.ok = true;
    return out;
}

void MateChecker::checkElectrical(const MatedCable& m, const PinPairing& pairing) {
    if (!m.selfPlug || !m.hasFarConnector) return;

    // The loom's far end plugs back into this board, so a conductor runs from
    // one board connector, through the cable, to another. Follow it and apply
    // the rules that would apply if the two had been wired together directly --
    // which, once the loom is fitted, they have been.
    const Component& nearBoard = board_.components[m.boardComponent];
    const Component& farBoard = board_.components[m.farBoardComponent];
    const Component& nearCable = m.design.components[m.nearConnector];
    const Component& farCable = m.design.components[m.farConnector];

    // Which cable pin reaches which, through the loom's own nets.
    auto through = [&](std::uint32_t nearCablePin) -> std::int32_t {
        std::int32_t net = nearCable.pins[nearCablePin].net;
        if (net < 0) return -1;
        // A conductor is a chain of two-terminal parts, so walk it.
        std::vector<bool> seen(m.design.nets.size(), false);
        std::vector<std::int32_t> queue{net};
        while (!queue.empty()) {
            std::int32_t n = queue.back();
            queue.pop_back();
            if (n < 0 || seen[static_cast<std::size_t>(n)]) continue;
            seen[static_cast<std::size_t>(n)] = true;
            for (const PinRef& ref : m.design.nets[static_cast<std::size_t>(n)].pins) {
                const Component& comp = m.design.components[ref.component];
                if (&comp == &farCable) return static_cast<std::int32_t>(ref.pin);
                if (comp.partType != PartType::Wire && comp.partType != PartType::Crimp) {
                    continue;
                }
                for (const ComponentPin& p : comp.pins) queue.push_back(p.net);
            }
        }
        return -1;
    };

    // The far cable connector's pins, paired to the far board connector's.
    PinPairing farPairing = pairPins(farBoard, farCable, m.at, nameOf(farBoard),
                                     nameOf(farCable));
    if (!farPairing.ok) return;

    for (const auto& [nearBoardPin, nearCablePin] : pairing.pairs) {
        std::int32_t farCablePin = through(nearCablePin);
        if (farCablePin < 0) continue;

        auto it = std::find_if(farPairing.pairs.begin(), farPairing.pairs.end(),
                               [&](const auto& p) {
                                   return p.second == static_cast<std::uint32_t>(farCablePin);
                               });
        if (it == farPairing.pairs.end()) continue;

        const ComponentPin& a = nearBoard.pins[nearBoardPin];
        const ComponentPin& b = farBoard.pins[it->first];

        if (isDriver(a) && isDriver(b)) {
            diags_.report(DiagId::E47, m.at,
                          std::format("'{}.{}' and '{}.{}' both drive, and cable '{}' joins "
                                      "them; a crossover map would keep them apart",
                                      nameOf(nearBoard), a.physical, nameOf(farBoard),
                                      b.physical, m.cableName));
        }
        if ((isSupply(a) && isGround(b)) || (isGround(a) && isSupply(b))) {
            diags_.report(DiagId::E48, m.at,
                          std::format("'{}.{}' is a supply and '{}.{}' is a ground, and cable "
                                      "'{}' joins them",
                                      nameOf(nearBoard), a.physical, nameOf(farBoard),
                                      b.physical, m.cableName));
        }
    }
}

}  // namespace manta
