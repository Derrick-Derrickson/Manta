// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "export/exporters.h"

#include <format>

#include "export/uuid.h"
#include "sema/registry.h"

namespace manta {

bool parseExportFormat(std::string_view name, ExportFormat& out) {
    if (name == "kicad") { out = ExportFormat::KiCad; return true; }
    if (name == "altium") { out = ExportFormat::Altium; return true; }
    if (name == "orcad") { out = ExportFormat::OrCad; return true; }
    if (name == "allegro") { out = ExportFormat::Allegro; return true; }
    return false;
}

std::string_view exportExtension(ExportFormat format) {
    switch (format) {
        case ExportFormat::KiCad: return ".net";
        case ExportFormat::Altium: return ".NET";
        case ExportFormat::OrCad: return ".net";
        case ExportFormat::Allegro: return ".tel";
    }
    return ".net";
}

bool readNetlist(const JsonValue& root, DiagEngine& diags, Design& out) {
    if (root.kind != JsonKind::Object || root.str("kind") != "mantaNets") {
        diags.report(DiagId::Io, Span{}, std::string("not a '.mantaNets' netlist"));
        return false;
    }
    out.top = std::string(root.str("top"));

    if (const JsonValue* components = root.arr("components")) {
        for (const JsonPtr& c : components->array) {
            Component component;
            component.designator = std::string(c->str("designator"));
            component.identity = component.designator;
            component.partName = std::string(c->str("part"));
            component.fitted = c->boolean_("fitted", true);
            component.bom = c->boolean_("bom", true);
            component.footprint = std::string(c->str("footprint"));
            component.type = std::string(c->str("type"));
            if (component.type.empty()) component.type = "board_part";
            bool typeNearMiss = false;
            std::string_view typeSuggestion;
            component.partType = lookupPartType(component.type, typeNearMiss, typeSuggestion);
            if (const JsonValue* path = c->arr("path")) {
                for (const JsonPtr& p : path->array) component.path.push_back(p->text);
            }
            if (const JsonValue* fields = c->find("fields");
                fields && fields->kind == JsonKind::Object) {
                for (const auto& [name, value] : fields->object) {
                    component.fields.emplace_back(name, value->text);
                }
            }
            out.components.push_back(std::move(component));
        }
    }

    // Pins are recorded per net, so the components' pin lists are rebuilt from
    // the net side. A component index is needed to attach them.
    FlatMap<std::string, std::uint32_t> byDesignator;
    for (std::uint32_t i = 0; i < out.components.size(); ++i) {
        byDesignator.insert(out.components[i].designator, i);
    }

    if (const JsonValue* nets = root.arr("nets")) {
        for (const JsonPtr& n : nets->array) {
            Net net;
            net.name = std::string(n->str("name"));
            if (const JsonValue* pins = n->arr("pins")) {
                for (const JsonPtr& p : pins->array) {
                    const std::uint32_t* ci = byDesignator.find(std::string(p->str("designator")));
                    if (!ci) continue;
                    Component& component = out.components[*ci];
                    ComponentPin pin;
                    pin.physical = std::string(p->str("pin"));
                    pin.logical = std::string(p->str("logical"));
                    // Both are optional: a netlist written before spec 15.4
                    // carried them has no such key, and the defaults on
                    // ComponentPin are what it meant.
                    if (std::string_view type = p->str("type"); !type.empty()) {
                        PinType parsed{};
                        bool caseError = false;
                        if (lookupPinType(type, parsed, caseError)) pin.type = parsed;
                    }
                    if (std::string_view dir = p->str("direction"); !dir.empty()) {
                        PortDir parsed{};
                        if (lookupPortDir(dir, parsed)) pin.direction = parsed;
                    }
                    pin.net = static_cast<std::int32_t>(out.nets.size());
                    auto pinIndex = static_cast<std::uint32_t>(component.pins.size());
                    component.pins.push_back(std::move(pin));
                    net.pins.push_back(PinRef{*ci, pinIndex});
                }
            }
            if (const JsonValue* directives = n->find("directives");
                directives && directives->kind == JsonKind::Object) {
                for (const auto& [name, value] : directives->object) {
                    net.directives.set(name, NetDirective{value->text, Strength::Normal, Span{},
                                                          false});
                }
            }
            out.nets.push_back(std::move(net));
        }
    }

    if (const JsonValue* matches = root.arr("matches")) {
        for (const JsonPtr& m : matches->array) {
            MatchGroup group;
            group.name = std::string(m->str("name"));
            group.src = std::string(m->str("src"));
            group.tolerance = std::string(m->str("tolerance"));
            group.offset = std::string(m->str("offset"));
            if (const JsonValue* dest = m->arr("dest")) {
                for (const JsonPtr& d : dest->array) group.dest.push_back(d->text);
            }
            out.matches.push_back(std::move(group));
        }
    }

    // The optional swap section (docs/assumptions.md, C3).
    if (const JsonValue* swaps = root.arr("swaps")) {
        for (const JsonPtr& s : swaps->array) {
            SwapRecord record;
            record.designator = std::string(s->str("designator"));
            record.group = std::string(s->str("group"));
            if (const JsonValue* order = s->arr("order")) {
                for (const JsonPtr& o : order->array) record.order.push_back(o->text);
            }
            out.swaps.push_back(std::move(record));
        }
    }

    return true;
}

namespace {

// The flat name a layout tool sees. Spec 13.4: "Export flattens the path to the
// single unique string a BOM and a layout tool require."
std::string flatName(const Component& c, std::string_view flatFormat) {
    if (!flatFormat.empty()) return applyFlatFormat(flatFormat, c.path);
    if (c.path.size() <= 1) return c.designator;
    return flattenPath(c.path);
}

void escapeSExpr(std::string& out, std::string_view text) {
    out += '"';
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
}

// The footprint name this target should see. The map is only a rename table, so
// it applies to every format; the default library nickname and the W-FOOTPRINT
// warning are KiCad's alone, because only KiCad resolves 'Library:Footprint'.
std::string mappedFootprint(const ExportOptions& options, const Component& c) {
    bool qualified = false;
    return resolveFootprint(options.footprints, /*defaultLib=*/"", c.footprint, qualified);
}

// KiCad's electrical pin types, from eeschema's ELECTRICAL_PINTYPE. Pcbnew
// stores the string on the pad and its design-rule check reads it, so the
// mapping decides whether a power pin is recognised as one. Recorded in
// docs/assumptions.md, C6.
std::string_view kicadPinType(PinType type, PortDir direction) {
    switch (type) {
        case PinType::Power:
            // A part declaring a supply output drives the net; anything else on
            // a POWER pin consumes from it, which is also what an arrowless
            // supply pin means.
            return direction == PortDir::Out ? "power_out" : "power_in";
        case PinType::Ground:
            return "power_in";
        case PinType::OpenDrain:
            // KiCad has no open-drain type; open_collector is the same rule for
            // DRC's purposes, which is that the pin may only pull one way.
            return "open_collector";
        case PinType::NC:
            return "no_connect";
        case PinType::Signal:
            switch (direction) {
                case PortDir::In: return "input";
                case PortDir::Out: return "output";
                case PortDir::Bidir: return "bidirectional";
                case PortDir::None: return "unspecified";
            }
            return "unspecified";
        case PinType::Passive:
            return "passive";
    }
    return "passive";
}

std::string exportKiCad(const Design& design, const ExportOptions& options, DiagEngine& diags) {
    // KiCad's S-expression netlist. No timestamp or tool-version line is
    // written: spec 15.8 requires byte-identical output for identical input.
    //
    // The parser skips tokens it does not know and requires none of them, so
    // everything here is additive from its point of view.
    std::string out;
    out += "(export (version \"E\")\n";
    out += "  (design\n";
    out += "    (source ";
    escapeSExpr(out, design.top);
    out += ")\n    (tool \"manta\")\n  )\n";

    // One warning per distinct unqualified footprint, not per component: a
    // 200-part board would otherwise produce 200 identical lines. Components are
    // walked in order and the set is insertion-ordered, so the diagnostics come
    // out the same way every run.
    FlatSet<std::string> warned;

    out += "  (components\n";
    for (const Component& c : design.components) {
        bool qualified = false;
        std::string footprint =
            resolveFootprint(options.footprints, options.footprintLib, c.footprint, qualified);
        if (!qualified && !warned.contains(c.footprint)) {
            warned.insert(c.footprint);
            diags.report(DiagId::Footprint, c.span,
                         std::format("footprint '{}' names no KiCad library; it will not place. "
                                     "Give it one with --footprint-map or --footprint-lib",
                                     c.footprint));
        }

        out += "    (comp (ref ";
        escapeSExpr(out, flatName(c, options.flatFormat));
        out += ")\n      (value ";
        std::string_view value = c.partName;
        for (const auto& [name, v] : c.fields) {
            if (name == "value") value = v;
        }
        escapeSExpr(out, value);
        out += ")\n      (footprint ";
        escapeSExpr(out, footprint);
        out += ")\n      (libsource (lib \"manta\") (part ";
        escapeSExpr(out, c.partName);
        out += ") (description \"\"))\n";
        if (!c.fitted) out += "      (property (name \"dnp\") (value \"1\"))\n";
        if (!c.bom) out += "      (property (name \"exclude_from_bom\") (value \"1\"))\n";
        for (const auto& [name, v] : c.fields) {
            if (name == "value") continue;
            out += "      (property (name ";
            escapeSExpr(out, name);
            out += ") (value ";
            escapeSExpr(out, v);
            out += "))\n";
        }

        // A manta block instance is a KiCad sheet. Both the names and the UUIDs
        // are '/'-delimited and both ends carry a separator, which is the shape
        // KiCad writes for its own hierarchies.
        std::string sheetNames = "/";
        std::string sheetStamps = "/";
        for (std::size_t i = 0; i + 1 < c.path.size(); ++i) {
            sheetNames += c.path[i];
            sheetNames += '/';
            sheetStamps += pathUuid(c.path, i + 1);
            sheetStamps += '/';
        }
        out += "      (sheetpath (names ";
        escapeSExpr(out, sheetNames);
        out += ") (tstamps ";
        escapeSExpr(out, sheetStamps);
        out += "))\n      (tstamps ";
        escapeSExpr(out, pathUuid(c.path, c.path.size()));
        out += ")\n    )\n";
    }
    out += "  )\n";

    out += "  (nets\n";
    std::size_t code = 1;
    for (const Net& net : design.nets) {
        if (net.pins.empty()) continue;
        out += std::format("    (net (code \"{}\") (name ", code++);
        escapeSExpr(out, net.name);
        out += ")\n";
        for (const PinRef& ref : net.pins) {
            const Component& c = design.components[ref.component];
            const ComponentPin& pin = c.pins[ref.pin];
            out += "      (node (ref ";
            escapeSExpr(out, flatName(c, options.flatFormat));
            out += ") (pin ";
            escapeSExpr(out, pin.physical);
            if (!pin.logical.empty()) {
                out += ") (pinfunction ";
                escapeSExpr(out, pin.logical);
            }
            out += ") (pintype ";
            escapeSExpr(out, kicadPinType(pin.type, pin.direction));
            out += "))\n";
        }
        out += "    )\n";
    }
    out += "  )\n)\n";
    return out;
}

std::string exportAltium(const Design& design, const ExportOptions& options) {
    // Protel/Altium: a '[' block per component, a '(' block per net.
    std::string out;
    for (const Component& c : design.components) {
        std::string_view value = c.partName;
        for (const auto& [name, v] : c.fields) {
            if (name == "value") value = v;
        }
        out += "[\n";
        out += flatName(c, options.flatFormat);
        out += '\n';
        out += mappedFootprint(options, c);
        out += '\n';
        out += value;
        out += '\n';
        if (!c.fitted) out += "DNP\n";
        out += "]\n";
    }
    for (const Net& net : design.nets) {
        if (net.pins.empty()) continue;
        out += "(\n";
        out += net.name;
        out += '\n';
        for (const PinRef& ref : net.pins) {
            const Component& c = design.components[ref.component];
            out += flatName(c, options.flatFormat);
            out += '-';
            out += c.pins[ref.pin].physical;
            out += '\n';
        }
        out += ")\n";
    }
    return out;
}

std::string exportOrCad(const Design& design, const ExportOptions& options) {
    // OrCAD PCB II flat netlist.
    std::string out = "( { manta }\n";
    for (const Component& c : design.components) {
        std::string_view value = c.partName;
        for (const auto& [name, v] : c.fields) {
            if (name == "value") value = v;
        }
        std::string footprint = mappedFootprint(options, c);
        out += std::format(" ( {} {} {}\n", footprint.empty() ? "UNKNOWN" : footprint,
                           flatName(c, options.flatFormat), value);
        for (const ComponentPin& pin : c.pins) {
            std::string_view netName;
            if (pin.net >= 0 && static_cast<std::size_t>(pin.net) < design.nets.size()) {
                netName = design.nets[static_cast<std::size_t>(pin.net)].name;
            }
            out += std::format("  ( {} {} )\n", pin.physical, netName);
        }
        out += " )\n";
    }
    out += ")\n";
    return out;
}

std::string exportAllegro(const Design& design, const ExportOptions& options) {
    // Allegro Telesis.
    std::string out = "(Telesis netlist from manta)\n\n$PACKAGES\n";
    for (const Component& c : design.components) {
        std::string footprint = mappedFootprint(options, c);
        out += std::format("!{:<24}{:<24}{}\n", footprint.empty() ? "UNKNOWN" : footprint,
                           c.partName, flatName(c, options.flatFormat));
    }
    out += "\n$NETS\n";
    for (const Net& net : design.nets) {
        if (net.pins.empty()) continue;
        out += net.name;
        out += ";\t";
        std::size_t onLine = 0;
        for (const PinRef& ref : net.pins) {
            const Component& c = design.components[ref.component];
            if (onLine == 4) {
                out += "\n\t";
                onLine = 0;
            }
            out += std::format("{}.{} ", flatName(c, options.flatFormat), c.pins[ref.pin].physical);
            ++onLine;
        }
        out += "\n";
    }
    out += "\n$END\n";
    return out;
}

}  // namespace

std::string exportDesign(const Design& design, ExportFormat format, const ExportOptions& options,
                         DiagEngine& diags) {
    switch (format) {
        case ExportFormat::KiCad: return exportKiCad(design, options, diags);
        case ExportFormat::Altium: return exportAltium(design, options);
        case ExportFormat::OrCad: return exportOrCad(design, options);
        case ExportFormat::Allegro: return exportAllegro(design, options);
    }
    return {};
}

std::string exportConstraints(const Design& design) {
    // Every directive on every net, plus the match groups, as JSON. The netlist
    // formats above carry none of this, and spec 15.5 says the sidecar exists
    // precisely for "directives ... where the target cannot carry them".
    std::string out;
    JsonWriter w(out, /*pretty=*/true);
    w.beginObject();
    w.field("version", "1.0");
    w.field("kind", "mantaConstraints");
    w.field("top", design.top);

    w.key("nets");
    w.beginArray();
    for (const Net& net : design.nets) {
        if (net.directives.empty()) continue;
        w.beginObject();
        w.field("name", net.name);
        w.key("directives");
        w.beginObject();
        for (const auto& [name, d] : net.directives) w.field(name, d.value);
        w.endObject();
        w.endObject();
    }
    w.endArray();

    w.key("matches");
    w.beginArray();
    for (const MatchGroup& m : design.matches) {
        w.beginObject();
        w.field("name", m.name);
        w.field("src", m.src);
        w.key("dest");
        w.beginArray();
        for (const std::string& d : m.dest) w.value(d);
        w.endArray();
        w.field("tolerance", m.tolerance);
        w.field("offset", m.offset.empty() ? "0" : m.offset);
        w.endObject();
    }
    w.endArray();

    w.endObject();
    out += '\n';
    return out;
}

}  // namespace manta
