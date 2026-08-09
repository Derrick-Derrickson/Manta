// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// RFC 4122 section 4.3 name-based UUIDs, version 5 (SHA-1).
//
// A layout tool matches a netlist component to a footprint already placed on a
// board either by reference designator or by UUID. Matching by designator means
// re-annotating a design orphans every placement; matching by UUID does not, so
// manta emits one.
//
// The UUID is derived from the component's *instance path*, never its
// designator, which is the whole point: renaming U3 to U7 must not move the
// part. Version 5 is used rather than a scheme of manta's own so that anyone
// can recompute the same UUID from the same path without reading this file.
//
// Deterministic by construction, as spec 15.8 requires: SHA-1 of a fixed byte
// string, no clock and no randomness anywhere.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace manta {

using UuidBytes = std::array<std::uint8_t, 16>;

// manta's own namespace, generated once and frozen. Changing it would change
// every UUID manta has ever emitted and orphan every placed footprint, so it
// must never be regenerated.
inline constexpr UuidBytes kMantaNamespace = {0xae, 0x13, 0x0b, 0x25, 0x72, 0x66, 0x4f, 0xdc,
                                              0x8d, 0xa0, 0xa1, 0x33, 0x0e, 0x18, 0x95, 0x42};

// The canonical 8-4-4-4-12 lower-case rendering.
[[nodiscard]] std::string formatUuid(const UuidBytes& uuid);

[[nodiscard]] UuidBytes uuidV5(const UuidBytes& ns, std::string_view name);

// The identity of one instance path, as a formatted UUID. The path is joined
// with '/', so ["power", "U3"] hashes the name "power/U3".
[[nodiscard]] std::string pathUuid(const std::vector<std::string>& path, std::size_t count);

}  // namespace manta
