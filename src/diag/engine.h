// Diagnostic collection, severity policy and rendering.
//
// Output form is fixed by spec 15.6:
//     <file>:<line>:<column>: <severity>[<code>]: <message>
// and, under --json-diagnostics, one JSON object per line on stderr.
//
// Diagnostics are buffered rather than streamed so that a parallel compile can
// merge per-file results in command-line order and still produce byte-identical
// stderr (spec 15.8).
#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/flat_map.h"
#include "diag/diagnostic.h"
#include "source/source_manager.h"

namespace manta {

// Per-code severity overrides from -W/-Wno-/--error=/--warn=/-Werror.
class SeverityPolicy {
public:
    void set(DiagId id, Severity s) { overrides_[static_cast<std::size_t>(id)] = s; }
    void setWerror(bool on) noexcept { werror_ = on; }

    // User-rule codes are not known when options are parsed, so a -W name that
    // matches no built-in is recorded here and resolved once the rules load.
    void setUser(std::string name, Severity s) { userOverrides_.set(std::move(name), s); }

    [[nodiscard]] Severity resolveUser(std::string_view name, Severity declared) const {
        Severity s = declared;
        if (const Severity* override_ = userOverrides_.find(std::string(name))) s = *override_;
        if (werror_ && s == Severity::Warning) return Severity::Error;
        return s;
    }

    [[nodiscard]] bool hasUserOverride(std::string_view name) const {
        return userOverrides_.find(std::string(name)) != nullptr;
    }

    [[nodiscard]] auto userOverrides() const { return userOverrides_.entries(); }

    [[nodiscard]] Severity resolve(DiagId id) const {
        Severity s = overrides_[static_cast<std::size_t>(id)];
        if (s == kUnset) s = diagInfo(id).defaultSeverity;
        // -Werror promotes warnings but never demotes an error, and never
        // resurrects a diagnostic that -Wno- switched off.
        if (werror_ && s == Severity::Warning) return Severity::Error;
        return s;
    }

private:
    static constexpr Severity kUnset = static_cast<Severity>(0xFF);
    std::array<Severity, static_cast<std::size_t>(DiagId::Count)> overrides_{
        [] {
            std::array<Severity, static_cast<std::size_t>(DiagId::Count)> a{};
            a.fill(kUnset);
            return a;
        }()};
    FlatMap<std::string, Severity> userOverrides_;
    bool werror_ = false;
};

// A builder returned by report(), so notes can be chained on:
//     engine.report(DiagId::E30, span, name, where).note(other, "first declared here");
class DiagBuilder {
public:
    DiagBuilder(std::vector<Diagnostic>* sink, Diagnostic d)
        : sink_(sink), diag_(std::move(d)) {}

    DiagBuilder(const DiagBuilder&) = delete;
    DiagBuilder& operator=(const DiagBuilder&) = delete;
    DiagBuilder(DiagBuilder&&) = default;

    ~DiagBuilder() {
        if (sink_) sink_->push_back(std::move(diag_));
    }

    DiagBuilder& note(Span span, std::string message) {
        if (sink_) diag_.notes.push_back(DiagNote{span, std::move(message)});
        return *this;
    }

private:
    std::vector<Diagnostic>* sink_;
    Diagnostic diag_;
};

class DiagEngine {
public:
    explicit DiagEngine(const SourceManager& sm, SeverityPolicy policy = {})
        : sources_(sm), policy_(std::move(policy)) {}

    template <typename... Args>
    DiagBuilder report(DiagId id, Span span, Args&&... args) {
        Severity sev = policy_.resolve(id);
        if (sev == Severity::Ignored) return DiagBuilder{nullptr, Diagnostic{}};

        Diagnostic d;
        d.id = id;
        d.severity = sev;
        d.span = span;
        d.message = std::vformat(diagInfo(id).format,
                                 std::make_format_args(args...));
        if (sev == Severity::Error) ++errors_;
        else if (sev == Severity::Warning) ++warnings_;
        return DiagBuilder{&diags_, std::move(d)};
    }

    // Reports a user rule's finding. Its code is the check's own name, so
    // "error[drive-high]" and "-Wno-drive-high" both work.
    void reportUser(std::string code, Severity declared, Span span, std::string message) {
        Severity sev = policy_.resolveUser(code, declared);
        if (sev == Severity::Ignored) return;

        Diagnostic d;
        d.id = DiagId::UserRule;
        d.severity = sev;
        d.span = span;
        d.message = std::move(message);
        d.userCode = std::move(code);
        if (sev == Severity::Error) ++errors_;
        else if (sev == Severity::Warning) ++warnings_;
        diags_.push_back(std::move(d));
    }

    [[nodiscard]] std::size_t errorCount() const noexcept { return errors_; }
    [[nodiscard]] std::size_t warningCount() const noexcept { return warnings_; }
    [[nodiscard]] bool hasErrors() const noexcept { return errors_ > 0; }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept { return diags_; }
    [[nodiscard]] const SourceManager& sources() const noexcept { return sources_; }
    [[nodiscard]] const SeverityPolicy& policy() const noexcept { return policy_; }

    // Rules name their checks only after options are parsed, so the policy is
    // replaceable up until the first user finding is reported.
    void setPolicy(SeverityPolicy policy) { policy_ = std::move(policy); }

    // Merges another engine's diagnostics, preserving the caller's order. Used
    // to fold per-file results from the compile thread pool.
    void absorb(DiagEngine&& other) {
        for (auto& d : other.diags_) diags_.push_back(std::move(d));
        errors_ += other.errors_;
        warnings_ += other.warnings_;
        other.diags_.clear();
    }

    void sortByLocation();

private:
    const SourceManager& sources_;
    SeverityPolicy policy_;
    std::vector<Diagnostic> diags_;
    std::size_t errors_ = 0;
    std::size_t warnings_ = 0;
};

struct RenderOptions {
    bool json = false;
    bool colour = false;
    bool showSource = true;  // caret line under the offending text
};

// Writes every buffered diagnostic to stderr in spec 15.6 form.
void renderDiagnostics(const DiagEngine& engine, const RenderOptions& opts, std::string& out);

}  // namespace manta
