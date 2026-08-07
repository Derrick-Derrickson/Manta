// The manta abstract syntax tree.
//
// Every node is arena-allocated and trivially destructible, so child lists are
// std::span over arena storage rather than std::vector. The tree is built once,
// walked many times, and freed wholesale.
//
// Two properties are load-bearing further down the pipeline:
//
//  * Source order is preserved exactly. Block, part, harness and match bodies
//    store one ordered list of BodyEntry rather than separate typed lists,
//    because "manta fmt" must not reorder declarations (spec 17).
//
//  * Every construct the annotator rewrites carries its own byte span. A
//    designator records the exact range of its number so that "manta annotate"
//    can edit it in place without reformatting the file (spec 13.7).
#pragma once

#include <cstdint>
#include <span>

#include "base/intern.h"
#include "lex/dimensioned.h"
#include "source/span.h"

namespace manta {

struct Expr;
struct Value;
struct NetExpr;
struct Stmt;
struct Item;
struct Directive;
struct FieldDecl;
struct Segment;

// ---------------------------------------------------------------------------
// Substitution expressions (spec 14.3)
// ---------------------------------------------------------------------------

enum class ExprKind : std::uint8_t { IntLit, BoolLit, FieldRef, Unary, Binary };

enum class UnOp : std::uint8_t { Neg, Not };

// Precedence order follows the spec 14.3 table, tightest first.
enum class BinOp : std::uint8_t {
    Pow,  // ^   right-associative
    Mul, Div,
    Add, Sub,
    Lt, Gt, Le, Ge,
    Eq, Ne,
    And,  // &
    Xor,  // ~
    Or,   // |
};

[[nodiscard]] std::string_view binOpText(BinOp op) noexcept;

struct Expr {
    ExprKind kind = ExprKind::IntLit;
    Span span;
    std::int64_t intVal = 0;
    bool boolVal = false;
    SymbolId field = SymbolId::kInvalid;  // FieldRef
    UnOp unOp = UnOp::Neg;
    BinOp binOp = BinOp::Add;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
};

// A run of literal text and "$...$" holes. Spec 14.6 notes that a substitution
// "occupies one delimited span on one line", which is what lets diagnostics
// arising after substitution report the original position (spec 15.6).
struct InterpChunk {
    bool isExpr = false;
    SymbolId literal = SymbolId::kInvalid;  // when !isExpr
    Expr* expr = nullptr;                   // when isExpr
    Span span;
};

struct InterpText {
    std::span<InterpChunk> chunks;
    Span span;
};

// A name that may itself contain substitutions: "R?~$res-val$-0603".
struct Name {
    SymbolId symbol = SymbolId::kInvalid;  // set when not interpolated
    InterpText* interp = nullptr;          // set when interpolated
    Span span;

    [[nodiscard]] bool valid() const noexcept {
        return symbol != SymbolId::kInvalid || interp != nullptr;
    }
    [[nodiscard]] bool isInterpolated() const noexcept { return interp != nullptr; }
};

// ---------------------------------------------------------------------------
// Values (spec 3)
// ---------------------------------------------------------------------------

enum class ValueKind : std::uint8_t {
    Integer,
    Decimal,
    Dimensioned,
    Percentage,   // "1%" following a number
    Tolerance,    // "±1%" or "+-1%"
    String,
    Boolean,
    Identifier,
    List,         // "[a, b, c]"
    Repeat,       // "(5ps)*3", the element-wise tolerance form of spec 11.4
    Interp,       // a value that is, or contains, "$...$"
    Version,      // "1.2+", "1.2-", "0.2-1.2" (spec 4.3)
    Unbind,       // "?" in "&NET=?" (spec 11.6)
};

struct VersionConstraint {
    std::uint32_t loMajor = 0, loMinor = 0;
    std::uint32_t hiMajor = 0, hiMinor = 0;
    bool hasLo = false, hasHi = false;
};

struct Value {
    ValueKind kind = ValueKind::Integer;
    Span span;

    Dimensioned num;                        // Integer/Decimal/Dimensioned/Percentage/Tolerance
    SymbolId text = SymbolId::kInvalid;     // String/Identifier (interned, unescaped)
    bool boolean = false;
    bool upperCaseSpelling = false;         // "TRUE" vs "true"; checked by E-34
    std::span<Value*> list;                 // List
    Value* inner = nullptr;                 // Repeat
    std::int64_t count = 0;                 // Repeat
    InterpText* interp = nullptr;           // Interp
    VersionConstraint version;              // Version
};

// ---------------------------------------------------------------------------
// Fields (spec 9) and directives (spec 11)
// ---------------------------------------------------------------------------

enum class FieldNamespace : std::uint8_t { System, User };  // '@' and '#'

// Spec 9.2: the strength ladder, shared by fields and directives.
enum class Strength : std::uint8_t { Weak, Normal, Locked };

// Spec 9.4: a field may be imported from, or exported to, the global scope.
enum class FieldDirection : std::uint8_t { Local, Import, Export };

struct FieldDecl {
    FieldNamespace ns = FieldNamespace::User;
    Strength strength = Strength::Normal;
    FieldDirection direction = FieldDirection::Local;
    Name name;
    Value* value = nullptr;
    Span span;
};

// Spec 11.4: "&MATCH={ddr-addr: @!offset=10ps}".
struct MatchRef {
    Name group;
    std::span<FieldDecl*> overrides;
    Span span;
};

struct Directive {
    Strength strength = Strength::Normal;
    Name name;
    Value* value = nullptr;      // absent for &CASUAL and &STUB
    MatchRef* matchRef = nullptr;
    Span span;
};

// ---------------------------------------------------------------------------
// Ports and net expressions (spec 5, 10)
// ---------------------------------------------------------------------------

enum class PortDir : std::uint8_t { None, In, Out, Bidir };

struct PortSpec {
    PortDir dir = PortDir::None;
    bool global = false;   // ">>": design-wide rather than block-scoped (spec 10.3)
    bool leading = false;  // written before the identifier
    Span span;

    [[nodiscard]] bool present() const noexcept { return dir != PortDir::None; }
};

struct Index {
    bool isExpr = false;
    std::int64_t literal = 0;
    Expr* expr = nullptr;
    Span span;
};

struct Range {
    bool present = false;
    // A single index rather than a range: "GPIO[$n$]" (spec 14.6) and "GPIO[1]"
    // are one wire, whereas "GPIO[1:9]" is nine. Recorded so the formatter can
    // reproduce what was written; lo == hi either way.
    bool single = false;
    Index lo, hi;
    Span span;
};

struct NetExpr {
    PortSpec leading, trailing;
    bool perCopy = false;             // a '%' prefix: the per-copy selector (spec 8.5)
    std::span<Name> path;             // "U1.GPIO1", "i2c.SDA"; one element when simple
    std::span<Name> memberList;       // "USB.[+,-]" (spec 12.2)
    bool hasMemberList = false;
    Range range;
    std::span<Value*> perCopyList;    // "%[GND,-1V,-5V]" (spec 8.5)
    bool hasPerCopyList = false;
    Span span;
};

// ---------------------------------------------------------------------------
// Devices (spec 7)
// ---------------------------------------------------------------------------

struct Terminal {
    bool dot = false;  // the '.' terminal: first unassigned casual pin (spec 7.3)
    Name name;
    // A terminal may name one wire or a range of them. The range is what gives
    // a replicated unit its arity: in "[4[ I{U?~splitter}O[0:1] ]8]" the
    // two-wide exit terminal is what makes the unit one-in two-out.
    Range range;
    Span span;
};

enum class DesignatorKind : std::uint8_t {
    Unassigned,  // "U?"
    Numbered,    // "U7"
    Range,       // "BLK%[1:4,9:10]" (spec 13.3)
};

struct DesigPart {
    std::int64_t lo = 0;
    std::int64_t hi = 0;  // equal to lo for a single number
};

struct Designator {
    Name prefix;
    DesignatorKind kind = DesignatorKind::Unassigned;
    std::int64_t number = 0;
    std::span<DesigPart> parts;
    Span span;
    // The exact byte range of everything after the prefix. "manta annotate"
    // replaces precisely this and nothing else (spec 13.7).
    Span assignmentSpan;
};

enum class BindingKind : std::uint8_t { PinNet, Field, Directive };

struct Binding {
    BindingKind kind = BindingKind::PinNet;
    // PinNet
    bool pinIsDot = false;
    Name pin;
    Range pinRange;
    NetExpr* net = nullptr;
    bool unbind = false;  // "GNDB=?": deliberately floating (spec 11.6)
    // Directives written against the pin rather than the instance, as in
    // "{U5~ddr-chip: DQ[0] &PINDELAY=18ps; }" (spec 11.5). A binding *is* a
    // pin-scoped &NET, so "GND=AGND" and "GND &NET=AGND" are one mechanism
    // (spec 11.6) and both land here.
    std::span<Directive*> pinDirectives;
    // '#' fields written against the pin, which override what the part declared
    // for it: "{U1~mcu: IO[3] #VOH=3V0; }".
    std::span<FieldDecl*> pinFields;
    // Field / Directive
    FieldDecl* field = nullptr;
    Directive* directive = nullptr;
    Span span;
};

struct Instance {
    bool dnp = false;  // the '!' prefix, sugar for @fitted=FALSE (spec 7.5)
    Designator designator;
    bool declares = false;  // '~' present: declares rather than references (spec 7.2)
    Name partOrBlock;
    std::span<Binding*> bindings;
    Span span;
};

struct Device {
    Terminal entry;
    bool hasEntry = false;
    Instance* instance = nullptr;
    Terminal exit;
    bool hasExit = false;
    Span span;
};

// ---------------------------------------------------------------------------
// Chains (spec 6, 8)
// ---------------------------------------------------------------------------

enum class Connector : std::uint8_t {
    Advance,    // '='   connect and advance the node (spec 6.2)
    Same,       // '=='  everything in the run is one net (spec 6.3)
    Gather,     // '=*'  array on the left shorted to one net (spec 6.5)
    Broadcast,  // '*='  one net fanned out to every element (spec 6.5)
};

enum class MultKind : std::uint8_t {
    None,
    Series,    // "+N": N copies in series along the chain
    Parallel,  // "|N": entry terminals common, exit terminals common
    Node,      // "*N": N copies hanging off the current node
};

enum class ElementKind : std::uint8_t { Net, Device, Group, Replication };

struct Group {
    Segment* body = nullptr;
    MultKind mult = MultKind::None;
    std::int64_t count = 0;
    Span multSpan;
    Span span;
};

struct Replication {
    Segment* body = nullptr;
    bool counted = false;  // "[N[ ... ]M]" rather than "[[ ... ]]"
    std::int64_t inWidth = 0;
    std::int64_t outWidth = 0;
    Span span;
};

struct Element {
    ElementKind kind = ElementKind::Net;
    NetExpr* net = nullptr;
    Device* device = nullptr;
    Group* group = nullptr;
    Replication* replication = nullptr;
    Span span;
};

// A run of elements joined by connectors. connectors.size() == elements.size()-1.
struct Segment {
    std::span<Element*> elements;
    std::span<Connector> connectors;
    Span span;
};

// Segments joined by '^', which places elements in one statement without
// connecting them (spec 6.4) while still sharing one directive scope (spec 11.2).
struct Chain {
    std::span<Segment*> segments;
    Span span;
};

// ---------------------------------------------------------------------------
// Statements and declarations (spec 4, 19)
// ---------------------------------------------------------------------------

enum class StmtKind : std::uint8_t {
    Chain,     // covers bare net declarations and port declarations alike
    Field,
    PortList,  // "[3V3, GND]>>;"
};

struct Stmt {
    StmtKind kind = StmtKind::Chain;
    bool isExtern = false;  // spec 6.6
    Chain* chain = nullptr;
    FieldDecl* field = nullptr;
    std::span<NetExpr*> ports;  // PortList
    PortSpec listArrow;
    std::span<Directive*> directives;
    Span span;
};

// A pin map line inside a part (spec 4.5).
struct PinMap {
    std::int64_t physLo = 0;
    std::int64_t physHi = 0;  // equal to physLo for a single pin
    bool physIsRange = false;
    Span physSpan;

    Name logical;
    Range logicalRange;
    std::span<Name> memberList;  // "USB.[+,-]" (spec 12.2)
    bool hasMemberList = false;

    PortSpec arrow;
    std::span<Directive*> directives;
    // '#' fields written on the pin map line. They apply to every pin the line
    // produces, exactly as its directives do, so a 48-pin bus declares a value
    // once rather than 48 times. The '&' namespace stays closed; '#' is already
    // the open one, which is what a user-defined ERC attribute wants.
    std::span<FieldDecl*> fields;
    Span span;
};

// A harness member (spec 12.1).
struct MemberDecl {
    Name name;
    PortSpec arrow;
    std::span<Directive*> directives;
    Span span;
};

enum class ItemKind : std::uint8_t { Block, Part, Harness, Netclass, Match };

// One entry in a declaration body, tagged so that source order survives into
// the formatter unchanged.
enum class BodyKind : std::uint8_t { Item, Stmt, Field, PinMap, Member, Directive };

struct BodyEntry {
    BodyKind kind = BodyKind::Stmt;
    Item* item = nullptr;
    Stmt* stmt = nullptr;
    FieldDecl* field = nullptr;
    PinMap* pin = nullptr;
    MemberDecl* member = nullptr;
    Directive* directive = nullptr;
};

struct Item {
    ItemKind kind = ItemKind::Block;
    bool isStatic = false;  // spec 4.2: internal linkage, exempt from E-30
    Name name;
    std::span<BodyEntry> body;
    Span span;
    Span nameSpan;
};

// The parse result for one source file.
struct SourceUnit {
    std::span<Item*> items;
    FileId file = kNoFile;
};

}  // namespace manta
