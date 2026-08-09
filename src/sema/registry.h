// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The closed sets of system field names (spec 9.5) and directive names
// (spec 11.3, 11.5-11.7), and the value type each one takes.
//
// These are closed on purpose. Spec 9.1 makes an unknown '@' field error E-10
// and spec 11.3 makes an unknown '&' directive error E-13, precisely because the
// compiler interprets them; the open namespace is '#', which is carried to BOM
// and documentation untouched.
#pragma once

#include <cstdint>
#include <string_view>

#include "ast/ast.h"

namespace manta {

// Where a name may legally appear. A directive written in the wrong place is
// reported against the same code as an unknown one, with a message that says so.
namespace DirCtx {
inline constexpr std::uint8_t Net = 1u << 0;       // on a chain statement
inline constexpr std::uint8_t Pin = 1u << 1;       // on a pin map line or binding
inline constexpr std::uint8_t Netclass = 1u << 2;  // inside a netclass body
inline constexpr std::uint8_t Harness = 1u << 3;   // on a harness type or member
inline constexpr std::uint8_t Any = 0xFF;
}  // namespace DirCtx

enum class ValueType : std::uint8_t {
    None,        // the directive takes no value: &CASUAL, &STUB
    Resistance,  // &IMP
    Current,     // &CURRENT, &PEAK
    Voltage,     // &VOLTAGE
    Time,        // &MAXDELAY, &PINDELAY, @tolerance, @offset
    Identifier,  // &CLASS, &LAYER, &SWAP, &HARNESS, @footprint, @FLATFORMAT
    NetName,     // &SHIELD, &NET  (&NET also accepts '?')
    PinType,     // &TYPE: one of a fixed, upper-case set
    MatchGroup,  // &MATCH: a group name or a group with overrides
    Boolean,     // @fitted, @bom
    Version,     // @VERSION
    Designator,  // @src
    DesigList,   // @dest
};

struct DirectiveInfo {
    std::string_view name;
    ValueType type;
    std::uint8_t contexts;
    bool valueRequired;
};

struct SystemFieldInfo {
    std::string_view name;
    ValueType type;
    bool matchOnly;  // legal only inside a match group (spec 11.4)
};

// Returns nullptr when the name is not a known directive (error E-13).
[[nodiscard]] const DirectiveInfo* lookupDirective(std::string_view name) noexcept;

// Returns nullptr when the name is not a known system field (error E-10).
[[nodiscard]] const SystemFieldInfo* lookupSystemField(std::string_view name) noexcept;

// What a part is, as declared by '@type'.
//
// The value set is deliberately open: '@type' also carries ordinary
// classifications -- resistor, regulator, connector -- that the compiler has no
// business enumerating, and which travel to the BOM untouched. These five are
// the ones it *interprets*, because they decide what may appear in a cable and
// which parts take part in mating.
enum class PartType : std::uint8_t {
    BoardPart,       // "board_part", and the default when '@type' is unstated
    BoardConnector,  // something plugs into it
    CableConnector,  // it plugs into something
    Wire,            // a conductor; its pins are its cores
    Crimp,           // a terminal on a wire end
    Other,           // any other classification, carried through untouched
};

// Recognises a structural role. `nearMiss` is set when the spelling did not
// match but is close enough to one that it was probably meant: '@type' is an
// open set, so a typo would otherwise silently produce a part that takes no
// part in any mating check. That is W-10.
[[nodiscard]] PartType lookupPartType(std::string_view value, bool& nearMiss,
                                      std::string_view& suggestion) noexcept;

[[nodiscard]] std::string_view partTypeName(PartType t) noexcept;

// Spec 11.6: the fixed set of pin types. Values are upper case (spec 2.6), and
// a lower-case spelling is error E-34.
enum class PinType : std::uint8_t { Passive, Signal, Power, OpenDrain, NC, Ground };

// Recognises a &TYPE value. `caseError` is set when the spelling matched only
// after case folding, which is what E-34 reports.
[[nodiscard]] bool lookupPinType(std::string_view value, PinType& out, bool& caseError) noexcept;

[[nodiscard]] std::string_view pinTypeName(PinType t) noexcept;

// The name a port direction is written under wherever one is emitted or read
// back: the rules language (docs/rules.md, "direction == out") and the netlist's
// per-pin 'direction'. One spelling, so a rule and a netlist cannot disagree.
[[nodiscard]] std::string_view portDirName(PortDir d) noexcept;
[[nodiscard]] bool lookupPortDir(std::string_view name, PortDir& out) noexcept;

// The unit a value type demands, for reporting a mismatch.
[[nodiscard]] Unit expectedUnit(ValueType t) noexcept;

// Suggests the nearest known name, for "did you mean" on E-10 and E-13.
[[nodiscard]] std::string_view nearestDirective(std::string_view name) noexcept;
[[nodiscard]] std::string_view nearestSystemField(std::string_view name) noexcept;

}  // namespace manta
