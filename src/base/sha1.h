// SHA-1 (FIPS 180-4).
//
// Present for exactly one reason: RFC 4122 name-based UUIDs are defined in terms
// of SHA-1, and manta emits them so a layout tool can recognise a component
// across a re-annotation (see src/export/uuid.h). It is not used for anything
// security-bearing, and must not be: SHA-1 is broken against collision attacks.
//
// Written out rather than taken from a library because the shipped compiler has
// no third-party dependencies.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace manta {

using Sha1Digest = std::array<std::uint8_t, 20>;

[[nodiscard]] Sha1Digest sha1(std::string_view data) noexcept;

// Lower-case hexadecimal, 40 characters.
[[nodiscard]] std::string sha1Hex(std::string_view data);

}  // namespace manta
