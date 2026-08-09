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
        case SymbolKind::Generic: break;
    }
    return buildGeneric(c);
}

}  // namespace manta::render
