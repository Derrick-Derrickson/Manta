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

// The same glyph turned sideways for a horizontal wire, growing in +dx, so it
// never intrudes into the pin row below.
void groundGlyphSideways(std::string& out, int x, int y, int dx) {
    line(out, "wire", x, y - 7, x, y + 7);
    line(out, "wire", x + 3 * dx, y - 4, x + 3 * dx, y + 4);
    line(out, "wire", x + 6 * dx, y - 2, x + 6 * dx, y + 2);
}

void emitPin(std::string& out, const RenderModel& m, const PlacedSymbol& s, const SymPin& p) {
    const int x = s.x, y = s.y, w = s.geom.w, h = s.geom.h;
    // A classic symbol is its own pin annotation: an anode is drawn, not named.
    const bool classic = !s.geom.prims.empty();
    std::string_view net;
    NetMark mark = NetMark::Label;
    if (p.net >= 0) {
        const RenderNet& rn = m.nets[static_cast<std::size_t>(p.net)];
        net = rn.display;
        mark = rn.mark;
    }
    std::string_view numCls = p.nc ? "pinnum nc" : "pinnum";
    std::string_view nameCls = p.nc ? "pinname nc" : "pinname";

    switch (p.side) {
        case Side::Left: {
            int py = y + p.offset;
            line(out, "wire", x - kStubLen, py, x, py, net);
            if (!classic) {
                text(out, numCls, x - 2, py - 2, "end", p.number);
                text(out, nameCls, x + 3, py + 3, {}, p.name);
            }
            if (net.empty()) break;
            if (mark == NetMark::Ground) {
                out += std::format("<g class=\"gnd\" data-net=\"{}\">\n", esc(net));
                groundGlyphSideways(out, x - kStubLen, py, -1);
                out += "</g>\n";
            } else if (mark == NetMark::Rail) {
                out += std::format("<g class=\"rail\" data-net=\"{}\">\n", esc(net));
                line(out, "wire", x - kStubLen, py - 5, x - kStubLen, py + 5);
                text(out, "netlabel", x - kStubLen - 4, py + 3, "end", net);
                out += "</g>\n";
            } else {
                text(out, "netlabel", x - kStubLen - 3, py + 3, "end", net, net);
            }
            break;
        }
        case Side::Right: {
            int py = y + p.offset;
            int px = x + w;
            line(out, "wire", px, py, px + kStubLen, py, net);
            if (!classic) {
                text(out, numCls, px + 2, py - 2, {}, p.number);
                text(out, nameCls, px - 3, py + 3, "end", p.name);
            }
            if (net.empty()) break;
            if (mark == NetMark::Ground) {
                out += std::format("<g class=\"gnd\" data-net=\"{}\">\n", esc(net));
                groundGlyphSideways(out, px + kStubLen, py, 1);
                out += "</g>\n";
            } else if (mark == NetMark::Rail) {
                out += std::format("<g class=\"rail\" data-net=\"{}\">\n", esc(net));
                line(out, "wire", px + kStubLen, py - 5, px + kStubLen, py + 5);
                text(out, "netlabel", px + kStubLen + 4, py + 3, {}, net);
                out += "</g>\n";
            } else {
                text(out, "netlabel", px + kStubLen + 3, py + 3, {}, net, net);
            }
            break;
        }
        case Side::Top: {
            int px = x + p.offset;
            line(out, "wire", px, y - kStubLen, px, y, net);
            if (!classic) {
                text(out, numCls, px + 2, y - 3, {}, p.number);
                out += std::format(
                    "<text class=\"{}\" transform=\"translate({} {}) rotate(90)\">{}</text>\n",
                    nameCls, px, y + 4, esc(p.name));
            }
            if (net.empty()) break;
            if (mark == NetMark::Ground) {
                out += std::format("<g class=\"gnd\" data-net=\"{}\">\n", esc(net));
                groundGlyph(out, px, y - kStubLen, -1);
                out += "</g>\n";
            } else if (mark == NetMark::Rail) {
                out += std::format("<g class=\"rail\" data-net=\"{}\">\n", esc(net));
                line(out, "wire", px - 6, y - kStubLen, px + 6, y - kStubLen);
                text(out, "netlabel", px, y - kStubLen - 4, "middle", net);
                out += "</g>\n";
            } else {
                text(out, "netlabel", px, y - kStubLen - 4, "middle", net, net);
            }
            break;
        }
        case Side::Bottom: {
            int px = x + p.offset;
            int by = y + h;
            line(out, "wire", px, by, px, by + kStubLen, net);
            if (!classic) {
                text(out, numCls, px + 2, by + 8, {}, p.number);
                out += std::format(
                    "<text class=\"{}\" transform=\"translate({} {}) rotate(-90)\">{}</text>\n",
                    nameCls, px, by - 4, esc(p.name));
            }
            if (net.empty()) break;
            if (mark == NetMark::Ground) {
                out += std::format("<g class=\"gnd\" data-net=\"{}\">\n", esc(net));
                groundGlyph(out, px, by + kStubLen, 1);
                out += "</g>\n";
            } else if (mark == NetMark::Rail) {
                out += std::format("<g class=\"rail\" data-net=\"{}\">\n", esc(net));
                line(out, "wire", px - 6, by + kStubLen, px + 6, by + kStubLen);
                text(out, "netlabel", px, by + kStubLen + 10, "middle", net);
                out += "</g>\n";
            } else {
                text(out, "netlabel", px, by + kStubLen + 10, "middle", net, net);
            }
            break;
        }
    }
}

// Classic artwork, translated from body-local to sheet coordinates.
void emitPrims(std::string& out, const PlacedSymbol& s) {
    for (const Prim& p : s.geom.prims) {
        switch (p.kind) {
            case Prim::Kind::Line:
                line(out, "glyph", s.x + p.pts[0], s.y + p.pts[1], s.x + p.pts[2],
                     s.y + p.pts[3]);
                break;
            case Prim::Kind::Polyline:
            case Prim::Kind::Polygon: {
                std::string_view tag = p.kind == Prim::Kind::Polygon ? "polygon" : "polyline";
                std::string_view cls = p.fill ? "glyph fill" : "glyph";
                out += std::format("<{} class=\"{}\" points=\"", tag, cls);
                for (std::size_t i = 0; i + 1 < p.pts.size(); i += 2) {
                    if (i) out += ' ';
                    out += std::format("{},{}", s.x + p.pts[i], s.y + p.pts[i + 1]);
                }
                out += std::format("\"/>\n");
                break;
            }
            case Prim::Kind::Circle:
                out += std::format("<circle class=\"{}\" cx=\"{}\" cy=\"{}\" r=\"{}\"/>\n",
                                   p.fill ? "glyph fill" : "glyph", s.x + p.pts[0],
                                   s.y + p.pts[1], p.pts[2]);
                break;
            case Prim::Kind::Arc:
                out += std::format(
                    "<path class=\"glyph\" d=\"M {} {} A {} {} 0 0 {} {} {}\"/>\n",
                    s.x + p.pts[0], s.y + p.pts[1], p.pts[4], p.pts[4], p.pts[5],
                    s.x + p.pts[2], s.y + p.pts[3]);
                break;
            case Prim::Kind::Text:
                text(out, "mark", s.x + p.pts[0], s.y + p.pts[1], "middle", p.text);
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
    }
    if (!c.fitted) partLine += " (DNF)";

    if (classic) {
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
    for (const SymPin& p : s.geom.pins) emitPin(out, m, s, p);
    out += "</g>\n";
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

    int cols = std::clamp(w / 300, 2, 8);
    int rows = std::clamp(h / 300, 2, 6);
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
    for (const PlacedSymbol& s : sheet.symbols) emitSymbol(out, model, s);
    out += "</svg>\n";
}

}  // namespace manta::render
