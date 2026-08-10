// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/symbols.h"

#include <algorithm>

namespace manta::render {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

int maxNameLen(const Component& c, const std::vector<std::uint32_t>& idx) {
    int best = 0;
    for (std::uint32_t i : idx) {
        best = std::max(best, static_cast<int>(c.pins[i].logical.size()));
    }
    return best;
}

// Sorts pin indices by natural order of the physical pin id. The netlist reader
// rebuilds Component::pins from the net side, so their stored order carries no
// meaning; the physical id is the one stable, human-expected order.
void naturalSort(const Component& c, std::vector<std::uint32_t>& idx) {
    std::sort(idx.begin(), idx.end(), [&](std::uint32_t a, std::uint32_t b) {
        const std::string& pa = c.pins[a].physical;
        const std::string& pb = c.pins[b].physical;
        if (naturalLess(pa, pb)) return true;
        if (naturalLess(pb, pa)) return false;
        return a < b;
    });
}

void placeColumn(const Component& c, const std::vector<std::uint32_t>& idx, Side side,
                 int start, int pitch, SymbolGeom& out) {
    int at = start;
    for (std::uint32_t i : idx) {
        SymPin p;
        p.number = c.pins[i].physical;
        p.name = c.pins[i].logical;
        p.side = side;
        p.offset = at;
        p.nc = c.pins[i].type == PinType::NC;
        p.pin = i;
        p.net = c.pins[i].net;
        out.pins.push_back(std::move(p));
        at += pitch;
    }
}

SymbolGeom buildGeneric(const Component& c) {
    std::vector<std::uint32_t> left, right, top, bottom, nc;
    for (std::uint32_t i = 0; i < c.pins.size(); ++i) {
        const ComponentPin& p = c.pins[i];
        if (p.type == PinType::NC) nc.push_back(i);
        else if (p.type == PinType::Power) top.push_back(i);
        else if (p.type == PinType::Ground) bottom.push_back(i);
        else if (p.direction == PortDir::Out) right.push_back(i);
        else left.push_back(i);  // In, and everything undirected
    }
    naturalSort(c, left);
    naturalSort(c, right);
    naturalSort(c, top);
    naturalSort(c, bottom);
    naturalSort(c, nc);
    // NC pins sit at the bottom of the right column, greyed.
    right.insert(right.end(), nc.begin(), nc.end());

    SymbolGeom g;
    int nL = static_cast<int>(left.size());
    int nR = static_cast<int>(right.size());
    int nT = static_cast<int>(top.size());
    int nB = static_cast<int>(bottom.size());

    int leftW = kCharWidth * maxNameLen(c, left);
    int rightW = kCharWidth * maxNameLen(c, right);

    // Vertical pins sit at triple pitch so their rail flags' names have room,
    // and start past both the corner text (designator above, part name below)
    // and the left name column, which their rotated names would otherwise
    // run into.
    const int vPitch = 3 * kPinPitch;
    g.topReserve = std::max({3 * kPinPitch,
                             kCharWidth * static_cast<int>(c.designator.size()) + kPinPitch,
                             leftW + 12});
    g.botReserve = std::max({3 * kPinPitch,
                             kCharWidth * static_cast<int>(c.partName.size()) + kPinPitch,
                             leftW + 12});

    int nameW = leftW + rightW + 3 * kPinPitch;
    int topW = nT > 0 ? g.topReserve + vPitch * (nT - 1) + rightW + 15 : 0;
    int botW = nB > 0 ? g.botReserve + vPitch * (nB - 1) + rightW + 15 : 0;
    g.w = std::max({6 * kPinPitch, nameW, topW, botW});
    g.h = kPinPitch * std::max({nL, nR, 1}) + 2 * kPinPitch;

    placeColumn(c, left, Side::Left, kPinPitch, kPinPitch, g);
    placeColumn(c, right, Side::Right, kPinPitch, kPinPitch, g);
    placeColumn(c, top, Side::Top, g.topReserve, vPitch, g);
    placeColumn(c, bottom, Side::Bottom, g.botReserve, vPitch, g);
    return g;
}

// ---------------------------------------------------------------------------
// Classic symbols. Artwork is body-local; terminals sit on the grid.
// ---------------------------------------------------------------------------

void addLine(SymbolGeom& g, int x1, int y1, int x2, int y2) {
    Prim p;
    p.kind = Prim::Kind::Line;
    p.pts = {x1, y1, x2, y2};
    g.prims.push_back(std::move(p));
}

void addPoly(SymbolGeom& g, Prim::Kind kind, std::initializer_list<int> pts, bool fill = false) {
    Prim p;
    p.kind = kind;
    p.pts = pts;
    p.fill = fill;
    g.prims.push_back(std::move(p));
}

void addCircle(SymbolGeom& g, int cx, int cy, int r, bool fill = false) {
    Prim p;
    p.kind = Prim::Kind::Circle;
    p.pts = {cx, cy, r};
    p.fill = fill;
    g.prims.push_back(std::move(p));
}

void addArc(SymbolGeom& g, int x1, int y1, int x2, int y2, int r, int sweep) {
    Prim p;
    p.kind = Prim::Kind::Arc;
    p.pts = {x1, y1, x2, y2, r, sweep};
    g.prims.push_back(std::move(p));
}

void addMark(SymbolGeom& g, int x, int y, std::string text) {
    Prim p;
    p.kind = Prim::Kind::Text;
    p.pts = {x, y};
    p.text = std::move(text);
    g.prims.push_back(std::move(p));
}

void addPin(const Component& c, SymbolGeom& g, std::uint32_t i, Side side, int offset) {
    SymPin p;
    p.number = c.pins[i].physical;
    p.name = c.pins[i].logical;
    p.side = side;
    p.offset = offset;
    p.nc = c.pins[i].type == PinType::NC;
    p.pin = i;
    p.net = c.pins[i].net;
    g.pins.push_back(std::move(p));
}

// The shared skeleton of every two-terminal symbol: a w x h body whose
// terminals are at (0, cy) and (w, cy). `left` picks which pin faces left
// (the anode of a polarised part); -1 means natural pin order.
SymbolGeom twoTerminal(const Component& c, int w, int h, int cy, int left = -1) {
    SymbolGeom g;
    g.w = w;
    g.h = h;
    g.refdesAt = {w / 2, -4, 0};
    g.valueAt = {w / 2, h + 11, 0};
    std::vector<std::uint32_t> idx{0, 1};
    naturalSort(c, idx);
    if (left == static_cast<int>(idx[1])) std::swap(idx[0], idx[1]);
    addPin(c, g, idx[0], Side::Left, cy);
    addPin(c, g, idx[1], Side::Right, cy);
    return g;
}

SymbolGeom buildResistor(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10);
    addPoly(g, Prim::Kind::Polyline,
            {0, 10, 5, 10, 8, 4, 14, 16, 20, 4, 26, 16, 32, 4, 35, 10, 40, 10});
    return g;
}

SymbolGeom buildCapacitor(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10);
    addLine(g, 0, 10, 18, 10);
    addLine(g, 22, 10, 40, 10);
    addLine(g, 18, 3, 18, 17);
    addLine(g, 22, 3, 22, 17);
    return g;
}

SymbolGeom buildCapacitorPolarised(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10, findPinByNames(c, {"+", "A", "ANODE", "POS"}));
    addLine(g, 0, 10, 18, 10);
    addLine(g, 25, 10, 40, 10);
    addLine(g, 18, 3, 18, 17);           // the straight positive plate
    addArc(g, 23, 3, 23, 17, 14, 1);     // the curved plate, concave toward it
    addLine(g, 6, 4, 12, 4);             // '+' by the positive terminal
    addLine(g, 9, 1, 9, 7);
    return g;
}

SymbolGeom buildInductor(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10);
    for (int x = 0; x < 40; x += 10) addArc(g, x, 10, x + 10, 10, 5, 1);
    return g;
}

SymbolGeom buildFerrite(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10);
    addLine(g, 0, 10, 15, 10);
    addLine(g, 25, 10, 40, 10);
    addPoly(g, Prim::Kind::Polygon, {12, 17, 18, 3, 28, 3, 22, 17}, true);
    return g;
}

// dstyle: how the cathode end is drawn.
enum class Cathode : std::uint8_t { Bar, Bent };

void addDiodeBody(SymbolGeom& g, int cy, Cathode style) {
    addLine(g, 0, cy, 15, cy);
    addLine(g, 25, cy, 40, cy);
    addPoly(g, Prim::Kind::Polygon, {15, cy - 6, 15, cy + 6, 25, cy}, true);
    if (style == Cathode::Bar) addLine(g, 25, cy - 6, 25, cy + 6);
    else addPoly(g, Prim::Kind::Polyline, {28, cy - 8, 25, cy - 6, 25, cy + 6, 22, cy + 8});
}

SymbolGeom buildDiode(const Component& c, Cathode style) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10, findPinByNames(c, {"A", "ANODE"}));
    addDiodeBody(g, 10, style);
    return g;
}

SymbolGeom buildTvs(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10, findPinByNames(c, {"A", "ANODE"}));
    addLine(g, 0, 10, 10, 10);
    addLine(g, 30, 10, 40, 10);
    addPoly(g, Prim::Kind::Polygon, {10, 4, 10, 16, 20, 10}, true);
    addPoly(g, Prim::Kind::Polygon, {30, 4, 30, 16, 20, 10}, true);
    addPoly(g, Prim::Kind::Polyline, {23, 2, 20, 4, 20, 16, 17, 18});
    return g;
}

SymbolGeom buildLed(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 30, 20, findPinByNames(c, {"A", "ANODE"}));
    addDiodeBody(g, 20, Cathode::Bar);
    // Two emission arrows, leaving the package toward the upper right.
    addLine(g, 17, 12, 22, 7);
    addPoly(g, Prim::Kind::Polygon, {23, 6, 19, 7, 22, 10}, true);
    addLine(g, 22, 16, 27, 11);
    addPoly(g, Prim::Kind::Polygon, {28, 10, 24, 11, 27, 14}, true);
    return g;
}

SymbolGeom buildCrystal(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10);
    addLine(g, 0, 10, 12, 10);
    addLine(g, 28, 10, 40, 10);
    addLine(g, 12, 4, 12, 16);
    addLine(g, 28, 4, 28, 16);
    addPoly(g, Prim::Kind::Polygon, {16, 2, 24, 2, 24, 18, 16, 18});
    return g;
}

SymbolGeom buildSwitch(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10);
    addLine(g, 0, 10, 13, 10);
    addLine(g, 27, 10, 40, 10);
    addCircle(g, 15, 10, 2);
    addCircle(g, 25, 10, 2);
    addLine(g, 16, 9, 28, 3);  // the lever, resting open
    return g;
}

SymbolGeom buildFuse(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10);
    addLine(g, 0, 10, 40, 10);
    addPoly(g, Prim::Kind::Polygon, {10, 5, 30, 5, 30, 15, 10, 15});
    return g;
}

SymbolGeom buildWire(const Component& c) {
    SymbolGeom g = twoTerminal(c, 40, 20, 10);
    addLine(g, 0, 10, 40, 10);
    return g;
}

SymbolGeom buildCrimp(const Component& c) {
    SymbolGeom g = twoTerminal(c, 10, 20, 10);
    addLine(g, 0, 10, 2, 10);
    addLine(g, 8, 10, 10, 10);
    addCircle(g, 5, 10, 3, true);
    return g;
}

SymbolGeom buildTestPoint(const Component& c) {
    SymbolGeom g;
    g.w = 10;
    g.h = 20;
    g.refdesAt = {5, -4, 0};
    g.valueAt = {5, 31, 0};
    addPin(c, g, 0, Side::Left, 10);
    addLine(g, 0, 10, 1, 10);
    addCircle(g, 5, 10, 4);
    return g;
}

// G left, D top-right, S bottom-right; enhancement-mode channel dashes. The
// arrow (into the channel for N, out of it for P) is omitted when the channel
// is unknown, rather than asserting a polarity nobody stated.
SymbolGeom buildMosfet(const Component& c, SymbolKind kind) {
    SymbolGeom g;
    g.w = 30;
    g.h = 40;
    // The top-left and bottom-left corners are clear: the gate stub sits at
    // mid-height and the drain/source stubs on the right vertical.
    g.refdesAt = {-2, 8, 1};
    g.valueAt = {-2, 38, 1};
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"G", "GATE"})), Side::Left, 20);
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"D", "DRAIN"})), Side::Top, 20);
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"S", "SOURCE"})), Side::Bottom, 20);
    addLine(g, 0, 20, 8, 20);                       // gate lead
    addLine(g, 8, 12, 8, 28);                       // gate bar
    addLine(g, 12, 9, 12, 15);                      // channel, in three dashes
    addLine(g, 12, 17, 12, 23);
    addLine(g, 12, 25, 12, 31);
    addPoly(g, Prim::Kind::Polyline, {12, 12, 20, 12, 20, 0});    // drain
    addPoly(g, Prim::Kind::Polyline, {12, 28, 20, 28, 20, 40});   // source
    addPoly(g, Prim::Kind::Polyline, {12, 20, 20, 20, 20, 28});   // body, tied to source
    if (kind == SymbolKind::Nmos) {
        addPoly(g, Prim::Kind::Polygon, {13, 20, 18, 17, 18, 23}, true);
    } else if (kind == SymbolKind::Pmos) {
        addPoly(g, Prim::Kind::Polygon, {19, 20, 14, 17, 14, 23}, true);
    }
    return g;
}

// B left, C top-right, E bottom-right; the emitter arrow carries the polarity.
SymbolGeom buildBjt(const Component& c, SymbolKind kind) {
    SymbolGeom g;
    g.w = 30;
    g.h = 40;
    g.refdesAt = {-2, 8, 1};
    g.valueAt = {-2, 38, 1};
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"B", "BASE"})), Side::Left, 20);
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"C", "COLLECTOR"})), Side::Top, 20);
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"E", "EMITTER"})), Side::Bottom, 20);
    addLine(g, 0, 20, 12, 20);
    addLine(g, 12, 12, 12, 28);
    addPoly(g, Prim::Kind::Polyline, {12, 16, 20, 8, 20, 0});
    addPoly(g, Prim::Kind::Polyline, {12, 24, 20, 32, 20, 40});
    addCircle(g, 16, 20, 13);
    if (kind == SymbolKind::Npn) addPoly(g, Prim::Kind::Polygon, {18, 30, 16, 25, 13, 28}, true);
    else addPoly(g, Prim::Kind::Polygon, {13, 25, 18, 27, 15, 30}, true);
    return g;
}

SymbolGeom buildOpAmp(const Component& c) {
    SymbolGeom g;
    g.w = 40;
    g.h = 40;
    // Left of the triangle belongs to the input labels and below-left to the
    // V- flag, so the text hugs the free upper-right and lower-right.
    g.refdesAt = {26, -4, 0};
    g.valueAt = {42, 38, 2};
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"IN-", "INN", "-"})), Side::Left,
           10);
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"IN+", "INP", "+"})), Side::Left,
           30);
    addPin(c, g, static_cast<std::uint32_t>(findPinByNames(c, {"OUT", "OUTPUT"})), Side::Right,
           20);
    addPoly(g, Prim::Kind::Polygon, {0, 2, 0, 38, 34, 20});
    addLine(g, 34, 20, 40, 20);
    addMark(g, 6, 13, "-");
    addMark(g, 6, 33, "+");
    if (int vp = findPinByNames(c, {"V+", "VCC", "VDD"}); vp >= 0) {
        addPin(c, g, static_cast<std::uint32_t>(vp), Side::Top, 10);
        addLine(g, 10, 0, 10, 8);  // bridges the stub to the triangle's slope
    }
    if (int vm = findPinByNames(c, {"V-", "VEE", "VSS"}); vm >= 0) {
        addPin(c, g, static_cast<std::uint32_t>(vm), Side::Bottom, 10);
        addLine(g, 10, 40, 10, 32);
    }
    return g;
}

SymbolGeom buildConnector(const Component& c) {
    std::vector<std::uint32_t> rows;
    for (std::uint32_t i = 0; i < c.pins.size(); ++i) rows.push_back(i);
    naturalSort(c, rows);

    SymbolGeom g;
    g.botReserve = std::max(3 * kPinPitch,
                            kCharWidth * static_cast<int>(c.partName.size()) + kPinPitch);
    g.w = std::max(4 * kPinPitch, kCharWidth * maxNameLen(c, rows) + 2 * kPinPitch);
    g.h = kPinPitch * std::max(1, static_cast<int>(rows.size())) + 2 * kPinPitch;
    placeColumn(c, rows, Side::Right, kPinPitch, kPinPitch, g);
    return g;
}

}  // namespace

bool naturalLess(std::string_view a, std::string_view b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            std::size_t si = i, sj = j;
            while (i < a.size() && isDigit(a[i])) ++i;
            while (j < b.size() && isDigit(b[j])) ++j;
            std::string_view na = a.substr(si, i - si);
            std::string_view nb = b.substr(sj, j - sj);
            while (na.size() > 1 && na.front() == '0') na.remove_prefix(1);
            while (nb.size() > 1 && nb.front() == '0') nb.remove_prefix(1);
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
        } else {
            if (a[i] != b[j]) {
                return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
            }
            ++i;
            ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

SymbolGeom buildSymbol(const Component& c, SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Connector: return buildConnector(c);
        case SymbolKind::Resistor: return buildResistor(c);
        case SymbolKind::Capacitor: return buildCapacitor(c);
        case SymbolKind::CapacitorPolarised: return buildCapacitorPolarised(c);
        case SymbolKind::Inductor: return buildInductor(c);
        case SymbolKind::Ferrite: return buildFerrite(c);
        case SymbolKind::Diode: return buildDiode(c, Cathode::Bar);
        case SymbolKind::Zener: return buildDiode(c, Cathode::Bent);
        case SymbolKind::Tvs: return buildTvs(c);
        case SymbolKind::Led: return buildLed(c);
        case SymbolKind::Crystal: return buildCrystal(c);
        case SymbolKind::Nmos:
        case SymbolKind::Pmos:
        case SymbolKind::Mosfet: return buildMosfet(c, kind);
        case SymbolKind::Npn:
        case SymbolKind::Pnp: return buildBjt(c, kind);
        case SymbolKind::OpAmp: return buildOpAmp(c);
        case SymbolKind::Switch: return buildSwitch(c);
        case SymbolKind::Fuse: return buildFuse(c);
        case SymbolKind::TestPoint: return buildTestPoint(c);
        case SymbolKind::Wire: return buildWire(c);
        case SymbolKind::Crimp: return buildCrimp(c);
        case SymbolKind::Generic: break;
    }
    return buildGeneric(c);
}

}  // namespace manta::render
