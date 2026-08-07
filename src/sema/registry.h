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

// Spec 11.6: the fixed set of pin types. Values are upper case (spec 2.6), and
// a lower-case spelling is error E-34.
enum class PinType : std::uint8_t { Passive, Signal, Power, OpenDrain, NC, Ground };

// Recognises a &TYPE value. `caseError` is set when the spelling matched only
// after case folding, which is what E-34 reports.
[[nodiscard]] bool lookupPinType(std::string_view value, PinType& out, bool& caseError) noexcept;

[[nodiscard]] std::string_view pinTypeName(PinType t) noexcept;

// The unit a value type demands, for reporting a mismatch.
[[nodiscard]] Unit expectedUnit(ValueType t) noexcept;

// Suggests the nearest known name, for "did you mean" on E-10 and E-13.
[[nodiscard]] std::string_view nearestDirective(std::string_view name) noexcept;
[[nodiscard]] std::string_view nearestSystemField(std::string_view name) noexcept;

}  // namespace manta
