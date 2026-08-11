// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The elaborated design, and the .mantaNets format of spec 15.4.
#pragma once

#include <string>
#include <vector>

#include "ast/ast.h"
#include "base/flat_map.h"
#include "base/intern.h"
#include "sema/registry.h"

namespace manta {

// A user field carried by one pin. These are what user-defined ERC rules read:
// '#' is the open namespace, so a design can be decorated with anything a rule
// needs without the compiler having to know the name in advance.
struct PinAttribute {
    std::string name;
    std::string value;   // rendered, canonical for a dimensioned value
    Dimensioned number;  // parsed, when the value is numeric
    bool numeric = false;
    Strength strength = Strength::Normal;
    Span declaredAt;
};

// A pin as it exists on one instantiated component.
struct ComponentPin {
    std::string physical;  // the package pin: "1", "14"
    std::string logical;   // the declared name: "VCC", "GPIO[1]", "USB.+"
    SymbolId base = SymbolId::kInvalid;    // "GPIO" for "GPIO[1]"
    std::int64_t index = 0;                // element within an array
    bool isArrayElement = false;
    SymbolId member = SymbolId::kInvalid;  // "+" for "USB.+"

    PinType type = PinType::Passive;
    PortDir direction = PortDir::None;
    bool casual = false;
    bool typeExplicit = false;  // '&TYPE' written, rather than defaulted from the arrow
    SymbolId swapGroup = SymbolId::kInvalid;
    bool swapGroupImplicit = false;  // from &CASUAL rather than an explicit &SWAP

    bool hasPinDelay = false;
    Dimensioned pinDelay;

    // Empty for almost every pin, so no allocation in the common case; a linear
    // scan over a handful of entries beats a map at this size.
    std::vector<PinAttribute> attributes;

    [[nodiscard]] const PinAttribute* attribute(std::string_view name) const {
        for (const PinAttribute& a : attributes) {
            if (a.name == name) return &a;
        }
        return nullptr;
    }

    std::uint32_t node = 0;      // union-find handle
    std::int32_t net = -1;       // index into Design::nets, filled after merging
    bool connected = false;      // appeared in a chain or a binding (for W-01)
    bool unbound = false;        // "&NET=?" (spec 11.6)
    std::int32_t declOrder = 0;  // part-declaration order, for the '.' terminal
    Span span;
};

struct Component {
    // The instance path from the top block, already rendered. Keeping the
    // Design free of SymbolIds means the writers need no interner and the
    // emitted order can never depend on interning order (spec 15.8).
    std::vector<std::string> path;
    std::string designator;      // "U1", or empty when unassigned
    std::string identity;        // path-derived identity, used when unassigned
    SymbolId part = SymbolId::kInvalid;
    std::string partName;
    bool fitted = true;
    bool bom = true;
    std::string footprint;
    // What the part is, from '@type'. A first-class member rather than a user
    // field because the compiler interprets the structural roles -- what may go
    // in a cable, what takes part in mating -- and because a system field is
    // excluded from `fields`, which would otherwise drop it from the BOM and
    // hide it from user rules.
    std::string type;
    PartType partType = PartType::BoardPart;
    // Mating (spec 12A). '@mate' names the cable fitted to a board connector;
    // '@mates' names what a cable connector plugs into; '@map' is the pin
    // correspondence, already expanded from its ranges, empty meaning 1:1.
    std::string mate;
    std::vector<std::string> mates;
    std::vector<std::pair<std::int64_t, std::int64_t>> pinMap;
    std::vector<std::pair<std::string, std::string>> fields;  // user fields, source order
    // Schematic sheet section, for 'manta render'. Empty until assigned.
    std::string section;
    // Which sheet edge a connector faces, from '&EDGE' (spec 11.10, revision
    // 1.5). One of LEFT, RIGHT, TOP, BOTTOM; empty when unset.
    std::string edge;
    std::vector<ComponentPin> pins;
    Span span;
};

// One declared port of an instantiated block, resolved to the design-wide net
// it ended up on in that instance. -1 when the port reached no emitted net.
struct BlockPort {
    std::string name;
    PortDir direction = PortDir::None;
    std::int32_t net = -1;
};

// A child block instance, recorded so a renderer can reconstruct the hierarchy
// the flat netlist was elaborated from. The top block is the design itself and
// has no record here.
struct BlockInstance {
    std::vector<std::string> path;  // instance path from the top, e.g. ["BLK1"]
    std::string block;              // definition name, e.g. "indicator"
    std::string section;            // empty until assigned
    std::vector<BlockPort> ports;   // declaration order
    // Every named net local to this instance -- its local spelling, "LED-ANODE"
    // or "NAME[3]" -- mapped to the design net index, sorted by name. This is
    // what lets a child page be labelled with local names rather than
    // parent-flat ones.
    std::vector<std::pair<std::string, std::int32_t>> localNets;
};

struct PinRef {
    std::uint32_t component = 0;
    std::uint32_t pin = 0;
};

struct NetDirective {
    std::string value;
    Strength strength = Strength::Normal;
    Span declaredAt;
    bool fromClass = false;  // came from a netclass rather than the net itself
};

struct Net {
    std::string name;
    std::vector<PinRef> pins;
    // Insertion-ordered: the order directives were applied, which is source
    // order, so the emitted netlist is stable.
    FlatMap<std::string, NetDirective> directives;
    bool ground = false;
    bool stub = false;
    bool global = false;
    // Carries '&HARNESS': the identifier names a bundle type (spec 12.1), not
    // a wire, so the single-reference rule does not apply to it.
    bool harness = false;
    PortDir direction = PortDir::None;
    std::uint32_t references = 0;  // textual occurrences, for E-26
    Span firstSeen;
};

struct MatchMember {
    std::string net;
    Span at;
    // Per-use overrides, "&MATCH={ddr-addr: @!offset=10ps}" (spec 11.4).
    std::string offset;
    std::string tolerance;
};

struct MatchGroup {
    std::string name;
    std::string src;
    std::vector<std::string> dest;
    std::string tolerance;
    std::string offset;
    std::vector<std::string> nested;  // names of contained groups
    std::vector<MatchMember> members;
    Span span;
};

// Spec 13.6 requires "manta annotate --swaps" to reconcile swaps performed
// during layout, but spec 15.5 gives the annotator only a netlist to read them
// from. This optional section is where they travel; see docs/assumptions.md.
struct SwapRecord {
    std::string designator;
    std::string group;
    std::vector<std::string> order;  // permuted member order
};

// A block instance left un-annotated. Spec 13.1 lets an un-annotated design
// elaborate so that 'manta annotate' has a netlist to read; this is what makes
// the leftovers reportable afterwards.
struct UnannotatedBlock {
    std::string identity;  // the label standing in for a designator
    Span span;
};

struct Design {
    std::string top;
    // "block" or "cable". A loom has no ground net and its parts have no
    // footprints, so two ERC rules that are right for a board are wrong for it.
    std::string kind = "block";
    std::vector<Component> components;
    std::vector<Net> nets;
    std::vector<BlockInstance> blocks;  // elaboration order
    std::vector<MatchGroup> matches;
    std::vector<SwapRecord> swaps;
    // Components a '==' run shorted out (spec 6.3), for W-02. Detected during
    // elaboration, where the run is visible; reported by ERC.
    std::vector<std::uint32_t> shorted;
    // Instance path to designator, for "manta link --map".
    std::vector<std::pair<std::string, std::string>> elaborationMap;
    std::vector<UnannotatedBlock> unannotatedBlocks;
};

// Writes .mantaNets in the shape of spec 15.4. Deterministic (spec 15.8).
void writeNetlist(const Design& design, std::string& out);

// Writes a BOM as CSV, for "manta link --bom".
void writeBom(const Design& design, std::string& out);

// Writes the elaboration map, for "manta link --map".
void writeElaborationMap(const Design& design, std::string& out);

// Joins an instance path with '_', the default @FLATFORMAT of spec 13.4.
[[nodiscard]] std::string flattenPath(const std::vector<std::string>& path);

// Applies a @FLATFORMAT template. "$COMPONENT$" is the leaf designator and
// "$INSTANCE$" the enclosing instance path (spec 13.4).
[[nodiscard]] std::string applyFlatFormat(std::string_view templateText,
                                          const std::vector<std::string>& path);

}  // namespace manta
