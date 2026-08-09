// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "source/span.h"

namespace manta {

enum class Severity : std::uint8_t {
    Ignored,
    Note,
    Warning,
    Error,
};

[[nodiscard]] constexpr std::string_view severityName(Severity s) noexcept {
    switch (s) {
        case Severity::Ignored: return "ignored";
        case Severity::Note: return "note";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "error";
}

enum class DiagId : std::uint16_t {
#define MANTA_DIAG(Enum, code, mnemonic, sev, msg) Enum,
#include "diag/codes.def"
#undef MANTA_DIAG
    Count
};

struct DiagInfo {
    std::string_view code;
    std::string_view mnemonic;
    Severity defaultSeverity;
    std::string_view format;
};

[[nodiscard]] const DiagInfo& diagInfo(DiagId id) noexcept;

// Resolves "E-22" or "bare-net-advance" to its id. Used by -W, -Wno-, --error=
// and --warn=.
[[nodiscard]] bool lookupDiag(std::string_view name, DiagId& out) noexcept;

// A secondary location attached to a diagnostic ("first declared here").
struct DiagNote {
    Span span;
    std::string message;
};

struct Diagnostic {
    DiagId id = DiagId::Internal;
    Severity severity = Severity::Error;
    Span span;
    std::string message;
    std::vector<DiagNote> notes;
    // A user rule's code, which is the check's own name. Empty for every
    // built-in diagnostic, whose code comes from the table.
    std::string userCode;

    [[nodiscard]] std::string_view code() const {
        return userCode.empty() ? diagInfo(id).code : std::string_view(userCode);
    }

    // Ordering key for deterministic output (spec 15.8): by file, then byte
    // offset, then id, so a parallel compile can merge without perturbing order.
    [[nodiscard]] auto sortKey() const noexcept {
        return std::tuple(span.file, span.offset, static_cast<std::uint16_t>(id));
    }
};

}  // namespace manta
