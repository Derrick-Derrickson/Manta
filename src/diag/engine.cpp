#include "diag/engine.h"

#include <algorithm>
#include <format>

#include "base/utf8.h"

namespace manta {

namespace {

constexpr DiagInfo kTable[] = {
#define MANTA_DIAG(Enum, code, mnemonic, sev, msg) DiagInfo{code, mnemonic, sev, msg},
#include "diag/codes.def"
#undef MANTA_DIAG
};

static_assert(std::size(kTable) == static_cast<std::size_t>(DiagId::Count));

// ANSI colours, used only when stderr is a terminal and --no-colour is absent.
constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kRed = "\x1b[1;31m";
constexpr std::string_view kYellow = "\x1b[1;33m";
constexpr std::string_view kCyan = "\x1b[1;36m";

std::string_view severityColour(Severity s) {
    switch (s) {
        case Severity::Error: return kRed;
        case Severity::Warning: return kYellow;
        case Severity::Note: return kCyan;
        default: return {};
    }
}

void appendEscapedJson(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += c;
                }
        }
    }
}

// The caret line. Columns are codepoint counts, so the underline lands under the
// right characters even when an earlier comment on the line held multi-byte text.
void appendSourceLine(std::string& out, const SourceManager& sm, Span span, bool colour) {
    const SourceFile* file = sm.get(span.file);
    if (!file) return;
    LineCol lc = file->lineCol(span.offset);
    std::string_view line = file->lineText(lc.line);
    if (line.empty()) return;

    std::string gutter = std::format("{:>5} | ", lc.line);
    out += gutter;
    out += line;
    out += '\n';
    out.append(gutter.size() - 2, ' ');
    out += "| ";

    // Pad with the source's own tabs preserved so the caret tracks the text.
    std::size_t byteCol = 0;
    for (std::uint32_t i = 1; i < lc.column && byteCol < line.size();) {
        out += (line[byteCol] == '\t') ? '\t' : ' ';
        byteCol += static_cast<std::size_t>(utf8SequenceLength(
            static_cast<unsigned char>(line[byteCol])));
        ++i;
    }

    if (colour) out += kRed;
    out += '^';
    // Underline the rest of the span, clipped to the line.
    std::uint32_t remaining = span.length > 0 ? span.length - 1 : 0;
    std::size_t avail = line.size() > byteCol ? line.size() - byteCol - 1 : 0;
    out.append(std::min<std::size_t>(remaining, avail), '~');
    if (colour) out += kReset;
    out += '\n';
}

}  // namespace

const DiagInfo& diagInfo(DiagId id) noexcept {
    return kTable[static_cast<std::size_t>(id)];
}

bool lookupDiag(std::string_view name, DiagId& out) noexcept {
    for (std::size_t i = 0; i < std::size(kTable); ++i) {
        if (kTable[i].code == name || kTable[i].mnemonic == name) {
            out = static_cast<DiagId>(i);
            return true;
        }
    }
    return false;
}

void DiagEngine::sortByLocation() {
    std::stable_sort(diags_.begin(), diags_.end(),
                     [](const Diagnostic& a, const Diagnostic& b) {
                         return a.sortKey() < b.sortKey();
                     });
}

void renderDiagnostics(const DiagEngine& engine, const RenderOptions& opts, std::string& out) {
    const SourceManager& sm = engine.sources();

    for (const Diagnostic& d : engine.diagnostics()) {
        LineCol lc = sm.lineCol(d.span);
        std::string_view path = sm.pathOf(d.span.file);
        const DiagInfo& info = diagInfo(d.id);

        if (opts.json) {
            out += R"({"file":")";
            appendEscapedJson(out, path);
            out += std::format(R"(","line":{},"column":{},"severity":")", lc.line, lc.column);
            out += severityName(d.severity);
            out += R"(","code":")";
            out += info.code;
            out += R"(","message":")";
            appendEscapedJson(out, d.message);
            out += '"';
            if (!d.notes.empty()) {
                out += R"(,"notes":[)";
                for (std::size_t i = 0; i < d.notes.size(); ++i) {
                    if (i) out += ',';
                    LineCol nlc = sm.lineCol(d.notes[i].span);
                    out += R"({"file":")";
                    appendEscapedJson(out, sm.pathOf(d.notes[i].span.file));
                    out += std::format(R"(","line":{},"column":{},"message":")", nlc.line,
                                       nlc.column);
                    appendEscapedJson(out, d.notes[i].message);
                    out += R"("})";
                }
                out += ']';
            }
            out += "}\n";
            continue;
        }

        if (opts.colour) out += kBold;
        out += std::format("{}:{}:{}: ", path, lc.line, lc.column);
        if (opts.colour) {
            out += kReset;
            out += severityColour(d.severity);
        }
        out += std::format("{}[{}]", severityName(d.severity), info.code);
        if (opts.colour) out += kReset;
        out += ": ";
        out += d.message;
        out += '\n';

        if (opts.showSource) appendSourceLine(out, sm, d.span, opts.colour);

        for (const DiagNote& n : d.notes) {
            LineCol nlc = sm.lineCol(n.span);
            if (opts.colour) out += kBold;
            out += std::format("{}:{}:{}: ", sm.pathOf(n.span.file), nlc.line, nlc.column);
            if (opts.colour) {
                out += kReset;
                out += kCyan;
            }
            out += "note";
            if (opts.colour) out += kReset;
            out += ": ";
            out += n.message;
            out += '\n';
        }
    }
}

}  // namespace manta
