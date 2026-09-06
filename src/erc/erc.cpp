// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "erc/erc.h"

#include <algorithm>
#include <format>

#include "base/flat_map.h"

namespace manta {

namespace {

// Spec 11.6: "A pin that can release a bus is '<>'; there is no separate
// tri-state type. E-01 fires only on multiple '>' pins."
bool isDriver(const ComponentPin& p) {
    return p.direction == PortDir::Out && p.type != PinType::OpenDrain &&
           p.type != PinType::Power;
}

bool isConsumer(const ComponentPin& p) {
    // An open-drain net has "many drivers permitted on one net" (spec 11.6) and
    // is held high by a pull-up rather than driven, so it is never an input
    // waiting for a driver. Power pins have their own checks, E-27 and E-28.
    return p.direction == PortDir::In && p.type != PinType::Power &&
           p.type != PinType::OpenDrain;
}

bool isPowerSource(const ComponentPin& p) {
    return p.type == PinType::Power && p.direction == PortDir::Out;
}

// Spec 11.6: "'>' provides the rail, '<' and '<>' consume it." A bidirectional
// supply pin -- a battery terminal, an OTG port's VBUS -- draws from the rail
// and never counts as its source.
bool isPowerConsumer(const ComponentPin& p) {
    return p.type == PinType::Power &&
           (p.direction == PortDir::In || p.direction == PortDir::Bidir);
}

// Normalises an identifier for W-07: "Two identifiers in one design differ only
// by '-' versus '_'."
std::string foldSeparators(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c == '_') c = '-';
    }
    return out;
}

}  // namespace

std::string_view ErcChecker::nameOf(const Component& c) const {
    return c.designator.empty() ? std::string_view(c.identity) : std::string_view(c.designator);
}

bool ErcChecker::isCapacitor(const Component& c) const {
    // See docs/assumptions.md, C1. The specification never says how a capacitor
    // is identified, yet W-04 depends on it.
    if (c.type == "capacitor") return true;
    // A '#type' user field is still honoured, because a design written before
    // 'type' moved to the system namespace is still a valid design.
    for (const auto& [name, value] : c.fields) {
        if (name == "type" && value == "capacitor") return true;
    }
    if (c.pins.size() != 2) return false;
    for (const auto& [name, value] : c.fields) {
        if (name != "value") continue;
        Dimensioned d;
        if (parseDimensioned(value, d) && d.unit == Unit::Farad) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Do-not-populate isolation (spec 16.3)
// ---------------------------------------------------------------------------

void ErcChecker::markDnpIsolated() {
    dnpIsolated_.assign(design_.nets.size(), false);

    // A net touched by an unfitted *series* part -- one with two or more pins --
    // is intentionally open in this build, so E-02 is suppressed across it.
    for (const Component& c : design_.components) {
        if (c.fitted) continue;
        if (c.pins.size() < 2) continue;
        for (const ComponentPin& p : c.pins) {
            if (p.net >= 0) dnpIsolated_[static_cast<std::size_t>(p.net)] = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

void ErcChecker::checkDrivers() {
    for (std::size_t i = 0; i < design_.nets.size(); ++i) {
        const Net& net = design_.nets[i];
        if (net.ground) continue;

        std::size_t drivers = 0;
        std::size_t consumers = 0;
        std::size_t passives = 0;
        std::size_t supplies = 0;
        std::size_t bidirs = 0;
        Span firstDriver;

        for (const PinRef& ref : net.pins) {
            const ComponentPin& pin = design_.components[ref.component].pins[ref.pin];
            if (isDriver(pin)) {
                ++drivers;
                if (!firstDriver.valid()) firstDriver = pin.span;
            }
            if (isConsumer(pin)) ++consumers;
            if (isPowerSource(pin)) ++supplies;
            // A '<>' pin can drive. It is not a *driver* for E-01 -- spec
            // 11.6: "E-01 fires only on multiple '>' pins" -- but a GPIO wired
            // straight into an input is normal, not a floating input.
            if (pin.direction == PortDir::Bidir) ++bidirs;
            // Spec 11.6: a pin with no arrow and no '&TYPE' is PASSIVE, which
            // "claims nothing".
            if (pin.type == PinType::Passive) ++passives;
        }

        // Spec 16.1 E-01: "Two or more '>' pins drive one net, with no
        // open-drain or bus declaration."
        if (drivers >= 2) {
            diags_.report(DiagId::E01, net.firstSeen, net.name,
                          static_cast<std::int64_t>(drivers));
        }

        // E-02: "A net has an input and no driver."
        //
        // A net is driven when something can set its level. A '>' pin does; so
        // does a '<>' pin, and so does a '&TYPE=POWER>' supply -- an enable
        // tied to a rail is tied, not floating. A passive pin means something
        // is attached that the checker cannot reason about, which is what a
        // pull-up, a divider or a filter looks like from here; firing on those
        // would make the rule useless on any real board.
        //
        // What is left is the case the rule exists for: a net that is nothing
        // but inputs, which is an input somebody forgot to connect.
        if (consumers > 0 && drivers == 0 && supplies == 0 && passives == 0 && bidirs == 0) {
            // A net that is a block input is driven from outside, and one
            // downstream of an unfitted series part is deliberately open.
            bool suppressed = dnpIsolated_[i] || net.direction == PortDir::In ||
                              net.direction == PortDir::Bidir || net.global;
            if (!suppressed) {
                diags_.report(DiagId::E02, net.firstSeen,
                              std::format("net '{}' has an input pin and no driver", net.name));
            }
        }
    }
}

void ErcChecker::checkFootprints() {
    for (const Component& c : design_.components) {
        // A footprint is a place on a board. A wire and a crimp have neither a
        // board nor a place on one, so E-20 is not about them.
        if (c.partType == PartType::Wire || c.partType == PartType::Crimp) continue;
        if (design_.kind == "cable") continue;
        // Spec 9.5: "@footprint ... Required for any fitted part."
        if (c.fitted && c.footprint.empty()) {
            diags_.report(DiagId::E20, c.span, nameOf(c));
        }
    }
}

void ErcChecker::checkGroundDeclared() {
    // A loom carries whatever the boards at its ends carry. Requiring it to
    // declare a ground of its own would mean inventing one.
    if (design_.kind == "cable") return;
    // Spec 5.3: "A design that declares no ground net is error E-24."
    for (const Net& net : design_.nets) {
        if (net.ground) return;
    }
    if (design_.components.empty() && design_.nets.empty()) return;
    diags_.report(DiagId::E24, Span{});
}

void ErcChecker::checkNotConnected() {
    for (const Component& c : design_.components) {
        for (const ComponentPin& pin : c.pins) {
            if (pin.type != PinType::NC) continue;
            if (pin.net < 0) continue;
            const Net& net = design_.nets[static_cast<std::size_t>(pin.net)];
            // Spec 11.6: "'&TYPE=NC' means the datasheet forbids connection,
            // and connecting it is error E-25."
            if (net.pins.size() > 1) {
                diags_.report(DiagId::E25, pin.span, pin.logical, nameOf(c), net.name);
            }
        }
    }
}

void ErcChecker::checkSingleReference() {
    for (const Net& net : design_.nets) {
        // Spec 11.8: "A net referenced exactly once is error E-26. A real net is
        // written at least twice, once at each end, so a single reference is
        // almost always a typo."
        if (net.stub) {
            // "A stub carries exactly one pin. &STUB on a net with more than
            // one pin is error E-33."
            if (net.pins.size() > 1) {
                diags_.report(DiagId::E33, net.firstSeen, net.name,
                              static_cast<std::int64_t>(net.pins.size()));
            }
            continue;
        }

        if (net.references != 1) continue;
        if (net.pins.size() > 1) continue;  // reached from a device, not a typo

        // A port is written once inside its block and connected from outside
        // it, so a single reference is exactly what a correct port looks like.
        // The rule exists to catch typos (spec 11.8), and neither a port, a
        // global, nor a harness *type* declaration can be one.
        if (net.direction != PortDir::None) continue;
        if (net.global || net.harness) continue;
        // A NC pin weakly implies &STUB (spec 11.8).
        bool nc = false;
        for (const PinRef& ref : net.pins) {
            if (design_.components[ref.component].pins[ref.pin].type == PinType::NC) nc = true;
        }
        if (nc) continue;

        diags_.report(DiagId::E26, net.firstSeen, net.name);
    }
}

void ErcChecker::checkPower() {
    for (const Net& net : design_.nets) {
        std::size_t sources = 0;
        std::size_t consumers = 0;
        for (const PinRef& ref : net.pins) {
            const ComponentPin& pin = design_.components[ref.component].pins[ref.pin];
            if (isPowerSource(pin)) ++sources;
            if (isPowerConsumer(pin)) ++consumers;
        }

        // Spec 11.6: "a net with POWER< pins and no POWER> source is error
        // E-27 ... A ground net is exempt from E-27." So is a net declared a
        // rail with '&TYPE=POWER' (spec 5.3): a rail that arrives through an
        // inductor, a diode-OR or whichever connector has a supply plugged in
        // has no sourcing pin, and the declaration is how the design says so.
        if (consumers > 0 && sources == 0 && !net.ground && !net.power) {
            // A global import is supplied by another object, and a net declared
            // an input to this block is driven from outside it.
            bool supplied = net.global || net.direction == PortDir::In ||
                            net.direction == PortDir::Bidir;
            if (!supplied) diags_.report(DiagId::E27, net.firstSeen, net.name);
        }

        // "two POWER> pins on one net is error E-28"
        if (sources >= 2) diags_.report(DiagId::E28, net.firstSeen, net.name);
    }
}

// ---------------------------------------------------------------------------
// Warnings
// ---------------------------------------------------------------------------

void ErcChecker::checkUnusedPins() {
    for (const Component& c : design_.components) {
        // Spec 16.2 W-01: "A declared part has pins appearing in no chain and
        // no binding. Catches unused sections of a multi-unit package."
        std::size_t unused = 0;
        for (const ComponentPin& pin : c.pins) {
            if (pin.connected || pin.unbound) continue;
            if (pin.type == PinType::NC) continue;  // not connecting it is the point
            ++unused;
        }
        if (unused > 0) {
            diags_.report(DiagId::W01, c.span, nameOf(c), static_cast<std::int64_t>(unused));
        }
    }
}

void ErcChecker::checkShortedDevices() {
    for (std::uint32_t index : design_.shorted) {
        const Component& c = design_.components[index];
        diags_.report(DiagId::W02, c.span, nameOf(c));
    }
}

void ErcChecker::checkCapacitors() {
    // W-04: "A '&TYPE=POWER<' pin has no capacitor on its net within two nodes."
    // Two nodes means: on the pin's own net, or on a net one series component
    // away from it.
    std::vector<bool> hasCapacitor(design_.nets.size(), false);
    for (const Component& c : design_.components) {
        if (!isCapacitor(c)) continue;
        for (const ComponentPin& pin : c.pins) {
            if (pin.net >= 0) hasCapacitor[static_cast<std::size_t>(pin.net)] = true;
        }
    }

    // One hop outward, so "within two nodes" is satisfied through a series part.
    std::vector<bool> withinTwo = hasCapacitor;
    for (const Component& c : design_.components) {
        bool touchesCapNet = false;
        for (const ComponentPin& pin : c.pins) {
            if (pin.net >= 0 && hasCapacitor[static_cast<std::size_t>(pin.net)]) {
                touchesCapNet = true;
            }
        }
        if (!touchesCapNet) continue;
        for (const ComponentPin& pin : c.pins) {
            if (pin.net >= 0) withinTwo[static_cast<std::size_t>(pin.net)] = true;
        }
    }

    for (const Component& c : design_.components) {
        for (const ComponentPin& pin : c.pins) {
            if (!isPowerConsumer(pin) || pin.net < 0) continue;
            const Net& net = design_.nets[static_cast<std::size_t>(pin.net)];
            if (net.ground) continue;
            if (withinTwo[static_cast<std::size_t>(pin.net)]) continue;
            diags_.report(DiagId::W04, pin.span, pin.logical, nameOf(c), net.name);
        }
    }
}

void ErcChecker::checkSimilarNames() {
    // W-07: "Two identifiers in one design differ only by '-' versus '_'."
    FlatMap<std::string, std::string> folded;
    for (const Net& net : design_.nets) {
        if (net.name.empty()) continue;
        std::string key = foldSeparators(net.name);
        auto [existing, inserted] = folded.insert(key, net.name);
        if (!inserted && *existing != net.name) {
            diags_.report(DiagId::W07, net.firstSeen, *existing, net.name);
        }
    }
}

void ErcChecker::checkSwapGroups() {
    // Spec 11.7: "All members of a swap group shall carry compatible
    // directives; a group whose members disagree is frozen and generates
    // warning W-08." Compatibility is judged on the directives that reach the
    // pin: its type, its direction and its pin delay.
    for (const Component& c : design_.components) {
        FlatMap<std::uint32_t, std::size_t> firstOfGroup;
        for (std::size_t i = 0; i < c.pins.size(); ++i) {
            const ComponentPin& pin = c.pins[i];
            if (!valid(pin.swapGroup)) continue;
            auto [slot, inserted] = firstOfGroup.insert(raw(pin.swapGroup), i);
            if (inserted) continue;

            // Compatibility is judged on *directives*, not on arrows. Spec 11.7's
            // own example gangs "IN[1:4]< &SWAP=ch" with "OUT[1:4]> &SWAP=ch":
            // the whole point of a ganged swap is that inputs and outputs
            // permute together, so differing directions are expected and must
            // not freeze the group.
            const ComponentPin& first = c.pins[*slot];
            bool compatible =
                first.typeExplicit == pin.typeExplicit &&
                (!first.typeExplicit || first.type == pin.type) &&
                first.hasPinDelay == pin.hasPinDelay &&
                (!first.hasPinDelay || first.pinDelay == pin.pinDelay);
            if (!compatible) {
                diags_.report(DiagId::W08, pin.span, interner_.text(pin.swapGroup))
                    .note(first.span, "first member of the group declared here");
                // Report a frozen group once, not once per member.
                firstOfGroup.set(raw(pin.swapGroup), i);
            }
        }
    }
}

// ---------------------------------------------------------------------------

void ErcChecker::run() {
    markDnpIsolated();

    checkGroundDeclared();
    checkFootprints();
    checkNotConnected();
    checkDrivers();
    checkPower();
    checkSingleReference();

    checkUnusedPins();
    checkShortedDevices();
    checkCapacitors();
    checkSimilarNames();
    checkSwapGroups();
}

}  // namespace manta
