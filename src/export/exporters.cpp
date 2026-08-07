#include "export/exporters.h"

#include <format>

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

    // The optional swap section (docs/assumptions.md, B2).
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

std::string exportKiCad(const Design& design, std::string_view flatFormat) {
    // KiCad's S-expression netlist. No timestamp or tool-version line is
    // written: spec 15.8 requires byte-identical output for identical input.
    std::string out;
    out += "(export (version \"E\")\n";
    out += "  (design\n";
    out += "    (source ";
    escapeSExpr(out, design.top);
    out += ")\n  )\n";

    out += "  (components\n";
    for (const Component& c : design.components) {
        out += "    (comp (ref ";
        escapeSExpr(out, flatName(c, flatFormat));
        out += ")\n      (value ";
        std::string_view value = c.partName;
        for (const auto& [name, v] : c.fields) {
            if (name == "value") value = v;
        }
        escapeSExpr(out, value);
        out += ")\n      (footprint ";
        escapeSExpr(out, c.footprint);
        out += ")\n";
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
        out += "    )\n";
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
            out += "      (node (ref ";
            escapeSExpr(out, flatName(c, flatFormat));
            out += ") (pin ";
            escapeSExpr(out, c.pins[ref.pin].physical);
            out += "))\n";
        }
        out += "    )\n";
    }
    out += "  )\n)\n";
    return out;
}

std::string exportAltium(const Design& design, std::string_view flatFormat) {
    // Protel/Altium: a '[' block per component, a '(' block per net.
    std::string out;
    for (const Component& c : design.components) {
        std::string_view value = c.partName;
        for (const auto& [name, v] : c.fields) {
            if (name == "value") value = v;
        }
        out += "[\n";
        out += flatName(c, flatFormat);
        out += '\n';
        out += c.footprint;
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
            out += flatName(c, flatFormat);
            out += '-';
            out += c.pins[ref.pin].physical;
            out += '\n';
        }
        out += ")\n";
    }
    return out;
}

std::string exportOrCad(const Design& design, std::string_view flatFormat) {
    // OrCAD PCB II flat netlist.
    std::string out = "( { manta }\n";
    for (const Component& c : design.components) {
        std::string_view value = c.partName;
        for (const auto& [name, v] : c.fields) {
            if (name == "value") value = v;
        }
        out += std::format(" ( {} {} {}\n", c.footprint.empty() ? "UNKNOWN" : c.footprint,
                           flatName(c, flatFormat), value);
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

std::string exportAllegro(const Design& design, std::string_view flatFormat) {
    // Allegro Telesis.
    std::string out = "(Telesis netlist from manta)\n\n$PACKAGES\n";
    for (const Component& c : design.components) {
        out += std::format("!{:<24}{:<24}{}\n", c.footprint.empty() ? "UNKNOWN" : c.footprint,
                           c.partName, flatName(c, flatFormat));
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
            out += std::format("{}.{} ", flatName(c, flatFormat), c.pins[ref.pin].physical);
            ++onLine;
        }
        out += "\n";
    }
    out += "\n$END\n";
    return out;
}

}  // namespace

std::string exportDesign(const Design& design, ExportFormat format, std::string_view flatFormat) {
    switch (format) {
        case ExportFormat::KiCad: return exportKiCad(design, flatFormat);
        case ExportFormat::Altium: return exportAltium(design, flatFormat);
        case ExportFormat::OrCad: return exportOrCad(design, flatFormat);
        case ExportFormat::Allegro: return exportAllegro(design, flatFormat);
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
