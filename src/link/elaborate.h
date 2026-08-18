// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// Elaboration: turning declarations into a netlist (spec 15.3).
//
// The linker "elaborates the top-level block, instantiating recursively and
// monomorphising each block against its parameters and connection widths,
// evaluating $...$ against the fields in force at each instantiation".
//
// The central idea is the *bundle*. Every chain element evaluates to an ordered
// vector of node handles for its entry terminal and another for its exit, and
// the connectors of spec 6 are operations on bundles:
//
//   =   union(left.exit, right.entry), then advance
//   ==  union every terminal of every element in the run -- which is why a
//       two-terminal device with '==' on both sides is shorted (spec 6.3)
//   ^   no union at all (spec 6.4)
//   =*  gather an N-wide bundle onto one node
//   *=  broadcast one node across an N-wide bundle
//
// Nodes are merged with a union-find, so the whole netlist build is effectively
// linear in the number of assertions.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "base/arena.h"
#include "base/small_vec.h"
#include "base/union_find.h"
#include "diag/engine.h"
#include "link/fields.h"
#include "link/netlist.h"
#include "link/part_info.h"
#include "link/subst.h"
#include "link/symbols.h"

namespace manta {

// A node handle bundle: one entry per wire.
using Bundle = SmallVec<std::uint32_t, 4>;

// Identifies a named net within one elaborated scope.
struct NetKey {
    std::uint32_t scope = 0;
    SymbolId name = SymbolId::kInvalid;
    std::int64_t index = 0;
    bool indexed = false;

    friend bool operator==(const NetKey&, const NetKey&) = default;
};

}  // namespace manta

template <>
struct manta::DefaultHash<manta::NetKey> {
    std::uint64_t operator()(const manta::NetKey& k) const noexcept {
        return hashCombine(hashCombine(mix64(k.scope), mix64(raw(k.name))),
                           static_cast<std::uint64_t>(k.index) ^ (k.indexed ? 1u : 0u));
    }
};

namespace manta {

// What a node is, for naming and diagnostics once the merge is done.
struct NodeInfo {
    std::string name;       // explicit name, when one was written
    bool explicitName = false;
    std::int32_t component = -1;  // set when the node is a pin
    std::int32_t pin = -1;
    Span firstSeen;
    std::uint32_t references = 0;
    bool global = false;
    PortDir direction = PortDir::None;
};

struct ElaborateOptions {
    Revision toolchain = Revision::toolchain();
    bool runErc = true;
};

class Elaborator {
public:
    Elaborator(SymbolTable& symbols, StringInterner& interner, DiagEngine& diags,
               const std::vector<LinkedObject>& objects, ElaborateOptions options)
        : symbols_(symbols), interner_(interner), diags_(diags), objects_(objects),
          options_(options), subst_(interner, diags) {}

    // Elaborates the named top-level block into a Design.
    [[nodiscard]] Design run(SymbolId topName, Span at);

private:
    // ---- scopes ----------------------------------------------------------
    struct Scope {
        std::uint32_t id = 0;
        const Scope* parent = nullptr;
        std::uint32_t objectIndex = 0;
        std::vector<std::string> path;  // instance path from the top
        std::unique_ptr<FieldEnv> fields;
        // Designator -> component index, for "{U3}" references (spec 7.6).
        FlatMap<SymbolId, std::uint32_t> instances;
        // Harness identifier -> the type assigned to it (spec 12.1).
        FlatMap<SymbolId, SymbolId> harnessTypes;
        std::string flatFormat;
        // The '--- TITLE' render section in force (revision 1.3): empty at
        // body start, set by each marker, copied onto whatever the statements
        // beneath it instantiate. A nested block's own body starts afresh.
        std::string activeSection;
    };

    // ---- element evaluation ----------------------------------------------
    // The result of evaluating one chain element.
    struct ElemValue {
        Bundle entry;
        Bundle exit;
        bool hasEntry = false;
        bool hasExit = false;
        std::int32_t component = -1;  // for W-02 and W-03
        Span span;
    };

    // Widths resolved before nodes are materialised, so that an omitted range
    // can be inferred from the other side of the connection (spec 8.1).
    struct ElemWidth {
        std::int64_t in = -1;   // -1 == not yet known
        std::int64_t out = -1;
        bool fixed = false;     // widths that cannot be inferred away
    };

    // ---- top-level driving ------------------------------------------------
    void elaborateBlock(const Item* block, Scope& scope);
    void elaborateStatement(const Stmt* stmt, Scope& scope);
    void elaborateChain(const Chain* chain, const Stmt* stmt, Scope& scope);
    // Returns the segment's own terminals: the entry of its first element and
    // the exit of its last, which is what a group or replication needs in order
    // to present itself as a single element to the enclosing chain.
    //
    // 'seedLead' prepends a leading element the segment does not itself spell.
    // Spec 7.4: a binding is a chain rooted at a pin, so "PIN <conn> <segment>"
    // is the segment with the pin standing in front of it and 'seedConn' between
    // them. Seeding rather than uniting afterwards is what makes a '==' run span
    // the pin: the run is one run, seen by one pass of spec 6's connectors.
    ElemValue elaborateSegment(const Segment* seg, Scope& scope, std::int64_t expectedIn,
                               std::int64_t expectedOut, std::vector<std::uint32_t>& touched,
                               const ElemValue* seedLead = nullptr,
                               Connector seedConn = Connector::Advance);

    ElemValue evalElement(const Element* el, Scope& scope, std::int64_t expectedIn,
                          std::int64_t expectedOut, std::vector<std::uint32_t>& touched);
    ElemValue evalDevice(const Device* dev, Scope& scope, std::vector<std::uint32_t>& touched);
    ElemValue evalGroup(const Group* g, Scope& scope, std::int64_t expectedIn,
                        std::vector<std::uint32_t>& touched);
    ElemValue evalReplication(const Replication* r, Scope& scope, std::int64_t expectedIn,
                              std::int64_t expectedOut, std::vector<std::uint32_t>& touched);
    ElemValue evalNet(const NetExpr* net, Scope& scope, std::int64_t expectedWidth,
                      std::vector<std::uint32_t>& touched);

    // Resolves "DESIGNATOR.PIN" against the instances in scope, appending the
    // pins it names in declaration order. Returns the component index, or null
    // when the head is not an instance or the tail names no pin of it.
    [[nodiscard]] const std::uint32_t* referencedPins(const NetExpr* net, Scope& scope,
                                                      std::vector<std::uint32_t>& matched);

    // ---- width inference ---------------------------------------------------
    // 'seedWidth' > 0 gives the segment a leading element of that fixed width,
    // occupying widths[0] and joined to the first written element by 'seedConn'.
    // A seeded pin takes part in inference like any other element: a scalar pin
    // makes the chain one wire wide, an N-pin range makes it N (spec 8.1).
    void inferWidths(const Segment* seg, Scope& scope, std::vector<ElemWidth>& widths,
                     std::int64_t seedWidth = -1, Connector seedConn = Connector::Advance);
    [[nodiscard]] std::int64_t staticWidth(const Element* el, Scope& scope);
    [[nodiscard]] std::int64_t terminalWidth(const Terminal& t, const PartInfo* part,
                                             Scope& scope);

    // ---- instantiation ------------------------------------------------------
    // Instantiates a part, producing a Component. Returns its index.
    std::uint32_t instantiatePart(const Instance* inst, const PartInfo& part, Scope& scope,
                                  const FieldEnv& callSiteFields);
    // Instantiates a block, elaborating its body in a child scope. Returns the
    // child scope so the caller can bind its ports.
    std::unique_ptr<Scope> instantiateBlock(const Instance* inst, const Item* block,
                                            Scope& parent);

    // Takes the component by index, not by reference: a binding may carry a
    // chain (spec 7.4) whose devices are instantiated here, and every such
    // instantiation may reallocate components_.
    void applyBindings(const Instance* inst, std::uint32_t componentIndex, Scope& scope,
                       std::vector<std::uint32_t>& touched);
    void applyDefaultNets(Component& component, const PartInfo& part, Scope& scope);

    // ---- nodes ---------------------------------------------------------------
    std::uint32_t netNode(Scope& scope, SymbolId name, std::int64_t index, bool indexed, Span at,
                          bool countReference = true);
    // Joins a '>>' node to the design-wide net of its spelling (spec 10.3).
    void bindGlobal(std::uint32_t node);
    std::uint32_t freshNode(Span at);
    void unite(std::uint32_t a, std::uint32_t b);
    void uniteBundles(const Bundle& a, const Bundle& b, Span at);

    // ---- naming and finishing -------------------------------------------------
    void buildNets(Design& design);
    // Resolves the block instances recorded during elaboration against the
    // root-to-net mapping buildNets leaves behind. Must run after it.
    void collectBlockInstances(Design& design);
    void applyStatementDirectives(const Stmt* stmt, std::span<const std::uint32_t> nodes,
                                  Scope& scope);
    void collectMatchGroups(Design& design);
    void collectNetclasses();

    // ---- helpers ---------------------------------------------------------------
    [[nodiscard]] const PartInfo* partInfoFor(SymbolId name, std::uint32_t objectIndex, Span at);
    [[nodiscard]] std::string netDisplayName(const NetExpr* net, Scope& scope);
    [[nodiscard]] SymbolId resolve(const Name& n, Scope& scope);

    SymbolTable& symbols_;
    StringInterner& interner_;
    DiagEngine& diags_;
    const std::vector<LinkedObject>& objects_;
    ElaborateOptions options_;
    Substituter subst_;
    Arena arena_;

    UnionFind uf_;
    std::vector<NodeInfo> nodeInfo_;
    FlatMap<NetKey, std::uint32_t> netNodes_;
    // Spec 10.3: "a global export is visible design-wide". One representative
    // node per '>>' spelling; every scope's '>>NAME' unites with it, which is
    // what makes the import/export pairing order-independent.
    FlatMap<SymbolId, std::uint32_t> globalNets_;
    // Union-find root -> Design::nets index, filled by buildNets so that data
    // recorded against node handles during elaboration can be resolved after
    // the merge is done.
    FlatMap<std::uint32_t, std::uint32_t> rootToNet_;

    std::vector<Component> components_;
    // Block instances written with '?'. A block is not a component, so it has no
    // Component::designator to be empty, but its label lands in the path of
    // every component beneath it and so in the netlist and the BOM. Collected
    // here and reported with the unassigned devices (E-UNANNOTATED).
    std::vector<UnannotatedBlock> unannotatedBlocks_;
    std::vector<std::uint32_t> shorted_;
    std::uint32_t nextScopeId_ = 1;
    int depth_ = 0;

    // The copy currently being elaborated inside a replication or multiplicity
    // group, so that a '%' per-copy selector knows which element to draw
    // (spec 8.5). -1 when not inside one.
    std::int64_t copyIndex_ = -1;
    std::int64_t copyCount_ = 0;

    // Cached part expansions, keyed by declaration pointer.
    FlatMap<std::uint64_t, std::unique_ptr<PartInfo>> partCache_;

    // Pending per-net data collected during elaboration.
    struct PendingDirective {
        std::uint32_t node;
        std::string name;
        std::string value;
        Strength strength;
        Span at;
        bool fromClass = false;
    };
    std::vector<PendingDirective> pendingDirectives_;

    struct PendingMatchUse {
        std::uint32_t node;
        SymbolId group;
        std::string offset;
        std::string tolerance;
        Span at;
    };
    std::vector<PendingMatchUse> pendingMatches_;

    // A port declaration seen during elaboration: the arrow-carrying net of
    // spec 4.4, recorded where the arrow is applied. Node handles are resolved
    // to net indices after buildNets.
    struct PendingPort {
        std::uint32_t scope;
        std::uint32_t node;
        PortDir dir;
    };
    std::vector<PendingPort> pendingPorts_;

    // A child block instance, recorded as instantiation begins so the order is
    // the order instances are encountered. The ports and local nets belonging
    // to it are found later by scope id.
    struct PendingBlock {
        std::uint32_t scope;
        std::vector<std::string> path;
        std::string block;
        // The section active at the instantiation site, in the parent.
        std::string section;
    };
    std::vector<PendingBlock> pendingBlocks_;

    // Netclass name -> its directives (spec 11.9).
    FlatMap<SymbolId, std::vector<const Directive*>> netclasses_;

    // Identifiers assigned the built-in 'diff' type (spec 12.4), so that a
    // single-ended '&IMP' on one can be caught as E-14.
    FlatSet<SymbolId> diffHarnesses_;

    // Weak field declarations and the set of names anything overrode, for W-06.
    // The check is design-wide, so it can only be settled once every
    // instantiation has been walked.
    struct WeakField {
        SymbolId name = SymbolId::kInvalid;
        FieldNamespace ns = FieldNamespace::User;
        Span declaredAt;
        SymbolId owner = SymbolId::kInvalid;
    };
    FlatMap<std::uint64_t, WeakField> weakFields_;
    FlatSet<FieldKey> overriddenFields_;

    // Diagnostics that ERC needs but only elaboration can see.
    friend class ErcChecker;
};

}  // namespace manta
