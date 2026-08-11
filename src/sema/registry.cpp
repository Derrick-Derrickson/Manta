// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sema/registry.h"

#include <algorithm>
#include <array>

namespace manta {

namespace {

using namespace DirCtx;

// Spec 11.3 net directives, plus the pin directives of 11.5-11.7, the instance
// directive of 11.10 (revision 1.5) and the harness assignment of 12.1.
constexpr std::array<DirectiveInfo, 18> kDirectives{{
    // Net directives (spec 11.3).
    {"IMP",       ValueType::Resistance, Net | Netclass | Harness, true},
    {"CURRENT",   ValueType::Current,    Net | Netclass | Harness, true},
    {"PEAK",      ValueType::Current,    Net | Netclass | Harness, true},
    {"VOLTAGE",   ValueType::Voltage,    Net | Netclass | Harness, true},
    {"MAXDELAY",  ValueType::Time,       Net | Netclass | Harness, true},
    {"CLASS",     ValueType::Identifier, Net | Harness,            true},
    {"MATCH",     ValueType::MatchGroup, Net | Netclass | Harness, true},
    {"LAYER",     ValueType::Identifier, Net | Netclass | Harness, true},
    {"SHIELD",    ValueType::NetName,    Net | Netclass | Harness, true},
    {"TYPE",      ValueType::PinType,    Net | Pin | Harness,      true},
    {"STUB",      ValueType::None,       Net,                      false},
    // Revision 1.5: an explicit power-rail mark for rendering, whatever the
    // net's name or class. Masked as &CLASS is: it is the same kind of
    // display-affecting, net-level membership claim.
    {"RAIL",      ValueType::None,       Net | Harness,            false},

    // Pin directives (spec 11.5-11.7).
    {"PINDELAY",  ValueType::Time,       Pin,                      true},
    {"NET",       ValueType::NetName,    Pin,                      true},
    {"CASUAL",    ValueType::None,       Pin,                      false},
    {"SWAP",      ValueType::Identifier, Pin,                      true},

    // Instance directives (spec 11.10, revision 1.5).
    {"EDGE",      ValueType::Edge,       DirCtx::Instance,         true},

    // Harness assignment (spec 12.1).
    {"HARNESS",   ValueType::Identifier, Net | Harness,            true},
}};

// Spec 9.5, plus @FLATFORMAT from 13.4 and the match-group fields of 11.4.
constexpr std::array<SystemFieldInfo, 13> kSystemFields{{
    {"footprint",  ValueType::Identifier, false},
    {"fitted",     ValueType::Boolean,    false},
    {"bom",        ValueType::Boolean,    false},
    {"type",       ValueType::Identifier, false},

    // Mating (spec 12A). One field per side, so the two never appear on the
    // same declaration and cannot be confused for one another.
    {"mate",       ValueType::Identifier, false},  // board connector -> cable
    {"mates",      ValueType::DesigList,  false},  // cable connector -> board part
    {"map",        ValueType::DesigList,  false},  // the pin correspondence
    {"VERSION",    ValueType::Version,    false},
    {"FLATFORMAT", ValueType::Identifier, false},

    // Legal only inside a match group.
    {"src",        ValueType::Designator, true},
    {"dest",       ValueType::DesigList,  true},
    {"tolerance",  ValueType::Time,       true},
    {"offset",     ValueType::Time,       true},
}};

// Case-insensitive comparison, used only for "did you mean" suggestions and for
// detecting the wrong-case spellings that E-34 reports.
bool equalFold(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return true;
}

// Levenshtein distance, capped: only used to suggest a correction.
std::size_t editDistance(std::string_view a, std::string_view b, std::size_t cap) {
    if (a.size() > b.size()) std::swap(a, b);
    if (b.size() - a.size() > cap) return cap + 1;

    std::array<std::size_t, 64> prev{}, cur{};
    if (a.size() >= prev.size() - 1) return cap + 1;
    for (std::size_t j = 0; j <= a.size(); ++j) prev[j] = j;

    for (std::size_t i = 1; i <= b.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= a.size(); ++j) {
            std::size_t cost = (a[j - 1] == b[i - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[a.size()];
}

template <typename Range, typename Get>
std::string_view nearest(Range&& table, Get get, std::string_view name) {
    std::string_view best;
    std::size_t bestDist = 3;  // only suggest a genuinely close match
    for (const auto& entry : table) {
        std::string_view candidate = get(entry);
        if (equalFold(candidate, name)) return candidate;
        std::size_t d = editDistance(candidate, name, bestDist);
        if (d < bestDist) {
            bestDist = d;
            best = candidate;
        }
    }
    return best;
}

}  // namespace

const DirectiveInfo* lookupDirective(std::string_view name) noexcept {
    for (const auto& d : kDirectives) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

const SystemFieldInfo* lookupSystemField(std::string_view name) noexcept {
    for (const auto& f : kSystemFields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

bool lookupPinType(std::string_view value, PinType& out, bool& caseError) noexcept {
    struct Entry {
        std::string_view name;
        PinType type;
    };
    static constexpr Entry kTypes[] = {
        {"PASSIVE", PinType::Passive}, {"SIGNAL", PinType::Signal},
        {"POWER", PinType::Power},     {"OPENDRAIN", PinType::OpenDrain},
        {"NC", PinType::NC},           {"GROUND", PinType::Ground},
    };

    caseError = false;
    for (const auto& e : kTypes) {
        if (e.name == value) {
            out = e.type;
            return true;
        }
    }
    // Spec 2.6: "System field values and directive values drawn from a fixed set
    // shall be upper case." A recognisable but wrongly-cased spelling is E-34,
    // not an unknown value.
    for (const auto& e : kTypes) {
        if (equalFold(e.name, value)) {
            out = e.type;
            caseError = true;
            return true;
        }
    }
    return false;
}

bool lookupEdgeSide(std::string_view value, EdgeSide& out, bool& caseError) noexcept {
    struct Entry {
        std::string_view name;
        EdgeSide side;
    };
    static constexpr Entry kSides[] = {
        {"LEFT", EdgeSide::Left},
        {"RIGHT", EdgeSide::Right},
        {"TOP", EdgeSide::Top},
        {"BOTTOM", EdgeSide::Bottom},
    };

    caseError = false;
    for (const auto& e : kSides) {
        if (e.name == value) {
            out = e.side;
            return true;
        }
    }
    // Spec 2.6, exactly as &TYPE: a recognisable but wrongly-cased spelling is
    // E-34, not an unknown value.
    for (const auto& e : kSides) {
        if (equalFold(e.name, value)) {
            out = e.side;
            caseError = true;
            return true;
        }
    }
    return false;
}

std::string_view edgeSideName(EdgeSide s) noexcept {
    switch (s) {
        case EdgeSide::Left: return "LEFT";
        case EdgeSide::Right: return "RIGHT";
        case EdgeSide::Top: return "TOP";
        case EdgeSide::Bottom: return "BOTTOM";
    }
    return "LEFT";
}

namespace {

struct PartTypeEntry {
    std::string_view name;
    PartType type;
};

constexpr std::array<PartTypeEntry, 5> kPartTypes{{
    {"board_part", PartType::BoardPart},
    {"boardconnector", PartType::BoardConnector},
    {"cableconnector", PartType::CableConnector},
    {"wire", PartType::Wire},
    {"crimp", PartType::Crimp},
}};

}  // namespace

PartType lookupPartType(std::string_view value, bool& nearMiss,
                        std::string_view& suggestion) noexcept {
    nearMiss = false;
    suggestion = {};
    if (value.empty()) return PartType::BoardPart;

    for (const auto& e : kPartTypes) {
        if (e.name == value) return e.type;
    }

    // Not a structural role. It may be an ordinary classification -- 'resistor',
    // 'regulator' -- which is perfectly legal and means nothing to the compiler.
    // But a near miss on a structural role is almost certainly a typo, and the
    // consequence is silent: every mating check simply stops applying.
    std::string_view near = nearest(kPartTypes, [](const PartTypeEntry& e) { return e.name; },
                                    value);
    if (!near.empty()) {
        nearMiss = true;
        suggestion = near;
    }
    return PartType::Other;
}

std::string_view partTypeName(PartType t) noexcept {
    switch (t) {
        case PartType::BoardPart: return "board_part";
        case PartType::BoardConnector: return "boardconnector";
        case PartType::CableConnector: return "cableconnector";
        case PartType::Wire: return "wire";
        case PartType::Crimp: return "crimp";
        case PartType::Other: return "";
    }
    return "board_part";
}

std::string_view pinTypeName(PinType t) noexcept {
    switch (t) {
        case PinType::Passive: return "PASSIVE";
        case PinType::Signal: return "SIGNAL";
        case PinType::Power: return "POWER";
        case PinType::OpenDrain: return "OPENDRAIN";
        case PinType::NC: return "NC";
        case PinType::Ground: return "GROUND";
    }
    return "PASSIVE";
}

std::string_view portDirName(PortDir d) noexcept {
    switch (d) {
        case PortDir::None: return "none";
        case PortDir::In: return "in";
        case PortDir::Out: return "out";
        case PortDir::Bidir: return "bidir";
    }
    return "none";
}

bool lookupPortDir(std::string_view name, PortDir& out) noexcept {
    if (name == "none") { out = PortDir::None; return true; }
    if (name == "in") { out = PortDir::In; return true; }
    if (name == "out") { out = PortDir::Out; return true; }
    if (name == "bidir") { out = PortDir::Bidir; return true; }
    return false;
}

Unit expectedUnit(ValueType t) noexcept {
    switch (t) {
        case ValueType::Resistance: return Unit::Ohm;
        case ValueType::Current: return Unit::Ampere;
        case ValueType::Voltage: return Unit::Volt;
        case ValueType::Time: return Unit::Second;
        default: return Unit::None;
    }
}

std::string_view nearestDirective(std::string_view name) noexcept {
    return nearest(kDirectives, [](const DirectiveInfo& d) { return d.name; }, name);
}

std::string_view nearestSystemField(std::string_view name) noexcept {
    return nearest(kSystemFields, [](const SystemFieldInfo& f) { return f.name; }, name);
}

}  // namespace manta
