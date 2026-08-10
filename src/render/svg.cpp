// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/svg.h"

#include <algorithm>
#include <format>

namespace manta::render {

namespace {

std::string esc(std::string_view text) {
    std::string out;
    xmlEscape(out, text);
    return out;
}

void text(std::string& out, std::string_view cls, int x, int y, std::string_view anchor,
          std::string_view content, std::string_view netAttr = {}) {
    out += std::format("<text class=\"{}\" x=\"{}\" y=\"{}\"", cls, x, y);
    if (!anchor.empty()) out += std::format(" text-anchor=\"{}\"", anchor);
    if (!netAttr.empty()) out += std::format(" data-net=\"{}\"", esc(netAttr));
    out += ">";
    xmlEscape(out, content);
    out += "</text>\n";
}

void line(std::string& out, std::string_view cls, int x1, int y1, int x2, int y2,
          std::string_view netAttr = {}) {
    out += std::format("<line class=\"{}\" x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\"", cls, x1, y1,
                       x2, y2);
    if (!netAttr.empty()) out += std::format(" data-net=\"{}\"", esc(netAttr));
    out += "/>\n";
}

// The three-bar ground glyph on a vertical wire, growing away from (x, y)
// in +dy: horizontal bars, shortening as they go.
void groundGlyph(std::string& out, int x, int y, int dy) {
    line(out, "wire", x - 7, y, x + 7, y);
    line(out, "wire", x - 4, y + 3 * dy, x + 4, y + 3 * dy);
    line(out, "wire", x - 2, y + 6 * dy, x + 2, y + 6 * dy);
}

// The same glyph turned sideways for a horizontal wire, growing in +dx.
void groundGlyphSideways(std::string& out, int x, int y, int dx) {
    line(out, "wire", x, y - 7, x, y + 7);
    line(out, "wire", x + 3 * dx, y - 4, x + 3 * dx, y + 4);
    line(out, "wire", x + 6 * dx, y - 2, x + 6 * dx, y + 2);
}

std::string_view netOf(const RenderModel& m, std::int32_t net) {
    return net < 0 ? std::string_view{} : m.nets[static_cast<std::size_t>(net)].display;
}

// ---------------------------------------------------------------------------
// Display-list items.
// ---------------------------------------------------------------------------

void emitWire(std::string& out, const RenderModel& m, const WireItem& w) {
    std::string_view net = netOf(m, w.net);
    if (w.pts.size() == 4) {
        line(out, "wire", w.pts[0], w.pts[1], w.pts[2], w.pts[3], net);
        return;
    }
    out += "<polyline class=\"wire\" points=\"";
    for (std::size_t i = 0; i + 1 < w.pts.size(); i += 2) {
        if (i) out += ' ';
        out += std::format("{},{}", w.pts[i], w.pts[i + 1]);
    }
    out += "\"";
    if (!net.empty()) out += std::format(" data-net=\"{}\"", esc(net));
    out += "/>\n";
}

void emitPortFlag(std::string& out, const MarkItem& mk, std::string_view name, PortDir dir) {
    int w = kCharWidth * static_cast<int>(name.size()) + 18;
    // The flag body's span and text centre, from the attach point and side.
    int x0 = mk.x, y = mk.y;
    switch (mk.dir) {
        case Side::Left: x0 = mk.x - w; break;
        case Side::Right: x0 = mk.x; break;
        case Side::Top:
            x0 = mk.x - w / 2;
            y = mk.y - 10;
            break;
        case Side::Bottom:
            x0 = mk.x - w / 2;
            y = mk.y + 10;
            break;
    }
    int x1 = x0 + w;
    out += "<polygon class=\"flag\" points=\"";
    auto pt = [&](int px, int py) { out += std::format("{},{} ", px, py); };
    // in: pointed left end; out: pointed right end; anything else: both.
    bool pointL = dir != PortDir::Out;
    bool pointR = dir != PortDir::In;
    if (pointL) {
        pt(x0, y);
        pt(x0 + 6, y - 6);
    } else {
        pt(x0, y - 6);
    }
    if (pointR) {
        pt(x1 - 6, y - 6);
        pt(x1, y);
        pt(x1 - 6, y + 6);
    } else {
        pt(x1, y - 6);
        pt(x1, y + 6);
    }
    if (pointL) pt(x0 + 6, y + 6);
    else pt(x0, y + 6);
    out.pop_back();  // the trailing space
    out += "\"/>\n";
    text(out, "flagtext", (x0 + x1) / 2, y + 3, "middle", name);
    // A vertical stub attaches mid-edge; bridge the gap to the flag body.
    if (mk.dir == Side::Top) line(out, "wire", mk.x, mk.y, mk.x, y + 6);
    if (mk.dir == Side::Bottom) line(out, "wire", mk.x, mk.y, mk.x, y - 6);
}

void emitMark(std::string& out, const RenderModel& m, const MarkItem& mk) {
    std::string_view net = netOf(m, mk.net);
    switch (mk.kind) {
        case MarkKind::Ground:
            out += std::format("<g class=\"gnd\" data-net=\"{}\">\n", esc(net));
            switch (mk.dir) {
                case Side::Bottom: groundGlyph(out, mk.x, mk.y, 1); break;
                case Side::Top: groundGlyph(out, mk.x, mk.y, -1); break;
                case Side::Left: groundGlyphSideways(out, mk.x, mk.y, -1); break;
                case Side::Right: groundGlyphSideways(out, mk.x, mk.y, 1); break;
            }
            out += "</g>\n";
            break;
        case MarkKind::RailFlag:
            out += std::format("<g class=\"rail\" data-net=\"{}\">\n", esc(net));
            switch (mk.dir) {
                case Side::Top:
                    line(out, "wire", mk.x - 6, mk.y, mk.x + 6, mk.y);
                    text(out, "netlabel", mk.x, mk.y - 4, "middle", net);
                    break;
                case Side::Bottom:
                    line(out, "wire", mk.x - 6, mk.y, mk.x + 6, mk.y);
                    text(out, "netlabel", mk.x, mk.y + 10, "middle", net);
                    break;
                case Side::Left:
                    line(out, "wire", mk.x, mk.y - 5, mk.x, mk.y + 5);
                    text(out, "netlabel", mk.x - 4, mk.y + 3, "end", net);
                    break;
                case Side::Right:
                    line(out, "wire", mk.x, mk.y - 5, mk.x, mk.y + 5);
                    text(out, "netlabel", mk.x + 4, mk.y + 3, {}, net);
                    break;
            }
            out += "</g>\n";
            break;
        case MarkKind::Label:
            switch (mk.dir) {
                case Side::Left: text(out, "netlabel", mk.x - 3, mk.y + 3, "end", net, net); break;
                case Side::Right: text(out, "netlabel", mk.x + 3, mk.y + 3, {}, net, net); break;
                case Side::Top: text(out, "netlabel", mk.x, mk.y - 4, "middle", net, net); break;
                case Side::Bottom:
                    text(out, "netlabel", mk.x, mk.y + 10, "middle", net, net);
                    break;
            }
            break;
        case MarkKind::PortFlag: {
            const RenderNet& rn = m.nets[static_cast<std::size_t>(mk.net)];
            out += std::format("<g class=\"portflag\" data-net=\"{}\">\n", esc(net));
            emitPortFlag(out, mk, net, rn.direction);
            out += "</g>\n";
            break;
        }
    }
}

// Classic artwork, rotated point-by-point from body-local to sheet space.
void emitPrims(std::string& out, const PlacedSymbol& s) {
    auto tp = [&](int px, int py, int& ox, int& oy) {
        rotatePoint(s.geom, s.rot, px, py, ox, oy);
        ox += s.x;
        oy += s.y;
    };
    for (const Prim& p : s.geom.prims) {
        switch (p.kind) {
            case Prim::Kind::Line: {
                int x1, y1, x2, y2;
                tp(p.pts[0], p.pts[1], x1, y1);
                tp(p.pts[2], p.pts[3], x2, y2);
                line(out, "glyph", x1, y1, x2, y2);
                break;
            }
            case Prim::Kind::Polyline:
            case Prim::Kind::Polygon: {
                std::string_view tag = p.kind == Prim::Kind::Polygon ? "polygon" : "polyline";
                std::string_view cls = p.fill ? "glyph fill" : "glyph";
                out += std::format("<{} class=\"{}\" points=\"", tag, cls);
                for (std::size_t i = 0; i + 1 < p.pts.size(); i += 2) {
                    if (i) out += ' ';
                    int x, y;
                    tp(p.pts[i], p.pts[i + 1], x, y);
                    out += std::format("{},{}", x, y);
                }
                out += std::format("\"/>\n");
                break;
            }
            case Prim::Kind::Circle: {
                int cx, cy;
                tp(p.pts[0], p.pts[1], cx, cy);
                out += std::format("<circle class=\"{}\" cx=\"{}\" cy=\"{}\" r=\"{}\"/>\n",
                                   p.fill ? "glyph fill" : "glyph", cx, cy, p.pts[2]);
                break;
            }
            case Prim::Kind::Arc: {
                // A rotation preserves handedness, so the sweep flag survives.
                int x1, y1, x2, y2;
                tp(p.pts[0], p.pts[1], x1, y1);
                tp(p.pts[2], p.pts[3], x2, y2);
                out += std::format("<path class=\"glyph\" d=\"M {} {} A {} {} 0 0 {} {} {}\"/>\n",
                                   x1, y1, p.pts[4], p.pts[4], p.pts[5], x2, y2);
                break;
            }
            case Prim::Kind::Text: {
                int x, y;
                tp(p.pts[0], p.pts[1], x, y);
                text(out, "mark", x, y, "middle", p.text);
                break;
            }
        }
    }
}

// Pin numbers and names for a box symbol. Classic symbols draw none: an anode
// is drawn, not named. Boxes never rotate, so the geometry reads directly.
void emitBoxPinText(std::string& out, const PlacedSymbol& s) {
    const int x = s.x, y = s.y, w = s.geom.w, h = s.geom.h;
    for (const SymPin& p : s.geom.pins) {
        std::string_view numCls = p.nc ? "pinnum nc" : "pinnum";
        std::string_view nameCls = p.nc ? "pinname nc" : "pinname";
        switch (p.side) {
            case Side::Left:
                text(out, numCls, x - 2, y + p.offset - 2, "end", p.number);
                text(out, nameCls, x + 3, y + p.offset + 3, {}, p.name);
                break;
            case Side::Right:
                text(out, numCls, x + w + 2, y + p.offset - 2, {}, p.number);
                text(out, nameCls, x + w - 3, y + p.offset + 3, "end", p.name);
                break;
            case Side::Top:
                text(out, numCls, x + p.offset + 2, y - 3, {}, p.number);
                out += std::format(
                    "<text class=\"{}\" transform=\"translate({} {}) rotate(90)\">{}</text>\n",
                    nameCls, x + p.offset, y + 4, esc(p.name));
                break;
            case Side::Bottom:
                text(out, numCls, x + p.offset + 2, y + h + 8, {}, p.number);
                out += std::format(
                    "<text class=\"{}\" transform=\"translate({} {}) rotate(-90)\">{}</text>\n",
                    nameCls, x + p.offset, y + h - 4, esc(p.name));
                break;
        }
    }
}

void emitSymbol(std::string& out, const RenderModel& m, const PlacedSymbol& s) {
    const Component& c = m.design->components[s.component];
    const bool classic = !s.geom.prims.empty();
    out += std::format("<g class=\"sym\" data-c=\"{}\">\n", esc(c.designator));

    std::string partLine = c.partName;
    if (classic) {
        emitPrims(out, s);
        // The value ("10kR") is what a schematic prints under a passive; the
        // part name stays a click away in the info pane.
        for (const auto& [name, value] : c.fields) {
            if (name == "value") partLine = value;
        }
    } else {
        out += std::format("<rect class=\"body\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"/>\n",
                           s.x, s.y, s.geom.w, s.geom.h);
        emitBoxPinText(out, s);
    }
    if (!c.fitted) partLine += " (DNF)";

    if (classic && (s.rot == Rot::R90 || s.rot == Rot::R270)) {
        // A vertical two-terminal part reads like the reference: designator
        // and value stacked to the right of the body.
        int tx = s.x + rotatedW(s.geom, s.rot) + 3;
        int cy = s.y + rotatedH(s.geom, s.rot) / 2;
        text(out, "refdes", tx, cy - 2, {}, c.designator);
        text(out, "partname", tx, cy + 10, {}, partLine);
    } else if (classic) {
        auto anchor = [](std::uint8_t a) -> std::string_view {
            return a == 0 ? "middle" : a == 1 ? "end" : std::string_view{};
        };
        const auto& rd = s.geom.refdesAt;
        const auto& vl = s.geom.valueAt;
        text(out, "refdes", s.x + rd.x, s.y + rd.y, anchor(rd.anchor), c.designator);
        text(out, "partname", s.x + vl.x, s.y + vl.y, anchor(vl.anchor), partLine);
    } else {
        text(out, "refdes", s.x, s.y - 4, {}, c.designator);
        text(out, "partname", s.x, s.y + s.geom.h + 11, {}, partLine);
    }
    out += "</g>\n";
}

void emitRoom(std::string& out, const RoomItem& r) {
    if (!r.framed) return;
    out += std::format("<rect class=\"room\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"/>\n",
                       r.x, r.y, r.w, r.h);
    if (!r.title.empty()) text(out, "roomtitle", r.x + 8, r.y + 13, {}, r.title);
}

// The double border with zone ticks: letters down the sides, numbers across
// the top and bottom, and the title block in the bottom-right corner.
void emitFrame(std::string& out, const SheetLayout& sheet) {
    const int w = sheet.w, h = sheet.h;
    const int o = 3, band = 20, i = o + band;

    out += std::format("<rect class=\"sheet-bg\" x=\"0\" y=\"0\" width=\"{}\" height=\"{}\"/>\n",
                       w, h);
    out += std::format(
        "<rect class=\"frame\" x=\"{0}\" y=\"{0}\" width=\"{1}\" height=\"{2}\"/>\n", o, w - 2 * o,
        h - 2 * o);
    out += std::format(
        "<rect class=\"frame\" x=\"{0}\" y=\"{0}\" width=\"{1}\" height=\"{2}\"/>\n", i, w - 2 * i,
        h - 2 * i);

    // Zone ticks roughly every 250 units, like a real drawing frame.
    int cols = std::clamp(w / 250, 2, 12);
    int rows = std::clamp(h / 250, 2, 10);
    for (int c = 1; c < cols; ++c) {
        int cx = o + (w - 2 * o) * c / cols;
        line(out, "frame", cx, o, cx, i);
        line(out, "frame", cx, h - i, cx, h - o);
    }
    for (int c = 0; c < cols; ++c) {
        int cx = o + (w - 2 * o) * (2 * c + 1) / (2 * cols);
        text(out, "zone", cx, i - 6, "middle", std::format("{}", c + 1));
        text(out, "zone", cx, h - o - 6, "middle", std::format("{}", c + 1));
    }
    for (int r = 1; r < rows; ++r) {
        int cy = o + (h - 2 * o) * r / rows;
        line(out, "frame", o, cy, i, cy);
        line(out, "frame", w - i, cy, w - o, cy);
    }
    for (int r = 0; r < rows; ++r) {
        int cy = o + (h - 2 * o) * (2 * r + 1) / (2 * rows);
        std::string letter(1, static_cast<char>('A' + r));
        text(out, "zone", o + 10, cy + 3, "middle", letter);
        text(out, "zone", w - o - 10, cy + 3, "middle", letter);
    }

    const int tbW = 280, tbH = 54;
    const int tx = w - i - tbW, ty = h - i - tbH;
    out += std::format("<rect class=\"tb\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"/>\n", tx,
                       ty, tbW, tbH);
    line(out, "frame", tx, ty + 18, tx + tbW, ty + 18);
    line(out, "frame", tx, ty + 36, tx + tbW, ty + 36);
    line(out, "frame", tx + 64, ty, tx + 64, ty + tbH);
    text(out, "tbkey", tx + 6, ty + 13, {}, "TITLE");
    text(out, "tbval", tx + 70, ty + 13, {}, sheet.tb.title);
    text(out, "tbkey", tx + 6, ty + 31, {}, "TOOL");
    text(out, "tbval", tx + 70, ty + 31, {}, "manta render");
    text(out, "tbkey", tx + 6, ty + 49, {}, "SHEET");
    text(out, "tbval", tx + 70, ty + 49, {}, sheet.tb.sheet);
}

}  // namespace

void xmlEscape(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
}

void emitSheetSvg(std::string& out, const RenderModel& model, const SheetLayout& sheet) {
    out += std::format(
        "<svg class=\"sheet\" viewBox=\"0 0 {} {}\" xmlns=\"http://www.w3.org/2000/svg\">\n",
        sheet.w, sheet.h);
    emitFrame(out, sheet);
    for (const RoomItem& r : sheet.rooms) emitRoom(out, r);
    for (const RailBarItem& b : sheet.bars) {
        std::string_view net = netOf(model, b.net);
        out += std::format(
            "<line class=\"railbar\" x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" data-net=\"{}\"/>\n",
            b.x1, b.y, b.x2, b.y, esc(net));
        text(out, "netlabel", b.x1, b.y - 4, {}, net, net);
    }
    for (const WireItem& w : sheet.wires) emitWire(out, model, w);
    for (const PlacedSymbol& s : sheet.symbols) emitSymbol(out, model, s);
    for (const DotItem& d : sheet.dots) {
        out += std::format("<circle class=\"dot\" cx=\"{}\" cy=\"{}\" r=\"2\" data-net=\"{}\"/>\n",
                           d.x, d.y, esc(netOf(model, d.net)));
    }
    for (const MarkItem& mk : sheet.marks) emitMark(out, model, mk);
    out += "</svg>\n";
}

}  // namespace manta::render
