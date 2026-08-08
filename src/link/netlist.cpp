#include "link/netlist.h"

#include <algorithm>
#include <format>

#include "json/json.h"
#include "sema/registry.h"

namespace manta {

std::string flattenPath(const std::vector<std::string>& path) {
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i) out += '_';
        out += path[i];
    }
    return out;
}

std::string applyFlatFormat(std::string_view templateText,
                            const std::vector<std::string>& path) {
    if (templateText.empty()) return flattenPath(path);

    // "$COMPONENT$" is the leaf, "$INSTANCE$" everything above it joined by '_'.
    std::string component = path.empty() ? std::string{} : path.back();
    std::string instance;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        if (i) instance += '_';
        instance += path[i];
    }

    std::string out;
    for (std::size_t i = 0; i < templateText.size();) {
        if (templateText[i] == '$') {
            std::size_t close = templateText.find('$', i + 1);
            if (close != std::string_view::npos) {
                std::string_view key = templateText.substr(i + 1, close - i - 1);
                if (key == "COMPONENT") out += component;
                else if (key == "INSTANCE") out += instance;
                else if (key == "PATH") out += flattenPath(path);
                i = close + 1;
                continue;
            }
        }
        out += templateText[i];
        ++i;
    }
    return out;
}

namespace {

// The name a component is known by throughout the netlist and the BOM.
//
// A designator is unique only within its block: instantiate a block twice and
// both copies hold an 'R1'. Spec 13.4 settles what to do about it -- "Export
// flattens the path to the single unique string a BOM and a layout tool
// require" -- and this is the one place that must be obeyed, because a net's
// pins name their component by this string and nothing else. Emitting the bare
// designator leaves a reader unable to tell one 'R1' from another, and every
// pin on the second and third copies is lost.
//
// The local designator is not thrown away: a component entry also carries its
// 'path', whose last element is exactly that.
//
// An un-annotated design "shall compile, link and check completely" (spec 13.1),
// so a component with no designator falls back to the identity derived from its
// instance path.
std::string componentName(const Component& c) {
    if (c.path.size() > 1) return flattenPath(c.path);
    return c.designator.empty() ? c.identity : c.designator;
}

}  // namespace

void writeNetlist(const Design& design, std::string& out) {
    JsonWriter w(out, /*pretty=*/true);
    w.beginObject();
    w.field("version", "1.0");
    w.field("kind", "mantaNets");
    w.field("top", design.top);

    w.key("components");
    w.beginArray();
    for (const Component& c : design.components) {
        w.beginObject();
        w.field("designator", componentName(c));
        w.key("path");
        w.beginArray();
        for (const std::string& p : c.path) w.value(p);
        w.endArray();
        w.field("part", c.partName);
        w.field("fitted", c.fitted);
        w.field("bom", c.bom);
        w.field("footprint", c.footprint);
        w.key("fields");
        w.beginObject();
        for (const auto& [name, value] : c.fields) w.field(name, value);
        w.endObject();
        w.endObject();
    }
    w.endArray();

    w.key("nets");
    w.beginArray();
    for (const Net& n : design.nets) {
        w.beginObject();
        w.field("name", n.name);
        w.key("pins");
        w.beginArray();
        for (const PinRef& p : n.pins) {
            const Component& c = design.components[p.component];
            const ComponentPin& pin = c.pins[p.pin];
            w.beginObject();
            w.field("designator", componentName(c));
            // Spec 15.4's example shows a physical number for one component and
            // a logical name for another; both are emitted so a consumer can
            // take whichever it needs. See docs/assumptions.md.
            w.field("pin", pin.physical);
            w.field("logical", pin.logical);
            // The electrical character of the pin. A layout tool wants it --
            // KiCad puts it on the pad and its DRC reads it -- and it cannot be
            // recovered from the netlist any other way, since the part
            // declaration is not part of the interchange.
            w.field("type", pinTypeName(pin.type));
            w.field("direction", portDirName(pin.direction));
            w.endObject();
        }
        w.endArray();

        w.key("directives");
        w.beginObject();
        for (const auto& [key, d] : n.directives) w.field(key, d.value);
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
        if (!m.nested.empty()) {
            w.key("contains");
            w.beginArray();
            for (const std::string& n : m.nested) w.value(n);
            w.endArray();
        }
        if (!m.members.empty()) {
            w.key("members");
            w.beginArray();
            for (const MatchMember& mem : m.members) {
                w.beginObject();
                w.field("net", mem.net);
                if (!mem.offset.empty()) w.field("offset", mem.offset);
                if (!mem.tolerance.empty()) w.field("tolerance", mem.tolerance);
                w.endObject();
            }
            w.endArray();
        }
        w.endObject();
    }
    w.endArray();

    if (!design.swaps.empty()) {
        w.key("swaps");
        w.beginArray();
        for (const SwapRecord& s : design.swaps) {
            w.beginObject();
            w.field("designator", s.designator);
            w.field("group", s.group);
            w.key("order");
            w.beginArray();
            for (const std::string& m : s.order) w.value(m);
            w.endArray();
            w.endObject();
        }
        w.endArray();
    }

    w.endObject();
    out += '\n';
}

namespace {

void csvField(std::string& out, std::string_view text) {
    bool needsQuotes = text.find_first_of(",\"\n\r") != std::string_view::npos;
    if (!needsQuotes) {
        out += text;
        return;
    }
    out += '"';
    for (char c : text) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
}

}  // namespace

void writeBom(const Design& design, std::string& out) {
    // Collect every distinct user field name, in first-seen order, so the
    // column set is deterministic and stable across runs (spec 15.8).
    std::vector<std::string> columns;
    for (const Component& c : design.components) {
        if (!c.bom) continue;
        for (const auto& [name, value] : c.fields) {
            if (std::find(columns.begin(), columns.end(), name) == columns.end()) {
                columns.push_back(name);
            }
        }
    }

    out += "designator,part,footprint,fitted,quantity";
    for (const std::string& col : columns) {
        out += ',';
        csvField(out, col);
    }
    out += '\n';

    for (const Component& c : design.components) {
        if (!c.bom) continue;
        csvField(out, componentName(c));
        out += ',';
        csvField(out, c.partName);
        out += ',';
        csvField(out, c.footprint);
        out += ',';
        out += c.fitted ? "TRUE" : "FALSE";
        // Spec 9.5: a DNP part is "on the BOM, flagged, quantity zero".
        out += c.fitted ? ",1" : ",0";
        for (const std::string& col : columns) {
            out += ',';
            std::string_view value;
            for (const auto& [name, v] : c.fields) {
                if (name == col) {
                    value = v;
                    break;
                }
            }
            csvField(out, value);
        }
        out += '\n';
    }
}

void writeElaborationMap(const Design& design, std::string& out) {
    for (const auto& [path, designator] : design.elaborationMap) {
        out += path;
        out += '\t';
        out += designator;
        out += '\n';
    }
}

}  // namespace manta
