// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "export/footprint_map.h"

#include <format>

namespace manta {

namespace {

constexpr bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace

bool loadFootprintMap(std::string_view text, std::string_view path, DiagEngine& diags,
                      FootprintMap& out) {
    bool ok = true;
    std::size_t line = 0;

    while (!text.empty()) {
        ++line;
        std::size_t eol = text.find('\n');
        std::string_view raw = eol == std::string_view::npos ? text : text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);

        if (std::size_t hash = raw.find('#'); hash != std::string_view::npos) {
            raw = raw.substr(0, hash);
        }
        raw = trim(raw);
        if (raw.empty()) continue;

        std::size_t split = 0;
        while (split < raw.size() && !isSpace(raw[split])) ++split;
        std::string_view name = raw.substr(0, split);
        std::string_view target = trim(raw.substr(split));

        if (target.empty()) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}:{}: '{}' has no footprint after it", path, line, name));
            ok = false;
            continue;
        }
        // Without a library nickname the entry would resolve to something the
        // layout tool still cannot find, which is the very thing this file
        // exists to prevent. Better to reject it here than to emit it.
        if (target.find(':') == std::string_view::npos) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}:{}: '{}' names no library; write 'Library:Footprint'",
                                     path, line, target));
            ok = false;
            continue;
        }
        if (out.entries.contains(std::string(name))) {
            diags.report(DiagId::Io, Span{},
                         std::format("{}:{}: '{}' is mapped twice", path, line, name));
            ok = false;
            continue;
        }
        out.entries.insert(std::string(name), std::string(target));
    }
    return ok;
}

std::string resolveFootprint(const FootprintMap* map, std::string_view defaultLib,
                             std::string_view raw, bool& qualified) {
    qualified = true;

    if (map) {
        if (const std::string* mapped = map->entries.find(std::string(raw))) return *mapped;
    }
    if (raw.find(':') != std::string_view::npos) return std::string(raw);
    if (!defaultLib.empty()) return std::format("{}:{}", defaultLib, raw);

    // An empty footprint is its own problem, reported elsewhere; do not claim it
    // is an unqualified one.
    qualified = raw.empty();
    return std::string(raw);
}

}  // namespace manta
