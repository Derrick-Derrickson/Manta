// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// WP3: the flow graph. A room is read as a directed graph -- sources on the
// left, loads on the right -- and ranked before any coordinate is chosen.
// The vertex classification re-expresses place_classic's roles (anchors,
// verticals, series strings); place_classic itself is transitional and
// nothing here calls into it.
//
// Determinism (spec 15.8): every container is a vector iterated in insertion
// or index order, every tie is broken by an explicit key (designator via
// naturalLess, then index), and barycentre positions are compared by
// cross-multiplied int64 -- no floats, no division, no unordered containers.
#include "render/flow.h"

#include <algorithm>
#include <cstddef>
#include <string>

#include "render/symbols.h"

namespace manta::render {

namespace {

// The gates place_classic applies (anchorKind / twoTerminalKind), restated
// here because this file must outlive the classic placer.
bool anchorKind(SymbolKind k, const Component& c) {
    switch (k) {
        case SymbolKind::Generic:
        case SymbolKind::Connector:
        case SymbolKind::OpAmp:
        case SymbolKind::Nmos:
        case SymbolKind::Pmos:
        case SymbolKind::Mosfet:
        case SymbolKind::Npn:
        case SymbolKind::Pnp: return true;
        default: return c.pins.size() > 4;
    }
}

bool twoTerminalKind(SymbolKind k) {
    switch (k) {
        case SymbolKind::Resistor:
        case SymbolKind::Capacitor:
        case SymbolKind::CapacitorPolarised:
        case SymbolKind::Inductor:
        case SymbolKind::Ferrite:
        case SymbolKind::Diode:
        case SymbolKind::Zener:
        case SymbolKind::Tvs:
        case SymbolKind::Led:
        case SymbolKind::Crystal:
        case SymbolKind::Switch:
        case SymbolKind::Fuse: return true;
        default: return false;
    }
}

// What a room component is to the flow graph, before vertices are formed.
// Ladder/PullUp/PullDown match place_classic's classification exactly.
enum class Role : std::uint8_t { Anchor, Ladder, PullUp, PullDown, Chain, Loose };

// An undirected edge with optional direction evidence. `a < b` always;
// weight sums over every shared net; the first driver->load evidence wins.
struct Edge {
    std::uint32_t a = 0, b = 0;
    int weight = 0;
    int dir = 0;  // 0 undirected, +1 a drives b, -1 b drives a
};

struct Arc {
    std::uint32_t from = 0, to = 0;
};

}  // namespace

RoomFlow buildRoomFlow(const RenderPage& pg, const RenderRoom& room, const RenderModel& m) {
    const Design& d = *m.design;
    const std::size_t nc = d.components.size();

    RoomFlow f;
    f.vertexOf.assign(nc, -1);

    // ------------------------------------------------------------------
    // Net predicates, shared with the classic placer's semantics.
    // ------------------------------------------------------------------
    auto isGnd = [&](std::int32_t net) {
        return net >= 0 && pg.nets[static_cast<std::size_t>(net)].mark == NetMark::Ground;
    };
    auto isRailN = [&](std::int32_t net) {
        return net >= 0 && pg.nets[static_cast<std::size_t>(net)].mark == NetMark::Rail;
    };
    // A private net can carry a drawn series link: exactly two pins, plain
    // label, not crossing, not a port -- classic isPrivate, verbatim.
    auto isPrivate = [&](std::int32_t net) {
        if (net < 0) return false;
        const Net& n = d.nets[static_cast<std::size_t>(net)];
        const RenderNet& rn = pg.nets[static_cast<std::size_t>(net)];
        return n.pins.size() == 2 && rn.mark == NetMark::Label && !rn.crossing &&
               rn.direction == PortDir::None;
    };
    auto otherEnd = [&](std::int32_t net, std::uint32_t comp, std::uint32_t pin) {
        for (const PinRef& pr : d.nets[static_cast<std::size_t>(net)].pins) {
            if (pr.component != comp || pr.pin != pin) return pr;
        }
        return PinRef{comp, pin};
    };

    // ------------------------------------------------------------------
    // 1. Classification, in room component order.
    // ------------------------------------------------------------------
    std::vector<Role> role(nc, Role::Loose);
    std::vector<std::int32_t> sigNet(nc, -1), railNet(nc, -1);
    std::vector<std::uint8_t> consumed(nc, 0), inRoom(nc, 0);

    for (std::uint32_t idx : room.components) {
        inRoom[idx] = 1;
        const Component& c = d.components[idx];
        SymbolKind k = m.kinds[idx];
        if (anchorKind(k, c)) {
            role[idx] = Role::Anchor;
            continue;
        }
        if (!twoTerminalKind(k) || c.pins.size() != 2) {
            role[idx] = Role::Loose;
            continue;
        }
        std::int32_t a = c.pins[0].net;
        std::int32_t b = c.pins[1].net;
        bool railA = isRailN(a), railB = isRailN(b);
        bool gndA = isGnd(a), gndB = isGnd(b);
        if ((railA && gndB) || (railB && gndA)) {
            role[idx] = Role::Ladder;
            railNet[idx] = railA ? a : b;
        } else if (railA != railB && !gndA && !gndB) {
            role[idx] = Role::PullUp;
            railNet[idx] = railA ? a : b;
            sigNet[idx] = railA ? b : a;
        } else if (gndA != gndB && !railA && !railB) {
            role[idx] = Role::PullDown;
            sigNet[idx] = gndA ? b : a;
        } else {
            role[idx] = Role::Chain;
        }
    }

    // ------------------------------------------------------------------
    // 2. Vertex formation, in room component order; children after.
    // ------------------------------------------------------------------
    using VK = FlowVertex::Kind;
    std::vector<Role> vrole;  // parallel to f.verts; Child carries Loose
    auto addVertex = [&](VK kind, Role r, std::vector<std::uint32_t> comps) {
        std::uint32_t vi = static_cast<std::uint32_t>(f.verts.size());
        FlowVertex v;
        v.kind = kind;
        v.comps = std::move(comps);
        if (kind != VK::Child) {
            for (std::uint32_t c : v.comps) f.vertexOf[c] = static_cast<std::int32_t>(vi);
        }
        f.verts.push_back(std::move(v));
        vrole.push_back(r);
        return vi;
    };
    auto chainLinkable = [&](std::uint32_t comp) {
        return inRoom[comp] != 0 && role[comp] == Role::Chain && consumed[comp] == 0;
    };

    for (std::uint32_t idx : room.components) {
        if (consumed[idx]) continue;
        switch (role[idx]) {
            case Role::Anchor: addVertex(VK::Anchor, Role::Anchor, {idx}); break;
            case Role::Ladder:
            case Role::PullUp:
            case Role::PullDown: addVertex(VK::Vertical, role[idx], {idx}); break;
            case Role::Loose: addVertex(VK::Loose, Role::Loose, {idx}); break;
            case Role::Chain: {
                // Walk back to the string's head first, ring-safe, exactly
                // like classic's leftover maximal-path walk.
                std::uint32_t head = idx;
                std::uint32_t headEntry = 0;
                std::vector<std::uint32_t> visited{head};
                while (true) {
                    std::int32_t back = d.components[head].pins[headEntry].net;
                    if (!isPrivate(back)) break;
                    PinRef prev = otherEnd(back, head, headEntry);
                    if (!chainLinkable(prev.component)) break;
                    bool seen = false;
                    for (std::uint32_t v0 : visited) {
                        if (v0 == prev.component) seen = true;
                    }
                    if (seen) break;  // a ring: break it where the scan found it
                    visited.push_back(prev.component);
                    head = prev.component;
                    headEntry = prev.pin == 0 ? 1u : 0u;
                }
                // Then forward, consuming the whole string.
                std::vector<std::uint32_t> comps;
                consumed[head] = 1;
                comps.push_back(head);
                std::uint32_t cur = head;
                std::uint32_t exitPin = headEntry == 0 ? 1u : 0u;
                std::int32_t net = d.components[cur].pins[exitPin].net;
                if (!isGnd(net) && !isRailN(net)) {
                    while (isPrivate(net)) {
                        PinRef nx = otherEnd(net, cur, exitPin);
                        if (!chainLinkable(nx.component)) break;
                        consumed[nx.component] = 1;
                        comps.push_back(nx.component);
                        cur = nx.component;
                        exitPin = nx.pin == 0 ? 1u : 0u;
                        net = d.components[cur].pins[exitPin].net;
                        if (isGnd(net) || isRailN(net)) break;
                    }
                }
                addVertex(VK::Series, Role::Chain, std::move(comps));
                break;
            }
        }
    }
    std::vector<std::uint32_t> childVert;
    childVert.reserve(room.children.size());
    for (std::uint32_t b : room.children) {
        childVert.push_back(addVertex(VK::Child, Role::Loose, {b}));
    }
    const std::uint32_t nv = static_cast<std::uint32_t>(f.verts.size());

    // ------------------------------------------------------------------
    // 3. Room rails and grounds, in first-touch order over room components.
    // ------------------------------------------------------------------
    auto pushUnique = [](std::vector<std::int32_t>& list, std::int32_t net) {
        for (std::int32_t n : list) {
            if (n == net) return;
        }
        list.push_back(net);
    };
    for (std::uint32_t idx : room.components) {
        for (const ComponentPin& p : d.components[idx].pins) {
            if (p.net < 0) continue;
            NetMark mk = pg.nets[static_cast<std::size_t>(p.net)].mark;
            if (mk == NetMark::Rail) pushUnique(f.roomRails, p.net);
            else if (mk == NetMark::Ground) pushUnique(f.roomGrounds, p.net);
        }
    }

    // ------------------------------------------------------------------
    // 4. Edges: vertex-to-vertex through shared nets, built in (net index,
    //    pin declaration) order. Label 3, rail 1, ground and no-connect
    //    excluded entirely; weights sum over multiple shared nets.
    // ------------------------------------------------------------------
    std::vector<Edge> edges;
    auto addEdge = [&](std::uint32_t u, std::uint32_t v, int w, int dir) {
        std::uint32_t a = u, b = v;
        if (a > b) {
            std::swap(a, b);
            dir = -dir;
        }
        for (Edge& e : edges) {
            if (e.a == a && e.b == b) {
                e.weight += w;
                if (e.dir == 0) e.dir = dir;  // first driver evidence wins
                return;
            }
        }
        edges.push_back(Edge{a, b, w, dir});
    };

    struct Touch {
        std::uint32_t v = 0;
        bool out = false, in = false;
    };
    for (std::size_t ni = 0; ni < d.nets.size(); ++ni) {
        NetMark mk = pg.nets[ni].mark;
        if (mk == NetMark::Ground || mk == NetMark::NoConnect) continue;
        const int w = mk == NetMark::Rail ? 1 : 3;

        std::vector<Touch> touches;
        auto touch = [&](std::uint32_t v, PortDir dir) {
            for (Touch& t : touches) {
                if (t.v == v) {
                    t.out = t.out || dir == PortDir::Out;
                    t.in = t.in || dir == PortDir::In;
                    return;
                }
            }
            touches.push_back(Touch{v, dir == PortDir::Out, dir == PortDir::In});
        };
        for (const PinRef& pr : d.nets[ni].pins) {
            if (!inRoom[pr.component]) continue;
            std::int32_t v = f.vertexOf[pr.component];
            if (v < 0) continue;
            touch(static_cast<std::uint32_t>(v),
                  d.components[pr.component].pins[pr.pin].direction);
        }
        for (std::size_t ci = 0; ci < room.children.size(); ++ci) {
            for (const BlockPort& p : d.blocks[room.children[ci]].ports) {
                if (p.net == static_cast<std::int32_t>(ni)) touch(childVert[ci], p.direction);
            }
        }

        for (std::size_t i = 0; i < touches.size(); ++i) {
            for (std::size_t j = i + 1; j < touches.size(); ++j) {
                bool ij = touches[i].out && touches[j].in;
                bool ji = touches[j].out && touches[i].in;
                int dir = 0;
                if (ij && !ji) dir = 1;
                else if (ji && !ij) dir = -1;
                addEdge(touches[i].v, touches[j].v, w, dir);
            }
        }
    }

    std::vector<std::vector<std::uint32_t>> incident(nv);
    for (std::size_t ei = 0; ei < edges.size(); ++ei) {
        incident[edges[ei].a].push_back(static_cast<std::uint32_t>(ei));
        incident[edges[ei].b].push_back(static_cast<std::uint32_t>(ei));
    }

    // ------------------------------------------------------------------
    // 5. Sources and sinks.
    // ------------------------------------------------------------------
    auto vertHasEdge = [&](std::uint32_t v, bool rightBottom) {
        const FlowVertex& fv = f.verts[v];
        if (fv.kind == VK::Child) return false;
        for (std::uint32_t c : fv.comps) {
            const std::string& e = d.components[c].edge;
            if (rightBottom ? (e == "RIGHT" || e == "BOTTOM") : (e == "LEFT" || e == "TOP")) {
                return true;
            }
        }
        return false;
    };
    auto vertTouches = [&](std::uint32_t v, std::int32_t net) {
        const FlowVertex& fv = f.verts[v];
        if (fv.kind == VK::Child) {
            for (const BlockPort& p : d.blocks[fv.comps[0]].ports) {
                if (p.net == net) return true;
            }
            return false;
        }
        for (std::uint32_t c : fv.comps) {
            for (const ComponentPin& p : d.components[c].pins) {
                if (p.net == net) return true;
            }
        }
        return false;
    };
    // Sources are structural, not net-borne: a connector faces in, and an
    // explicit '&EDGE' LEFT/TOP is an order. Touching a room-crossing net
    // proves nothing -- on a real design nearly every part in a room touches
    // one, and admitting them as sources flattened whole rooms into rank 0.
    std::vector<char> isSink(nv, 0), isSrc(nv, 0);
    for (std::uint32_t v = 0; v < nv; ++v) {
        if (vertHasEdge(v, true)) isSink[v] = 1;
    }
    for (std::uint32_t v = 0; v < nv; ++v) {
        if (isSink[v]) continue;
        const FlowVertex& fv = f.verts[v];
        if (fv.kind == VK::Anchor && m.kinds[fv.comps[0]] == SymbolKind::Connector) {
            isSrc[v] = 1;
            continue;
        }
        if (vertHasEdge(v, false)) isSrc[v] = 1;
    }

    bool anySrc = false;
    for (std::uint32_t v = 0; v < nv; ++v) anySrc = anySrc || isSrc[v] != 0;
    if (!anySrc && nv > 0) {
        // The hub anchors its own room: the vertex with the highest total
        // edge weight. Verticals and forced sinks stand aside when anything
        // else is available; ties go to the earlier vertex.
        auto pick = [&](bool allowVertical) {
            std::int32_t best = -1;
            long long bestW = -1;
            for (std::uint32_t v = 0; v < nv; ++v) {
                if (isSink[v]) continue;
                if (!allowVertical && f.verts[v].kind == VK::Vertical) continue;
                long long w = 0;
                for (std::uint32_t ei : incident[v]) w += edges[ei].weight;
                if (w > bestW) {
                    bestW = w;
                    best = static_cast<std::int32_t>(v);
                }
            }
            return best;
        };
        std::int32_t best = pick(false);
        if (best < 0) best = pick(true);
        if (best >= 0) isSrc[static_cast<std::size_t>(best)] = 1;
    }

    // ------------------------------------------------------------------
    // 6. Ranking: longest path from the sources over signal edges
    //    (weight > 1). Vertical vertices hang off the signal path rather
    //    than sitting on it, so their edges are left out here; they take
    //    their rank from the vertex they serve below.
    // ------------------------------------------------------------------
    auto signalEdge = [&](const Edge& e) {
        return e.weight > 1 && f.verts[e.a].kind != VK::Vertical &&
               f.verts[e.b].kind != VK::Vertical;
    };

    // Undirected breadth-first depth from the sources, used only to orient
    // the edges no pin direction spoke for: away from the sources.
    std::vector<int> depth(nv, -1);
    std::vector<std::uint32_t> bfs;
    for (std::uint32_t v = 0; v < nv; ++v) {
        if (isSrc[v]) {
            depth[v] = 0;
            bfs.push_back(v);
        }
    }
    for (std::size_t qi = 0; qi < bfs.size(); ++qi) {
        std::uint32_t u = bfs[qi];
        for (std::uint32_t ei : incident[u]) {
            const Edge& e = edges[ei];
            if (!signalEdge(e)) continue;
            std::uint32_t w = e.a == u ? e.b : e.a;
            if (depth[w] < 0) {
                depth[w] = depth[u] + 1;
                bfs.push_back(w);
            }
        }
    }

    std::vector<Arc> arcs;
    for (const Edge& e : edges) {
        if (!signalEdge(e)) continue;
        std::uint32_t from = e.a, to = e.b;
        if (e.dir > 0) {
            from = e.a;
            to = e.b;
        } else if (e.dir < 0) {
            from = e.b;
            to = e.a;
        } else {
            int da = depth[e.a], db = depth[e.b];
            if (da >= 0 && (db < 0 || da <= db)) {
                from = e.a;
                to = e.b;
            } else if (db >= 0) {
                from = e.b;
                to = e.a;
            }  // both unreachable: keep a -> b, the canonical pair order
        }
        arcs.push_back(Arc{from, to});
    }

    // Deterministic cycle-breaking: arcs in insertion order; an arc whose
    // head already reaches its tail through kept arcs would close a cycle
    // and is dropped.
    std::vector<std::vector<std::uint32_t>> out(nv);
    auto reaches = [&](std::uint32_t from, std::uint32_t target) {
        std::vector<char> seen(nv, 0);
        std::vector<std::uint32_t> stack{from};
        seen[from] = 1;
        while (!stack.empty()) {
            std::uint32_t u = stack.back();
            stack.pop_back();
            if (u == target) return true;
            for (std::uint32_t w : out[u]) {
                if (!seen[w]) {
                    seen[w] = 1;
                    stack.push_back(w);
                }
            }
        }
        return false;
    };
    for (const Arc& a : arcs) {
        if (a.from == a.to) continue;
        if (reaches(a.to, a.from)) continue;  // would close a cycle: dropped
        out[a.from].push_back(a.to);
    }

    // Longest path over the kept DAG, in topological (Kahn) order; the
    // sources stay pinned at rank 0.
    std::vector<int> indeg(nv, 0);
    for (std::uint32_t u = 0; u < nv; ++u) {
        for (std::uint32_t w : out[u]) ++indeg[w];
    }
    std::vector<int> rank(nv, 0);
    std::vector<std::uint32_t> topo;
    topo.reserve(nv);
    for (std::uint32_t v = 0; v < nv; ++v) {
        if (indeg[v] == 0) topo.push_back(v);
    }
    for (std::size_t qi = 0; qi < topo.size(); ++qi) {
        std::uint32_t u = topo[qi];
        for (std::uint32_t w : out[u]) {
            if (!isSrc[w]) rank[w] = std::max(rank[w], rank[u] + 1);
            if (--indeg[w] == 0) topo.push_back(w);
        }
    }

    // Verticals rank next to the vertex they serve: a pull to the anchor on
    // its signal net (any non-vertical on it failing that), a ladder to its
    // rail's first consumer.
    for (std::uint32_t v = 0; v < nv; ++v) {
        if (f.verts[v].kind != VK::Vertical) continue;
        std::uint32_t comp = f.verts[v].comps[0];
        if (vrole[v] == Role::PullUp || vrole[v] == Role::PullDown) {
            std::int32_t sig = sigNet[comp];
            if (sig < 0) continue;
            std::int32_t target = -1;
            for (std::uint32_t w = 0; w < nv && target < 0; ++w) {
                if (w != v && f.verts[w].kind == VK::Anchor && vertTouches(w, sig)) {
                    target = static_cast<std::int32_t>(w);
                }
            }
            for (std::uint32_t w = 0; w < nv && target < 0; ++w) {
                if (w != v && f.verts[w].kind != VK::Vertical && vertTouches(w, sig)) {
                    target = static_cast<std::int32_t>(w);
                }
            }
            if (target >= 0) rank[v] = rank[static_cast<std::size_t>(target)];
        } else if (vrole[v] == Role::Ladder) {
            std::int32_t rail = railNet[comp];
            if (rail < 0) continue;
            for (std::uint32_t w = 0; w < nv; ++w) {
                if (w != v && f.verts[w].kind != VK::Vertical && vertTouches(w, rail)) {
                    rank[v] = rank[w];
                    break;
                }
            }
        }
    }

    // Sinks forced by '&EDGE' land at max rank.
    if (nv > 0) {
        int maxR = 0;
        for (std::uint32_t v = 0; v < nv; ++v) maxR = std::max(maxR, rank[v]);
        for (std::uint32_t v = 0; v < nv; ++v) {
            if (isSink[v]) rank[v] = maxR;
        }
    }

    // Compress to contiguous ranks: an adjustment can empty a middle rank.
    if (nv > 0) {
        std::vector<int> used(rank);
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        for (std::uint32_t v = 0; v < nv; ++v) {
            rank[v] = static_cast<int>(
                std::lower_bound(used.begin(), used.end(), rank[v]) - used.begin());
        }
        f.ranks.assign(used.size(), std::vector<std::uint32_t>{});
    }

    // ------------------------------------------------------------------
    // 7. In-rank order: canonical start (designator order, so a permuted
    //    declaration cannot leak in), then two barycentre sweeps.
    // ------------------------------------------------------------------
    std::vector<std::string> key(nv);
    for (std::uint32_t v = 0; v < nv; ++v) {
        const FlowVertex& fv = f.verts[v];
        if (fv.kind == VK::Child) {
            key[v] = flattenPath(d.blocks[fv.comps[0]].path);
        } else {
            const Component& c = d.components[fv.comps[0]];
            key[v] = c.designator.empty() ? c.identity : c.designator;
        }
    }
    auto keyLess = [&](std::uint32_t x, std::uint32_t y) {
        if (naturalLess(key[x], key[y])) return true;
        if (naturalLess(key[y], key[x])) return false;
        return x < y;
    };
    for (std::uint32_t v = 0; v < nv; ++v) {
        f.ranks[static_cast<std::size_t>(rank[v])].push_back(v);
    }
    for (std::vector<std::uint32_t>& layer : f.ranks) {
        std::sort(layer.begin(), layer.end(), keyLess);
    }

    std::vector<std::int64_t> pos(nv, 0);
    for (const std::vector<std::uint32_t>& layer : f.ranks) {
        for (std::size_t i = 0; i < layer.size(); ++i) {
            pos[layer[i]] = static_cast<std::int64_t>(i);
        }
    }
    auto sweep = [&](bool leftToRight) {
        const int nr = static_cast<int>(f.ranks.size());
        for (int r = leftToRight ? 1 : nr - 2; leftToRight ? r < nr : r >= 0;
             leftToRight ? ++r : --r) {
            std::vector<std::uint32_t>& layer = f.ranks[static_cast<std::size_t>(r)];
            struct K {
                std::int64_t sum = 0, cnt = 0;
                std::uint32_t v = 0;
            };
            std::vector<K> ks;
            ks.reserve(layer.size());
            for (std::size_t i = 0; i < layer.size(); ++i) {
                std::uint32_t v = layer[i];
                K k;
                k.v = v;
                for (std::uint32_t ei : incident[v]) {
                    const Edge& e = edges[ei];
                    std::uint32_t u = e.a == v ? e.b : e.a;
                    bool ahead = leftToRight ? rank[u] < r : rank[u] > r;
                    if (!ahead) continue;
                    k.sum += static_cast<std::int64_t>(e.weight) * pos[u];
                    k.cnt += e.weight;
                }
                if (k.cnt == 0) {  // no neighbours that way: hold position
                    k.sum = static_cast<std::int64_t>(i);
                    k.cnt = 1;
                }
                ks.push_back(k);
            }
            std::sort(ks.begin(), ks.end(), [&](const K& x, const K& y) {
                // Cross-multiplied positions: sumX/cntX vs sumY/cntY without
                // ever dividing.
                std::int64_t lhs = x.sum * y.cnt;
                std::int64_t rhs = y.sum * x.cnt;
                if (lhs != rhs) return lhs < rhs;
                return keyLess(x.v, y.v);
            });
            for (std::size_t i = 0; i < layer.size(); ++i) {
                layer[i] = ks[i].v;
                pos[layer[i]] = static_cast<std::int64_t>(i);
            }
        }
    };
    sweep(true);
    sweep(false);

    for (std::uint32_t v = 0; v < nv; ++v) f.verts[v].rank = rank[v];
    for (const std::vector<std::uint32_t>& layer : f.ranks) {
        for (std::size_t i = 0; i < layer.size(); ++i) {
            f.verts[layer[i]].order = static_cast<int>(i);
        }
    }
    return f;
}

InterRoomFlow buildInterRoomFlow(const RenderPage& page, const RenderModel& m) {
    const Design& d = *m.design;
    const std::size_t nr = page.rooms.size();

    InterRoomFlow f;
    f.counts.assign(nr, std::vector<std::uint32_t>(nr, 0));
    f.pull.assign(nr, 0);

    // counts[a][b]: signal nets touching both rooms, through component pins
    // or child-block ports. Rails and grounds connect everywhere and say
    // nothing about adjacency; a no-connect conducts nothing.
    for (std::size_t ni = 0; ni < d.nets.size(); ++ni) {
        if (page.nets[ni].mark != NetMark::Label) continue;
        const std::int32_t net = static_cast<std::int32_t>(ni);
        std::vector<std::size_t> touched;
        for (std::size_t r = 0; r < nr; ++r) {
            const RenderRoom& room = page.rooms[r];
            bool hit = false;
            for (std::uint32_t idx : room.components) {
                for (const ComponentPin& p : d.components[idx].pins) {
                    if (p.net == net) hit = true;
                }
                if (hit) break;
            }
            for (std::size_t ci = 0; !hit && ci < room.children.size(); ++ci) {
                for (const BlockPort& p : d.blocks[room.children[ci]].ports) {
                    if (p.net == net) hit = true;
                }
            }
            if (hit) touched.push_back(r);
        }
        for (std::size_t i = 0; i < touched.size(); ++i) {
            for (std::size_t j = i + 1; j < touched.size(); ++j) {
                ++f.counts[touched[i]][touched[j]];
                ++f.counts[touched[j]][touched[i]];
            }
        }
    }

    // pull[r]: -1 per LEFT/TOP part, +1 per RIGHT/BOTTOM part, and -1 per
    // connector nobody gave an edge -- a connector defaults to a source
    // pulling towards the reading direction's start.
    for (std::size_t r = 0; r < nr; ++r) {
        for (std::uint32_t idx : page.rooms[r].components) {
            const Component& c = d.components[idx];
            if (c.edge == "LEFT" || c.edge == "TOP") --f.pull[r];
            else if (c.edge == "RIGHT" || c.edge == "BOTTOM") ++f.pull[r];
            else if (c.edge.empty() && m.kinds[idx] == SymbolKind::Connector) --f.pull[r];
        }
    }
    return f;
}

}  // namespace manta::render
