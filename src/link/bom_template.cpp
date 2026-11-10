// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link/bom_template.h"

#include <algorithm>
#include <format>

#include "base/flat_map.h"

namespace manta {

bool naturalLess(std::string_view a, std::string_view b) {
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            std::size_t si = i;
            std::size_t sj = j;
            while (i < a.size() && digit(a[i])) ++i;
            while (j < b.size() && digit(b[j])) ++j;
            std::string_view na = a.substr(si, i - si);
            std::string_view nb = b.substr(sj, j - sj);
            while (na.size() > 1 && na[0] == '0') na.remove_prefix(1);
            while (nb.size() > 1 && nb[0] == '0') nb.remove_prefix(1);
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            continue;
        }
        if (a[i] != b[j]) return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
        ++i;
        ++j;
    }
    if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
    return a < b;  // equal under natural order: fall back to bytes, so the order is total
}

namespace {

using Kind = TemplatePiece::Kind;

// Placement placeholders: positions, which a BOM does not have.
constexpr std::string_view kPlacementOnly[] = {"x", "y", "y_up", "rot", "side"};

bool parseTemplate(std::string_view text, Template& out, std::string& error) {
    std::string literal;
    auto flush = [&] {
        if (literal.empty()) return;
        TemplatePiece piece;
        piece.text = std::move(literal);
        out.pieces.push_back(std::move(piece));
        literal.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '}') {
            if (i + 1 < text.size() && text[i + 1] == '}') {
                literal += '}';
                ++i;
                continue;
            }
            error = std::format("'{}': a lone '}}'; write '}}}}' for a literal brace", text);
            return false;
        }
        if (c != '{') {
            literal += c;
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '{') {
            literal += '{';
            ++i;
            continue;
        }
        std::size_t close = text.find('}', i);
        if (close == std::string_view::npos) {
            error = std::format("'{}': an unclosed '{{'", text);
            return false;
        }
        std::string_view inner = text.substr(i + 1, close - i - 1);
        i = close;
        std::string_view name = inner;
        std::string_view arg;
        bool hasArg = false;
        if (std::size_t colon = inner.find(':'); colon != std::string_view::npos) {
            name = inner.substr(0, colon);
            arg = inner.substr(colon + 1);
            hasArg = true;
        }

        TemplatePiece piece;
        static constexpr std::pair<std::string_view, Kind> kNames[] = {
            {"ref", Kind::Ref},           {"refs", Kind::Refs},
            {"qty", Kind::Qty},           {"value", Kind::Value},
            {"footprint", Kind::Footprint}, {"footprint_name", Kind::FootprintName},
            {"lib", Kind::Lib},           {"part", Kind::Part},
            {"type", Kind::Type},         {"fitted", Kind::Fitted},
            {"field", Kind::Field},
        };
        bool known = false;
        for (const auto& [n, k] : kNames) {
            if (n == name) {
                piece.kind = k;
                known = true;
            }
        }
        if (!known) {
            if (std::find(std::begin(kPlacementOnly), std::end(kPlacementOnly), name) !=
                std::end(kPlacementOnly)) {
                error = std::format("'{{{}}}' is a placement placeholder; a BOM has no positions",
                                    name);
            } else {
                error = std::format("'{{{}}}' is not a placeholder manta knows", name);
            }
            return false;
        }
        if (piece.kind == Kind::Field) {
            if (!hasArg || arg.empty()) {
                error = "'{field}' needs a name: '{field:mpn}'";
                return false;
            }
            std::size_t bar = arg.find('|');
            piece.text = std::string(arg.substr(0, bar));
            if (bar != std::string_view::npos) piece.fallback = std::string(arg.substr(bar + 1));
        } else if (hasArg) {
            error = std::format("'{{{}}}' takes no argument", inner);
            return false;
        }
        if (piece.kind == Kind::Refs || piece.kind == Kind::Qty) out.perGroup = true;
        flush();
        out.pieces.push_back(std::move(piece));
    }
    flush();
    return true;
}

// The values one component contributes, worked out once.
struct Member {
    const Component* c = nullptr;
    std::string ref;
    std::string footprint;  // as resolved through the map, if any
};

struct BomGroup {
    std::vector<std::size_t> members;  // indices into the member list, natural ref order
    std::string refs;
    std::int64_t qty = 0;
};

std::string render(const Template& t, const Member& m, const BomGroup* g) {
    std::string out;
    for (const TemplatePiece& p : t.pieces) {
        switch (p.kind) {
            case Kind::Text: out += p.text; break;
            case Kind::Ref: out += m.ref; break;
            case Kind::Refs: if (g) out += g->refs; break;
            case Kind::Qty: if (g) out += std::to_string(g->qty); break;
            case Kind::Value: {
                std::string_view value = m.c->partName;
                for (const auto& [name, v] : m.c->fields) {
                    if (name == "value") value = v;
                }
                out += value;
                break;
            }
            case Kind::Footprint: out += m.footprint; break;
            case Kind::FootprintName: {
                std::size_t colon = m.footprint.find(':');
                out += colon == std::string::npos ? m.footprint : m.footprint.substr(colon + 1);
                break;
            }
            case Kind::Lib: {
                std::size_t colon = m.footprint.find(':');
                if (colon != std::string::npos) out += m.footprint.substr(0, colon);
                break;
            }
            case Kind::Part: out += m.c->partName; break;
            case Kind::Type: out += m.c->type; break;
            case Kind::Fitted: out += m.c->fitted ? "TRUE" : "FALSE"; break;
            case Kind::Field: {
                bool found = false;
                for (const auto& [name, v] : m.c->fields) {
                    if (name == p.text) {
                        out += v;
                        found = true;
                        break;
                    }
                }
                if (!found) out += p.fallback;
                break;
            }
        }
    }
    return out;
}

bool takeString(const TomlValue& table, std::string_view key, std::string& out,
                std::string& error) {
    const TomlValue* v = table.find(key);
    if (!v) return true;
    if (v->kind != TomlValue::Kind::String) {
        error = std::format("line {}: '{}' must be a string", v->line, key);
        return false;
    }
    out = v->string;
    return true;
}

bool takeStrings(const TomlValue& table, std::string_view key, std::vector<std::string>& out,
                 std::string& error) {
    const TomlValue* v = table.find(key);
    if (!v) return true;
    if (v->kind != TomlValue::Kind::Array) {
        error = std::format("line {}: '{}' must be an array of strings", v->line, key);
        return false;
    }
    for (const TomlValue& e : v->array) {
        if (e.kind != TomlValue::Kind::String) {
            error = std::format("line {}: '{}' must be an array of strings", v->line, key);
            return false;
        }
        out.push_back(e.string);
    }
    return true;
}

bool loadOutput(const TomlValue& output, BomTemplate& out, std::string& error) {
    // Keys manta reads, and keys it accepts and ignores. Anything else is a
    // typo, and a typo in a BOM template is a wrong BOM.
    static constexpr std::string_view kKnown[] = {"kind",      "file",  "group_by", "skip",
                                                  "sort",      "separator", "refs_separator",
                                                  "quote",     "columns", "name"};
    for (const auto& [k, v] : output.table) {
        if (std::find(std::begin(kKnown), std::end(kKnown), k) == std::end(kKnown)) {
            error = std::format("line {}: unknown key '{}' in an [[output]]", v.line, k);
            return false;
        }
    }

    auto parse = [&](const std::string& text, Template& t, int line) {
        std::string why;
        if (parseTemplate(text, t, why)) return true;
        error = std::format("line {}: {}", line, why);
        return false;
    };

    std::vector<std::string> groupBy;
    if (!takeStrings(output, "group_by", groupBy, error)) return false;
    for (const std::string& g : groupBy) {
        Template t;
        if (!parse(g, t, output.find("group_by")->line)) return false;
        if (t.perGroup) {
            error = std::format("line {}: group_by '{}' names a group total; group by what each "
                                "part is, not by its group",
                                output.find("group_by")->line, g);
            return false;
        }
        out.groupBy.push_back(std::move(t));
    }

    std::vector<std::string> skip;
    if (!takeStrings(output, "skip", skip, error)) return false;
    for (const std::string& s : skip) {
        if (s == "dnp") {
            out.skipDnp = true;
        } else if (s != "exclude_from_bom") {
            // exclude_from_bom is '@bom = FALSE', which no BOM ever lists.
            error = std::format("line {}: skip '{}': expected 'dnp' or 'exclude_from_bom'",
                                output.find("skip")->line, s);
            return false;
        }
    }

    std::string sort = "{ref}";
    if (!takeString(output, "sort", sort, error)) return false;
    if (!parse(sort, out.sort, output.find("sort") ? output.find("sort")->line : output.line)) {
        return false;
    }

    if (!takeString(output, "separator", out.separator, error)) return false;
    if (!takeString(output, "refs_separator", out.refsSeparator, error)) return false;
    if (out.separator.empty()) {
        error = std::format("line {}: separator is empty", output.find("separator")->line);
        return false;
    }

    std::string quote = "needed";
    if (!takeString(output, "quote", quote, error)) return false;
    if (quote == "always") out.quote = BomTemplate::Quote::Always;
    else if (quote == "needed") out.quote = BomTemplate::Quote::Needed;
    else if (quote == "never") out.quote = BomTemplate::Quote::Never;
    else {
        error = std::format("line {}: quote '{}': expected always, needed or never",
                            output.find("quote")->line, quote);
        return false;
    }

    const TomlValue* columns = output.find("columns");
    if (!columns || columns->kind != TomlValue::Kind::Array || columns->array.empty()) {
        error = std::format("line {}: a bom output needs columns = [[\"Header\", \"{{value}}\"], ...]",
                            columns ? columns->line : output.line);
        return false;
    }
    for (const TomlValue& col : columns->array) {
        if (col.kind != TomlValue::Kind::Array || col.array.size() != 2 ||
            col.array[0].kind != TomlValue::Kind::String ||
            col.array[1].kind != TomlValue::Kind::String) {
            error = std::format("line {}: each column is [\"Header\", \"template\"]", col.line);
            return false;
        }
        Template t;
        if (!parse(col.array[1].string, t, col.line)) return false;
        bool refs = std::any_of(t.pieces.begin(), t.pieces.end(),
                                [](const TemplatePiece& p) { return p.kind == Kind::Refs; });
        if (refs && out.quote == BomTemplate::Quote::Never &&
            out.refsSeparator.find(out.separator) != std::string::npos) {
            error = std::format("line {}: with quote = \"never\", a '{{refs}}' joined by '{}' "
                                "would split the row; change refs_separator or quote",
                                col.line, out.refsSeparator);
            return false;
        }
        out.columns.emplace_back(col.array[0].string, std::move(t));
    }
    return true;
}

void writeCell(std::string& out, std::string_view value, const BomTemplate& tpl) {
    bool quote = tpl.quote == BomTemplate::Quote::Always;
    if (tpl.quote == BomTemplate::Quote::Needed) {
        quote = value.find(tpl.separator) != std::string_view::npos ||
                value.find_first_of("\"\r\n") != std::string_view::npos;
    }
    if (!quote) {
        out += value;
        return;
    }
    out += '"';
    for (char c : value) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
}

}  // namespace

bool loadBomTemplate(const TomlValue& root, std::string_view path, DiagEngine& diags,
                     BomTemplate& out) {
    const TomlValue* outputs = root.find("output");
    const TomlValue* chosen = nullptr;
    if (outputs && outputs->kind == TomlValue::Kind::Array) {
        for (const TomlValue& o : outputs->array) {
            const TomlValue* kind = o.find("kind");
            if (!kind || kind->kind != TomlValue::Kind::String || kind->string != "bom") continue;
            if (chosen) {
                diags.report(DiagId::BomTemplate, Span{},
                             std::format("{}: line {}: a second [[output]] with kind = \"bom\"; "
                                         "manta writes one BOM, so keep one per file",
                                         path, o.line));
                return false;
            }
            chosen = &o;
        }
    }
    if (!chosen) {
        diags.report(DiagId::BomTemplate, Span{},
                     std::format("{}: no [[output]] with kind = \"bom\"", path));
        return false;
    }
    std::string error;
    if (!loadOutput(*chosen, out, error)) {
        diags.report(DiagId::BomTemplate, Span{}, std::format("{}: {}", path, error));
        return false;
    }
    return true;
}

bool renderBomTemplate(const Design& design, const BomTemplate& tpl,
                       const FootprintMap* footprints, DiagEngine& diags, std::string& out) {
    std::vector<Member> members;
    for (const Component& c : design.components) {
        if (!c.bom) continue;
        if (tpl.skipDnp && !c.fitted) continue;
        Member m;
        m.c = &c;
        m.ref = flatDesignator(c);
        bool qualified = false;
        m.footprint = footprints ? resolveFootprint(footprints, "", c.footprint, qualified)
                                 : c.footprint;
        members.push_back(std::move(m));
    }
    std::vector<std::size_t> order(members.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return naturalLess(members[a].ref, members[b].ref);
    });

    // Insertion-ordered by first member in ref order; no group_by means every
    // component is its own group.
    std::vector<BomGroup> groups;
    FlatMap<std::string, std::size_t> byKey;
    for (std::size_t i : order) {
        std::string key;
        if (tpl.groupBy.empty()) {
            key = std::to_string(i);
        } else {
            for (const Template& g : tpl.groupBy) {
                key += render(g, members[i], nullptr);
                key += '\x1f';  // keeps ("a","bc") and ("ab","c") apart
            }
        }
        auto [slot, inserted] = byKey.insert(key, groups.size());
        if (inserted) groups.emplace_back();
        groups[*slot].members.push_back(i);
    }
    for (BomGroup& g : groups) {
        for (std::size_t i : g.members) {
            if (!g.refs.empty()) g.refs += tpl.refsSeparator;
            g.refs += members[i].ref;
            // Spec 9.5: a DNP part is on the BOM with quantity zero.
            if (members[i].c->fitted) ++g.qty;
        }
    }

    std::vector<std::string> sortKeys;
    for (const BomGroup& g : groups) sortKeys.push_back(render(tpl.sort, members[g.members[0]], &g));
    std::vector<std::size_t> groupOrder(groups.size());
    for (std::size_t i = 0; i < groupOrder.size(); ++i) groupOrder[i] = i;
    std::stable_sort(groupOrder.begin(), groupOrder.end(), [&](std::size_t a, std::size_t b) {
        return naturalLess(sortKeys[a], sortKeys[b]);
    });

    bool ok = true;
    auto cell = [&](std::string_view value, std::string_view header) {
        if (tpl.quote == BomTemplate::Quote::Never &&
            (value.find(tpl.separator) != std::string_view::npos ||
             value.find_first_of("\"\r\n") != std::string_view::npos)) {
            diags.report(DiagId::BomTemplate, Span{},
                         std::format("column '{}': '{}' cannot be written with quote = \"never\"",
                                     header, value));
            ok = false;
        }
        writeCell(out, value, tpl);
    };

    for (std::size_t i = 0; i < tpl.columns.size(); ++i) {
        if (i) out += tpl.separator;
        cell(tpl.columns[i].first, tpl.columns[i].first);
    }
    out += '\n';

    for (std::size_t gi : groupOrder) {
        const BomGroup& g = groups[gi];
        for (std::size_t ci = 0; ci < tpl.columns.size(); ++ci) {
            const auto& [header, t] = tpl.columns[ci];
            std::string value = render(t, members[g.members[0]], &g);
            // A column the grouping does not pin down has to agree across the
            // group, or one row would speak for parts it does not describe.
            for (std::size_t k = 1; k < g.members.size(); ++k) {
                if (render(t, members[g.members[k]], &g) == value) continue;
                diags.report(DiagId::BomTemplate, Span{},
                             std::format("column '{}' differs within the group {}: '{}' on {}, "
                                         "'{}' on {}; add it to group_by or make them agree",
                                         header, g.refs, value, members[g.members[0]].ref,
                                         render(t, members[g.members[k]], &g),
                                         members[g.members[k]].ref));
                ok = false;
                break;
            }
            if (ci) out += tpl.separator;
            cell(value, header);
        }
        out += '\n';
    }
    return ok;
}

}  // namespace manta
