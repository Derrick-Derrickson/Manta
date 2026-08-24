# The Manta Schematic Definition Language

**Specification, revision 1.6**

> **Corrected against a reference implementation.**
>
> **1.6 makes the connector state whether the chain moved.** `=` is the
> plain join everywhere — two bare net names included, which retires E-22 —
> and it advances through the far side of the element before it. `==`
> continues on the near side, and is legal exactly when that element has no
> far side: its other pins are spoken for inside its `{}`, so the node is
> where the chain stays (§6.2, §6.3). A connector that disagrees with the
> element before it is error E-49, so every `=` and `==` in a file is forced,
> never stylistic. Deliberate shorts get a written form instead of a chain
> trick: a run of dots (`..{R1}`) or a pin list (`[A,K]{D1}`) attaches that
> many pins of one part to one node (§7.3), and W-02 becomes a connectivity
> check that stays quiet about bridges spelled that way. The membership dot
> generalises: a terminal touches its braces through it (`A.{D1~d}.K`), and a
> binding names this instance's pin with a blank left side (`.VIN = VPOS;`),
> so a pin on the left of a binding is always marked and never misread as a
> net. A pin declaration maps its pad with `:` (`5: SDA<>;`): declarations
> name, references attach, `=` assigns and joins. Sources written for 1.5
> respell a joining `==` as `=`, keep `==` only after dead-end attachments,
> dot their bindings and terminals, and swap pin-declaration `=` for `:`.
>
> **1.5 says what a net and a connector are for the page.** Two directives, one
> revision. `&RAIL` (§11.3) marks a net as a power rail for rendering, whatever
> its name or class: the renderer's rail heuristics remain, and the directive is
> the explicit override for the nets they miss. `&EDGE=LEFT|RIGHT|TOP|BOTTOM`
> (§11.10) declares which sheet edge a connector faces, written bare in the
> instance's binding list — the first directive with instance scope. 1.5 is
> purely additive: both directives were error E-13 under 1.4, so no source that
> compiled before means anything different now. One defect is fixed alongside:
> a directive written bare in a binding list was accepted and silently ignored;
> it is now checked against the instance context, so the forms that used to do
> nothing are the errors they always should have been (E-13).
>
> **1.4 lets a binding carry a chain.** The right-hand side of a binding (§7.4)
> was a single net name; it is now an ordinary segment. A decoupling capacitor, a
> feedback divider or a series resistor can be written at the pin it belongs to
> rather than hoisted into a statement of its own. The rule is an equivalence: a
> pin, a connector and a segment in the binding list of instance `D` mean exactly
> what that connector and segment mean in the enclosing body written after
> `D.PIN`. All four connectors of §6 may open a binding, where only `=` was
> accepted before; §8's grouping and replication are available inside one; and a
> binding is its own directive scope, exactly as the hoisted statement would be.
> `PIN = NET` is the degenerate case, and is unchanged in meaning.
>
> **1.3 puts the design on a page.** A `--- TITLE` line inside a block body
> (§4.7) names a render section: a purely presentational grouping of the
> statements after it, with no effect on connectivity or checking. `manta
> render` (§15.5) draws a netlist as a clickable schematic — one sheet per
> block definition, sections as titled rooms. The netlist carries what a
> renderer needs (§15.4): each component's declared pin list, its section, and
> a `blocks` array recording the instance hierarchy with its ports and local
> net spellings.
>
> **1.2 describes what plugs into a board.** A `cable` (§12A) is a loom: its own
> declaration and its own deliverable, with its own netlist and bill of
> materials. A connector says which loom is fitted to it and what that loom
> plugs into, and the compiler checks that the two fit — including the case
> where a board plugs into another copy of itself. `@type` (§9.7) says what a
> part is, and is what makes a connector, a wire and a crimp distinguishable
> from anything else on the board. Two smaller additions serve those: an area
> unit (§3.2), so a conductor's cross-section is a quantity a rule can check,
> and a range inside a value list (§12A.2), so a twenty-way pin map is one pair
> rather than twenty.
>
> **1.1 added one construct**: the end-of-content marker of §2.8, which lets a
> file carry documentation after its declarations.
>
> Each revision is a superset of the one before. A 1.0 source is a valid 1.5
> source, and a toolchain reads any object whose revision is no newer than its
> own. Every form a revision adds was an error before it — a syntax error for
> 1.4's chain bindings, error E-13 for 1.5's directives — so no construct that
> compiled under an earlier revision has changed meaning.
>
> The remaining changes are editorial.
>
> Two grammar productions in §19 were written more narrowly than the language
> they describe, and are widened here: a terminal may carry a range (§7.3), and
> a bracketed index may select a single wire (§8.1). Four worked examples
> contradicted rules stated elsewhere in this document and have been corrected:
> the op-amp supply pins of §7.6 and §20.7 (which the §2.3 identifier grammar
> cannot produce), the unbraced instantiation of §20.7 (§7.1 requires braces),
> a terminal in §20.7 naming pins §20.3 does not declare, and an unquoted
> hyphenated field name in §20.6 (§14.5 requires quoting). One diagnostic,
> E-UNANNOTATED, has been added to §16.1, and §16.2 now says which warnings are
> enabled by default. No rule has changed meaning.

---

## 1. Introduction

### 1.1 Scope

Manta is a plain-text language for describing electronic schematics. A manta design
describes one printed circuit board: its components, their interconnections, and the
electrical constraints placed on those interconnections.

Manta describes nets and parts. Board construction — stackup, copper weight, layer count,
dielectric, drill sizes, placement and routing — is outside its scope. The test for
whether a property belongs in manta is whether it is a statement about a net or a part.

### 1.2 Normative language

**Shall** marks a requirement. An implementation that does not honour it is
non-conforming; a source file that violates it is in error.

**May** marks a permitted choice.

**Should** marks a recommendation that an implementation is free to decline.

### 1.3 Conformance

A conforming implementation shall:

- accept every construct in this document and reject every construct this document
  declares an error
- implement all diagnostics in §16, reporting the code given there
- produce the artifacts of §15 in the formats given there
- produce byte-identical output for identical input

### 1.4 File extension and encoding

Source files use the extension `.manta`. Files are UTF-8; a byte-order mark is permitted
and ignored. Line endings may be LF or CRLF, and the formatter normalises them to LF.

A file may carry documentation after its declarations, separated by the end-of-content
marker of §2.8. Text after the marker is not manta and is not normalised.

---

## 2. Lexical structure

### 2.1 Character set

Outside comments and string literals, source is ASCII. Every construct in the language is
typable on a standard keyboard.

Comments and string literals may contain any UTF-8 codepoint.

### 2.2 Case sensitivity

Identifiers are case sensitive. `SDA`, `sda` and `Sda` are three distinct identifiers.

```
i2c.SDA = MCU-SDA;      // SDA
i2c.sda = MCU-SDA;      // a different member
```

### 2.3 Identifiers

```ebnf
identifier = [ "-" ] ( letter | "_" | digit ) { letter | digit | "_" | "-" }
                    ( letter | digit | "_" ) ;
letter     = "A".."Z" | "a".."z" ;
digit      = "0".."9" ;
```

A `-` may appear inside an identifier and as its first character. It shall not be the
last character.

```
PWR-EN                  // legal
SW-NODE                 // legal
-5V                     // legal: a negative rail
I2C-SDA                 // legal
VCC-                    // ERROR E-02
```

A leading `-` is resolved by grammatical position. In a net position a token beginning
with `-` is an identifier; in a value position it is a negative numeric literal.

```
BIAS = -5V;             // the net named -5V
#min-supply = -5V;      // the value minus five volts
```

Inside a substitution (§14) `-` is always subtraction; a hyphenated field name is
referenced there by quoting it.

An identifier shall not be a reserved word (§2.6).

Pin names in a part definition follow these rules, and may additionally be an integer or
an integer range (§9.2).

### 2.4 Whitespace

Whitespace separates tokens and is otherwise insignificant. A statement may span any
number of lines. Indentation has no meaning and there is no line-continuation operator.

```
SW = SW-NODE
   = .{L1~MT100UFA}.
   = 3V3;
```

is identical to:

```
SW = SW-NODE = .{L1~MT100UFA}. = 3V3;
```

### 2.5 Comments

```
// a line comment, to end of line

/* a block comment,
   spanning lines */
```

Block comments do not nest: the first `*/` closes the comment. A `//` inside a block
comment is not significant.

A comment may appear wherever whitespace may appear.

```
SW = SW-NODE    // the switching node
   = .{L1~MT100UFA /* 10uH */}.
   = 3V3;
```

### 2.6 Reserved words

```
block   part   harness   netclass   match   cable   static   extern
```

Reserved words are lowercase. `Block` and `PART` are legal identifiers.

System field values and directive values drawn from a fixed set shall be upper case.

```
&TYPE=POWER;            // correct
&TYPE=power;            // ERROR E-34
#status = power;        // legal: a user field value is unconstrained
```

### 2.7 Statement termination

Every statement is terminated by `;`. This includes chain statements, field declarations,
port declarations, directive statements, and the closing brace of every definition.

```
>SIG-IN = I.{U1~AMP012}.O = SIG-OUT>;
#board-rev = C;
>VIN;
block amp { ... };
```

The terminator delimits a statement, and a statement is the scope unit for directives
(§11.2). Splitting one statement into two is a change of meaning; reflowing a statement
across lines is not.

### 2.8 End of content

A line consisting of exactly `---`, outside any declaration, ends the manta content of
the file. Everything after it is documentation: it is never tokenised, and the language
places no constraints on it at all.

```
part STM32F0QA5 {
    @~footprint    = QFP-32;
    #value         = STM32F0QA5;
    #!manufacturer = "ST Microelectronics";

    1: VCC< &TYPE=POWER &~NET=3V3;
    2: GND< &TYPE=POWER &~NET=GND;
};

---

# STM32F0QA5

## Absolute maximum ratings

| Parameter | Min  | Max |
|-----------|------|-----|
| VDD       | -0.3 | 4.0 |
```

This is what lets one file hold a part and its datasheet.

The marker shall begin a line and be followed by nothing but whitespace, and it is
recognised only where a declaration could begin — at the top level, outside every brace.
Inside a `block` body a `---` line is the render section marker of §4.7; inside any other
declaration it is the syntax error it would otherwise be. In neither case is the rest of
the file silently discarded.

Text after the marker is reproduced byte for byte by every tool. `manta fmt` does not
reflow it, reindent it, or normalise its line endings, because it is not manta and may be
anything at all.

---

## 3. Values

### 3.1 Integers

```ebnf
integer = [ "-" ] digit { digit } ;
```

```
#channel-count = 4;
#offset = -2;
```

### 3.2 Dimensioned values

A dimensioned value is a number, an optional SI prefix, and a unit suffix, written with no
intervening space.

| Quantity | Suffix | Example |
|---|---|---|
| Resistance | `R` | `100R`, `4k7R`, `1M5R` |
| Capacitance | `F` | `100nF`, `10uF` |
| Inductance | `H` | `2u2H`, `100nH` |
| Voltage | `V` | `3V3`, `-5V`, `48V` |
| Current | `A` | `750mA`, `3A` |
| Power | `W` | `250mW` |
| Frequency | `Hz` | `100MHz`, `2G4Hz` |
| Length | `m` | `5mm`, `100um` |
| Area | `m2` | `0.35mm2`, `2m2` |
| Time | `s` | `10ns`, `1ms` |
| Temperature | `C` | `85C`, `-40C` |

SI prefixes are `p n u m k M G T`. The prefix `u` denotes micro; `µ` is not accepted.

Where a value has a fractional part, the SI prefix may replace the decimal point. Both
forms are accepted and denote the same value; the substituted form is the canonical
spelling, which compiled artifacts carry (§15.4). The formatter leaves either as
written (§17).

```
4k7R    ≡  4.7kR
3V3     ≡  3.3V
2u2H    ≡  2.2uH
2G4Hz   ≡  2.4GHz
```

A suffix that is itself two characters ending in a digit gains nothing from the
substituted form — `1.5mm2` would become `1m5m2`, which reads as nothing at all
— so a squared unit always writes an explicit point.

`m2` is a length squared, and an SI prefix on it squares with it: `1mm2` is a
square millimetre, `(10⁻³ m)² = 10⁻⁶ m²`, and not a milli-square-metre. The
canonical form therefore steps through prefixes by `10⁶`.

```
1mm2      ≡  0.000001m2
1000mm2   ≡  0.001m2
2m2                            // the leading 'm' is the unit, not a prefix
```

### 3.3 Tolerance

A tolerance is a percentage suffix following a value. `±` may be written `+-`.

```
#value = 10kR;
#tolerance = ±1%;
#tolerance = +-1%;
```

A `%` following a number is a percentage. A `%` leading a token is the per-copy selector
(§9.5). The two never collide.

### 3.4 Metric units

Manta supports metric units only. There is no inch, mil, thou, foot, mile, ounce, pound or
Fahrenheit literal, and a source file containing one is error **E-17**.

Copper-related lengths, where they appear as part or net properties, are written in
metres:

```
#trace-clearance = 200um;
```

Package codes such as `0603`, `0805`, `1206` and `0402` are identifiers naming a footprint
family, not measurements. They are unaffected by this rule.

```
R?~10kR-0603
```

### 3.5 Strings

String literals use double quotes and support `\"`, `\\`, `\n` and `\t`.

```
#manufacturer = "ST Microelectronics";
#note = "second source: \"Acme\" P/N 1234";
```

A bare word is accepted where it contains no whitespace and lexes as an identifier.

```
#footprint = QFP-32;
#footprint = "QFP-32";      // identical
```

### 3.6 Booleans

`true` and `false`, lowercase, in user fields. `TRUE` and `FALSE` in system fields
(§2.6).

```
@fitted = FALSE;
#populate-in-lite = false;
```

### 3.7 Lists

A list is a comma-separated sequence in square brackets.

```
[3V3, GND]>>;
@dest = [U5, U6, U7];
```

---

## 4. Program structure

### 4.1 Files and names

```ebnf
file = { item } ;
item = block_def | part_def | harness_def | netclass_def | match_def | cable_def ;
```

A file has no identity. There is no implicit file-level block, and a filename means
nothing to the language: a `.manta` file is a collection of named declarations. Every
block, part, harness, netclass, match group and cable is declared explicitly and
referenced by that name.

Order of declaration is irrelevant. A name may be used before it is declared.

```
block top {
    {U1~cool_mcu};          // declared below
};

part cool_mcu {
    1: VCC< &TYPE=POWER;
    2: GND< &TYPE=POWER;
};
```

Names are resolved at link across every object supplied to the linker (§15.3). A name
declared twice is error **E-30**. A name referenced but never declared is error **E-31**.

Bare statements appear only inside a block.

### 4.2 Linkage

A declaration may carry a linkage keyword.

| Keyword | Meaning |
|---|---|
| *(none)* | External linkage. Visible to every object at link. |
| `static` | Internal linkage. Visible only within its own object, and exempt from **E-30**. |

```
static part house-resistor-0603 {
    @~footprint = R-0603;
    1: A &CASUAL;
    2: B &CASUAL;
};
```

Two libraries may each declare a `static` part named `house-resistor-0603` without
conflict.

### 4.3 Version constraints

A declaration may state the language revision it requires.

| Form | Meaning |
|---|---|
| `1.2+` | revision 1.2 or later |
| `1.2-` | revision 1.2 or earlier |
| `0.2-1.2` | inclusive range |

```
block power-stage {
    @VERSION = 1.2+;
    ...
};
```

A trailing `-` is an upper bound; a `-` between two revisions is a range. A constraint the
toolchain cannot satisfy is error **E-36**.

### 4.4 Blocks

A block is a reusable subcircuit.

```ebnf
block_def = [ linkage ] "block" identifier
            "{" { item | statement | section_marker } "}" ";" ;
```

```
block rc-filter {
    >IN = .{R?~10kR-0603}. = .{C?~100nF-0603: .=GND} == OUT>;
};
```

#### Block ports

A block's interface is the set of nets in its body carrying a direction arrow. The arrow
is mandatory on a block port, and shall be `<`, `>` or `<>`.

```
block amp-stage {
    >SIG-IN;
    AMPED-SIG>;
    <>i2c;

    SIG-IN = I.{U?~AMP012}.O = AMPED-SIG;
};
```

A port with no arrow is error **E-32**.

The port set determines:

- the legal entry and exit terminals of `{BLK?~name}` (§8.3)
- the block's arity for replication (§9.4)

Rails reach a block by global import (§10.4) rather than as ports.

### 4.5 Parts

A part maps a package's physical pins to named pins and declares the part's fields.

```ebnf
part_def = [ linkage ] "part" identifier "{" { field_decl | pin_map } "}" ";" ;
pin_map  = pin_spec ":" identifier [ arrow ] { directive | field_decl } ";" ;
pin_spec = integer | "[" integer ":" integer "]" ;
```

Each pin line is `physical: logical`, with the direction arrow attached to the logical
name and directives following it. The colon is a *mapping*, not an assignment or a join —
`=` assigns values and connects nets, and a pin declaration does neither. Ranges are
bracketed on both sides and shall be of equal width.

```
part cool-mcu {
    @~footprint    = QFP-STM32-32;
    #value         = STM32F0QA5;
    #!manufacturer = "ST Microelectronics";

    1:       VCC<        &TYPE=POWER &~NET=3V3;
    2:       GND<        &TYPE=POWER &~NET=GND;
    [3:11]:  GPIO[1:9]<>;
    [12:13]: USB.[+,-]<>;
    14:      SDA<>       &TYPE=OPENDRAIN;
    15:      SCL<        &TYPE=OPENDRAIN;
    16:      NC          &TYPE=NC;
};
```

A pin map line may also carry `#` fields, which describe the pin rather than constrain
it. They apply to every pin the line produces, exactly as its directives do, so a wide
bus states a figure once:

```
part MCU-48 {
    [1:48]: IO[1:48]<> #VOH=2V4 #VOL=0V4 #VIH=2V0 #VIL=0V8;
    49:     SDA<>      &TYPE=OPENDRAIN #VOL=0V6;
};
```

The `&` namespace stays closed, because the compiler interprets it. `#` is the open one
(§9.1), and a pin field is exactly what it describes: a name the compiler carries without
interpreting. What reads them is a user-defined rule (see the companion rules
specification), so a design decorated this way compiles and links whether or not any rule
file is present.

Pin fields take the strength ladder of §9.2, and a call site overrides one the same way
it overrides any other field — by declaring a stronger one:

```
{U1~MCU-48: .IO[3] #!VOH=3V0; };
```

They stay on the pin. They are not BOM columns, which are per component.

A part exports all of its pins. There is no separate export declaration: every logical
name is addressable at a call site as a binding target, and as a net reference (§5.2).

### 4.6 Parts and blocks are interchangeable

At a call site a part and a block are instantiated identically. A block's ports are its
pins, and a part may be replaced by a block of the same interface without editing callers.

```
>IN = {BLK?~rc-filter}.OUT = MID>;      // a block
>IN = A.{D?~DI3643}.K       = MID>;      // a part
```

### 4.7 Render sections

A line beginning `---` inside a block body names a **render section**. The statements
after it belong to the section it titles, until the next marker or the end of the block.

```ebnf
section_marker = "---" title ;
```

The marker is line-oriented, exactly as the end-of-content marker of §2.8: the `---`
shall be first on its line, leading whitespace permitted, and `title` is free text
running to the end of it. `//` is not special in a title — everything after the `---`
names the section, so `--- I/O // left` is one title, not a title and a comment.

```
block charger {
    --- POWER IN
    {J1~CONN-USB-C: .VBUS = VBUS;};

    --- REGULATION
    VBUS = VIN.{U1~LDO-3V3}.VOUT = 3V3;
};
```

A bare `---` inside a block is an error — a section marker needs a title — while at the
top level, outside every brace, it remains the end-of-content marker of §2.8, unchanged.
A marker is legal only in a `block` body; in a `part`, `cable`, `harness`, `netclass` or
`match` body it is an error.

A section is purely presentational: it is how `manta render` (§15.5) groups a page into
titled rooms. It changes no connectivity, no check, and no electrical content of the
netlist — two designs differing only in markers link to identical nets. What it does
change is the `section` field (§15.4) that elaboration stamps onto every component and
child block instantiated under it. A nested block's body begins sectionless and keeps
its own markers to itself; its instantiation site takes the enclosing section exactly as
a component does.

`manta fmt` indents a marker line to body depth like any statement (§17); the blank
lines around it are the author's.

---

## 5. Nets and nodes

### 5.1 Definitions

A **node** is a point in a chain. A **net** is the set of pins electrically common to one
another. A bare identifier in a chain names the node at that position.

Nets are global within their block. Two occurrences of one name in one scope are one net.
There is no net declaration statement: a net exists because it is named.

```
SW = SW-NODE = .{L1~MT100UFA}. = VOUT;
VOUT = .{C1~10uF-0805: .=GND};      // the same VOUT
```

### 5.2 Pin references are net names

A node that is not named takes the name of the first pin connected to it, written
`DESIGNATOR.PIN`.

A pin belongs to exactly one net, so `U1.GPIO1` denotes that net whether read as the pin
or as the net at the pin. This holds everywhere, not only for unnamed nodes.

```
{C5~100nF-0603: .=GND}. = U1.GPIO1;
```

Where a net is also named explicitly the two are aliases, and the explicit name is used
for display, netlist output and BOM.

```
U1.GPIO1 = LED-DRIVE;       // the same net, called LED-DRIVE thereafter
```

A harness name shall not collide with a designator, or member access is ambiguous. That is
error **E-21**.

### 5.3 Ground

A net carrying `&TYPE=GROUND` is a ground net. Ground is declared, not inferred, and a
design may declare several.

```
GND  &TYPE=GROUND;
AGND &TYPE=GROUND;
```

A ground net is exempt from the supply check (§16.1, E-27). A design that declares no
ground net is error **E-24**.

Two grounds are tied through an ordinary chain:

```
AGND = .{FB1~BLM18PG}. = DGND;
```

`GND` and `0V` are distinct identifiers and are not special-cased. A design using both has
two nets.

---

## 6. Connections

### 6.1 The chain

A chain statement is a sequence of elements joined by connection operators. Each operator
joins the exit terminal of the element on its left to the entry terminal of the element on
its right.

```
SW = SW-NODE = .{L1~MT100UFA}. = VOUT;
```

### 6.2 `=` — join

`=` joins the exit terminal of the element on its left to the entry terminal of the
element on its right. Through a device the node advances: the node on the far side of the
device is distinct from the node on the near side.

```
A = .{R1~10kR-0603}. = B;      // A and B are different nets, joined through R1
```

Between two bare net names there is no device to advance through, so `=` puts both names
on one node. This is how two nets are tied and how a node acquires a second name (§5.2);
there is no separate aliasing form.

```
SW = SW-NODE = .{L1~MT100UFA}.;      // SW and SW-NODE are one net, feeding L1
U1.GPIO1 = LED-DRIVE;                // the same net, called LED-DRIVE thereafter
TP7 = U3.OUT &STUB;                  // a pin reference (§11.8)
extern U5.1 = GND;                   // a pin reference (§6.6)
USB = MCU-USB;                       // harnesses, assigned member-wise (§12.1)
```

`=` shall follow an element that has a far side to advance through. After an element
whose written pins are all on the near side — a shunt, a one-pin attachment — the chain
has not moved, and saying otherwise is error **E-49**; the truthful spelling is `==`
(§6.3).

*(Revision note: 1.5 and earlier required a device on one side of `=` and reserved
error E-22 for two bare names. 1.6 retires E-22: `=` is the plain join, and `==` is
reserved for the one place a chain genuinely cannot advance.)*

### 6.3 `==` — continue on the node

`==` continues the chain on the near side of the element before it. It is legal exactly
when that element has no far side — a shunt whose other pin is bound inside its `{}`, a
device attached by one pin, a `*N` group hanging on the node — and it is error **E-49**
after anything that passes through. The node does not move across `==`: what follows it
attaches to, or names, the node the chain already stands on.

```
VIN = .{R1~10kR-0603}. = .{C1~100nF-0603: .=GND} == EN;
```

`R1` passes through, so plain joins carry the chain to `C1`'s exposed terminal. `C1`
dead-ends — its far pin went to `GND` inside the braces — so `==` continues on that
node and names it `EN`. `R1`'s far terminal, `C1`'s exposed terminal and `EN` are one
node, and every connector in the line states truthfully whether the chain moved.

The buck idiom reads the same way — attach, stay, stay, advance:

```
SW = K.{D2~DI3643: .A=GND;} == .{C3~100nF-0603: .=BUCK-BST;} == A.{L1~MT100UFA}.B
   = .{C4~10uF-0805: .=GND} == 5V;
```

`D2` attaches to `SW` by its cathode and dead-ends; `C3` rides the same node; `L1`
attaches by `A` and passes through; the plain join advances through `L1` to `C4`; and
the final `==` names the far node `5V`. The inductor is unambiguously in series —
nothing here can short it, because `==` never joins across an element.

A shunt ladder attaches once and continues on the node for each further shunt:

```
3V3 = .{C1~10uF-0805: .=GND} == .{C2~100nF-0603: .=GND} == .{C3~100nF-0603: .=GND};
```

The mismatches are E-49 in both directions, so the spelling is forced, never stylistic:

```
A == B;                          // ERROR E-49: a net passes through; join with '='
A = .{R1~10kR-0603}. == B;       // ERROR E-49: R1 passes through; advance with '='
A = .{C1~100nF-0603: .=GND} = B; // ERROR E-49: C1 dead-ends; continue with '=='
A = .{C1~100nF-0603: .=GND} ==;  // ERROR E-49: '==' continues only into an element
```

There is no way to short a device with connectors. A deliberate bridge is written as a
multi-pin terminal — `..{R1}` or `[A,K]{D1}` (§7.3) — and a bridge that merely *happens*,
two pads of one part reaching one net through separate statements, is warning **W-02**
(§16.2).

### 6.4 `^` — adjacency

`^` places two elements in one statement without connecting them. It creates no net and no
electrical relationship, and exists so that a physically associated group whose internal
relationship is not expressed in this file can be written as one readable chain.

```
SIG-IN = I.{U2~AMP012}  ^  {U3~AMP012}.O = AMPED-SIG;
```

`SIG-IN` connects to `U2`'s `I` pin, and `U3`'s `O` pin connects to `AMPED-SIG`. `U2` and
`U3` are not connected. Where a connection is intended, use `=`.

`^` binds looser than `=` and `==`, partitioning a statement into independent segments
that share one directive scope (§11.2).

### 6.5 `=*` and `*=` — width change

An array-to-scalar or scalar-to-array connection shall use an explicit width-change
operator. The `=` side is the bus; the `*` side is the single net.

`=*` gathers: an array on the left is shorted to one net on the right.

```
SIG-IN[0:3] = [[.{R?~100kR-0603}.]] =* COMBINED-OUT;
```

`*=` broadcasts: one net on the left connects to every element of the array on the right.

```
VREF *= BIAS[0:7];
```

A width mismatch without either operator is error **E-04**.

```
A[0:1] = B;      // ERROR E-04
```

### 6.6 `extern` — cross-object reference

A statement may be prefixed `extern` to declare that the instance it refers to is declared
in another object.

```
extern U5.1 = GND;
```

Pin 1 of `U5` joins `GND`, where `U5` is instantiated elsewhere in the design.

`U5.1` is a legal net name whether or not `U5` exists (§5.2), so `extern` is how an author
states that the absence from this object is deliberate. An `extern` reference to a
designator that no object declares is error **E-31**.

### 6.7 Precedence

From tightest to loosest:

1. terminal attachment — `I.{...}.O`
2. multiplicity — `+N`, `|N`, `*N`
3. grouping — `( ... )`
4. `=`, `==`, `=*`, `*=` — equal precedence, left-associative
5. `^`
6. directives — `&NAME=value`

---

## 7. Devices

### 7.1 Form

```ebnf
device   = [ terminal ] "{" instance "}" [ terminal ] ;
instance = [ "!" ] designator [ "~" identifier ] [ ":" binding { ";" binding } [ ";" ] ] ;
terminal = "." { "." }
         | "[" identifier { "," identifier } "]"
         | identifier [ "[" range "]" ] ;
```

### 7.2 Declaration and reference

The presence of `~` distinguishes declaring a new instance from referencing an existing
one.

```
I.{U?~AMP012: .PWR=PWR-SWITCHED}.O      // declares a new AMP012
I.{U3}.O                                // references existing U3
```

Referencing a designator that is never declared is error **E-07**.

### 7.3 Terminals

The terminals written outside the braces are the pins through which the chain passes: the
left is the entry, the right the exit. A terminal always touches its braces through the
membership dot — the same dot as `U5.EN` — pointing from the pin to its instance: entry
pins are written `PIN.{...}`, exit pins `{...}.PIN`. A bare dot is the casual-pin
terminal, its own attachment.

```
A.{D1~DI3643}.K                          // diode: anode in, cathode out
S.{Q1~FFET123}.D                         // FET: source in, drain out
I.{U5~AMP012}.O                          // amplifier: input in, output out
.{R1~10kR-0603}.                         // resistor: either pin
Down.{U9~USB-ISO}.Up                     // any pin name may be a terminal
{BLK1~rc-filter}.OUT                     // a block: its ports are its pins
```

The entry terminal is omitted where the device begins a statement. The exit terminal is
omitted where the device ends a statement, or where the device is a shunt.

A terminal name shall be a declared pin of the instance's part, or a declared port of its
block, or `.`.

A pin used as a terminal shall not also appear in the binding list. That is error
**E-08**.

```
A.{D1~DI3643: .A=VIN}.K      // ERROR E-08
```

#### The `.` terminal

`.` means: take the first unassigned pin from those remaining. Resolution order is fixed.

1. Bindings consume their named pins.
2. Explicitly named terminals consume theirs.
3. Each `.`, left to right, takes the next unconsumed pin in part-declaration order.

`.` is legal only on pins carrying `&CASUAL` (§11.7). Otherwise it is error **E-23**.

```
part resistor-0603 {
    1: A &CASUAL;
    2: B &CASUAL;
};

.{R1~resistor-0603}.       // legal
.{D1~DI3643}.              // ERROR E-23: a diode's pins are not casual
```

#### Multi-pin terminals

A run of dots takes that many next-unassigned casual pins — resolution order as above,
each dot in turn — all onto the **one** node the terminal stands on. A pin list does the
same by name: every listed pin joins the node. Either is one wire wide however many pins
it consumes, which is what distinguishes a list from a range — `[A,K]` is two pins on
one net, where `O[0:1]` is two wires of a bus.

```
X = Y = ..{R1~10kR-0603};        // both pads of R1 on the X node: a written short
X = Y = [A,K].{D1~DI3643};        // the same by name, for pins that are not casual
VIN = [1,2].{J5~PWR-CONN}.[3,4] = GND;   // paralleled connector pins, two per side
```

Writing two or more pins of one part onto one node is the explicit spelling of a
bridge, so **W-02** stays quiet about a short declared this way (§16.2). A multi-pin
terminal consumes every pin it takes on the near side, so an element attached through
one and writing no exit dead-ends, and the chain continues with `==` (§6.3).

### 7.4 Bindings

`:` introduces a binding list and `;` separates bindings. A trailing `;` before `}` is
permitted, and preferred in new code.

```
S.{Q1~FFET123: .G=nPWR-EN; }.D
I.{U5~AMP012: .EN=AMP-EN; .PWR=PWR-SWITCHED; .GND=GND; }.O
```

**A binding is a chain rooted at a pin of the enclosing instance.** A binding is written
as a leading `.`, a pin of the instance, a connector, and a segment. The dot is the same
membership dot as `U5.EN`, with the blank left side meaning *this instance* — so a pin on
the left of a binding is always marked, and `.GND = GND;` cannot be misread as a chain
joining two nets. The bare `.` binding (§7.3's casual pin) is the degenerate case: this
instance's next unassigned casual pin. A binding means exactly what its connector and
that segment mean in a statement of the enclosing body whose leading element is a reference
to the pin: the pin takes the place of that leading element, and everything after the
connector is an ordinary segment (§19), with all of §6's connectors and all of §8's
grouping and replication available.

```
VBAT = VIN.{U5~ldo: .GND=GND; .EN = .{R7~100kR-0603}. = VBAT; }.VOUT = 3V3;
```

The `EN` binding is the second of these two statements, written where the pin is:

```
VBAT = VIN.{U5~ldo: .GND=GND; }.VOUT = 3V3;
U5.EN = .{R7~100kR-0603}. = VBAT;
```

`.PIN = NET` is the simplest case of the same rule, where the segment is a single net
element. It joins the pin and the net onto one node, as any `=` does (§6.2).

A binding opens with `=`, `=*` or `*=` — each with its
usual meaning. `^` (§6.4) does not appear in a binding: a binding is one segment, not a
chain of `^`-separated segments. Where two unconnected things belong to one pin, write two
bindings or a separate statement.

```
{U2~buck-3a:
    .VIN  = VPOS = .{C1~10uF-0805: .=GND; };
    .FB   = .{R3~51kR-0603: .=5V; } == .{R4~10kR-0603: .=GND; };
    .COMP = .{C2~1nF-0603}. = GND;
};
```

`VIN` sits on `VPOS`, which carries `C1` to ground. `FB` is one node with the junction of
`R3` and `R4`. `COMP` reaches ground through `C2`.

A binding forms its own scope, exactly as the equivalent hoisted statement does. The nets
of a binding's chain are not part of the enclosing chain and are not covered by the
directives of the statement that contains the instance (§11.2); directives written inside a
binding do not reach the enclosing chain either.

A segment ends at the first token that is not a connector or an element, so a `&`, `#` or
`@` item written after a binding's right-hand side is never part of its chain. Such an item
attaches to the pin, as it always has (§11.5, §11.6), and a pin may carry one with no
right-hand side at all.

```
{U1~ddr-chip: .DQ[0] = MEM-D0 &PINDELAY=18ps; .DQ[1] &PINDELAY=18ps; }
```

`&PINDELAY` attaches to `DQ[0]`, not to `MEM-D0`.

A binding whose right-hand side is a single net is a pin-scoped `&NET` (§11.6); `.GND=AGND`
and `.GND &NET=AGND` are one mechanism. `.PIN = ?` unbinds a pin (§11.6) and is written only
with `=`.

A device declared inside a binding is an ordinary instance of the enclosing body: it takes
that body's active render section (§4.7) and is annotated with everything else (§13).

A pin used as a terminal shall not also appear in the binding list (**E-08**, §7.3),
whether that binding carries a single net or a chain.

A device with no bindings is written bare.

```
.{L1~MT100UFA}.        // correct
.{L1~MT100UFA:}.       // ERROR E-09
.{L1~MT100UFA;}.       // ERROR E-09
```

### 7.5 Do not populate

A `!` prefix on the designator marks the instance as not fitted. It is exact sugar for
`@fitted=FALSE`.

```
!U?~AMP012
!BLK?~audio-stage      // cascades recursively to every part within
```

DNP affects BOM and ERC. It does not affect the netlist: the footprint is placed, the pads
exist, and the copper is routed (§16.3).

### 7.6 Multi-unit packages

A quad op-amp is one instance with fourteen pins. Several statements referencing one
designator are the same physical package. Bindings are declared once, on the statement
that declares the instance; later references inherit them.

```
SIG-A-IN = INA.{U3~LM324: .v-pos=5V; .v-neg=GND}.OUTA = SIG-A-OUT;
SIG-B-IN = INB.{U3}.OUTB = SIG-B-OUT;
SIG-C-IN = INC.{U3}.OUTC = SIG-C-OUT;
SIG-D-IN = IND.{U3}.OUTD = SIG-D-OUT;
```

A reference requires an assigned designator, so a designator intended for multiple
references should be author-assigned rather than left as `?`.

---

## 8. Arrays and replication

### 8.1 Ranges

```
NAME[low:high]
```

Range order is significant and defines wire order. Reversing one side reverses the
mapping.

```
D[1:8]   = digital-bus[0:7];      // D1→bit0 … D8→bit7
DAC[4:0] = dac-channels[0:4];     // DAC4→ch0, DAC3→ch1 … DAC0→ch4
```

Widths on both sides of a connection shall match exactly. Manta never truncates, pads or
reindexes. Index bases need not match: `D[1:8] = bus[0:7]` is legal because both are eight
wide and both ascend.

Where one side omits its range it is inferred from the other.

```
>SIG-IN[0:3] = [[ I.{U?~AMP012}.O ]] = AMPED>;      // AMPED is 4 wide
```

### 8.2 Pin ranges

Within a part, a contiguous run of physical pins maps to an array. Widths shall match.

```
[3:11]: GPIO[1:9]>;      // nine pins, nine signals
```

### 8.3 Replication

```
[[ chain ]]              // count inferred
[N[ chain ]M]            // N and M are the in and out widths
```

`[[ ]]` instantiates one copy of its contents per element of the array flowing through it.
The copy count follows from the arity of the replicated unit and the width of the
connection.

```
>SIG2-IN[0:3] = [[ I.{U?~AMP012: PWR=3V3; GND=GND}.O ]] = AMPED[0:3]>;
```

Four copies, because `AMP012` is one-in one-out and the bus is four wide.

A block's arity is its declared in-port and out-port counts (§4.4). A part's is derived
from the pins used as chain terminals.

The copy count is `N / in_arity`, which shall equal `M / out_arity`. Where the widths do
not divide, that is error **E-05**; where the two quotients disagree, error **E-06**.

```
[4[ I.{U?~splitter}.O[0:1] ]8]      // 1-in 2-out unit: 4 copies, 8 out
```

The bracketed widths are repeated on the closing delimiter so a mismatched pair is caught
by eye.

Multiplicity shall not be applied to a replication, whether directly or through a group
whose sole content is a replication. That is error **E-37**.

```
[[.{R?~10kR-0603}.]]+2        // ERROR E-37
([[.{R?~10kR-0603}.]])+2      // ERROR E-37
```

### 8.4 Scalar bindings inside replication

Within `[[ ]]`, connections carrying an array index per copy; connections to a scalar net
broadcast to every copy.

```
>SIG[0:3] = [[ I.{U?~AMP012: PWR=PWR-SWITCHED; GND=GND}.O ]] = OUT[0:3]>;
```

Each amplifier receives its own signal; all four share `PWR-SWITCHED` and `GND`.

### 8.5 Per-copy values

`%` supplies a distinct value to each copy.

```
[[ I.{U?~AMP012: EN=%AMP-EN[0:3]; PWR=3V3}.O ]]
```

`%NAME[range]` draws the *n*th element for the *n*th copy. `%[a,b,c]` supplies an explicit
list, whose length shall equal the copy count.

```
({C?~10uF-0805: .=%[GND,-1V,-5V]}.)*3
```

### 8.6 Multiplicity

Multiplicity applies to a parenthesised group.

| Operator | Meaning |
|---|---|
| `+N` | N copies in series along the chain |
| `\|N` | N copies in parallel: entry terminals common, exit terminals common |
| `*N` | N copies hanging off the current node; topology unasserted |

```
(.{L?~MT100UFA}.)+2                              // two inductors in series
(A.{D?~DI3643}.K)|2                                // two diodes in parallel
({C?~100nF-0603: .=GND}.)*4                      // four caps to ground
({D?~ESD2013: .VCC=5V; .GND=GND}.)*2               // two ESD diodes sharing VCC
({C?~10uF-0805: .=%[GND,-1V,-5V]}.)*3            // three caps, three rails
```

`|N` is true parallel and the chain passes through the group. `*N` asserts only that N
copies hang off the current node; each copy's remaining terminals are determined by its own
bindings and may differ per copy.

---

## 9. Fields

### 9.1 Namespaces

| Sigil | Meaning |
|---|---|
| `@` | System field. The compiler interprets it. An unknown name is error **E-10**. |
| `#` | User field. Carried to BOM and documentation untouched. Unknown names are permitted. |

```
@footprint = QFP-32;        // interpreted
#allegro-xnet = DDR-A0;     // carried through
@footprnt = QFP-32;         // ERROR E-10
```

### 9.2 Strength

Every field takes a strength modifier between the sigil and the name.

| Form | Strength | Meaning |
|---|---|---|
| `@~name` / `#~name` | weak | A suggestion. Overridden by any other declaration. |
| `@name` / `#name` | normal | Overridable, but unusual to do so. |
| `@!name` / `#!name` | locked | Not overridable. Overriding is error **E-11**. |

The strongest declaration wins. Two declarations of equal strength with different values
are error **E-12**.

Declaration and override are distinguished by position: inside a `part` or `block`
definition you are declaring; inside an instantiation you are overriding.

Overriding is the strength ladder, not a separate mechanism. To override a normal field at
a call site, declare a locked one:

```
part cool-mcu {
    #mpn = "RC0603FR-0710KL";
};

.{R1~cool-mcu: #!mpn = "ERJ-3EKF1002V"; }.
```

Two declarations of equal strength that disagree are **E-12** wherever they appear, which
is what makes a field's value unambiguous.

```
part cool-mcu {
    @~footprint    = QFP-STM32-32;
    #!manufacturer = "ST Microelectronics";
};

{U1~cool-mcu: @footprint = QFP-STM32-32-CUSTOM; }   // overrides the weak default
{U2~cool-mcu: #manufacturer = "Acme"; }             // ERROR E-11
```

### 9.3 Scope

Fields are hierarchical and flow downward only. A field declared on a block is visible to
every declaration instantiated within it. A field declared on a part or an instance stays
there and does not propagate upward.

```
block audio {
    #board-rev = C;                 // visible to everything instantiated below
    {U1~cool-mcu};                  // sees board-rev
};

part resistor-0603 {
    #tolerance = ±1%;               // does not reach the enclosing block
};
```

### 9.4 Global import and export

A field may be drawn from, or published to, the global scope. The arrow points toward the
name for an import and away for an export.

```
>#author  = TJM;             // import from global, normal strength
>#~source = digikey;         // import from global, weak
#!>board-rev = C;            // export to global, locked
```

Export requires locked strength, so that a global's value cannot be changed by whichever
object happens to link last.

Canonical sigil order is direction, sigil, strength for an import (`>#~`), and sigil,
strength, direction for an export (`#!>`). Other orderings are accepted and left as
written; new code should use the canonical order.

A `part` shall not export a field. Export is available to blocks only, and `#!>` inside a
part definition is error **E-43**.

### 9.5 System fields

| Field | Type | Default | Meaning |
|---|---|---|---|
| `@footprint` | identifier | — | Physical footprint name. Required for any fitted part. |
| `@fitted` | boolean | `TRUE` | Whether the part is populated. `FALSE` is equivalent to a `!` prefix. |
| `@bom` | boolean | `TRUE` | Whether the part appears on the BOM. |
| `@type` | identifier | `board_part` | What the part is (§9.7). |
| `@VERSION` | constraint | — | Language revision required (§4.3). |

`@fitted` and `@bom` are independent.

```
@fitted=FALSE; @bom=TRUE;      // DNP resistor: on the BOM, flagged, quantity zero
@fitted=TRUE;  @bom=FALSE;     // printed antenna: exists, nothing purchased
```

A fitted part with no `@footprint` is error **E-20**.

### 9.6 User fields

Value, tolerance, manufacturer, MPN, cost, supplier and datasheet are user fields declared
in the part.

```
part R-10k-1pct-0603 {
    @~footprint = R-0603;
    #value      = 10kR;
    #tolerance  = ±1%;
    #power      = 100mW;
    @!type      = resistor;
    #~mpn       = "RC0603FR-0710KL";
    #~cost      = 0.002;
    #~supplier  = digikey;

    1: A &CASUAL;
    2: B &CASUAL;
};
```

Declaring `#mpn` weakly allows a second source to be substituted at a call site without
editing the part.

```
.{R1~R-10k-1pct-0603: #mpn = "ERJ-3EKF1002V"; }.
```

### 9.7 What a part is

`@type` says what a part is. Its value set is open: a design is free to write
`@type = regulator` or `@type = ferrite`, and such a value means nothing to the
compiler and travels to the BOM untouched.

Five values are **structural**, and the compiler does interpret them.

| Value | Meaning |
|---|---|
| `board_part` | Something on the board. The default when `@type` is unstated. |
| `boardconnector` | Something plugs into it. May carry `@mate`. |
| `cableconnector` | It plugs into something. May carry `@mates` and `@map`. |
| `wire` | A conductor. Its pins are its cores. |
| `crimp` | A terminal on a wire end. |

Because the set is open, a misspelt structural role cannot be an error — but its
consequence is silent, since `@type = boardconector` is simply not a connector
and every check that depends on one stops applying without a word. A value that
is not a structural role but is within one edit of one, or matches one after
case folding, is therefore warning **W-TYPE**.

`@type` is carried in the netlist and given a BOM column, and a rule may read it
as `component.type`.

---

## 10. Ports

### 10.1 Direction

A port marks a net as crossing a boundary. The arrow points toward the identifier for an
input and away from it for an output.

| Written | Direction |
|---|---|
| `>SIG` | input |
| `SIG<` | input |
| `<SIG` | output |
| `SIG>` | output |
| `<>SIG` / `SIG<>` | bidirectional |

Canonical form, which new code should write: leading `>` at the start of a statement,
trailing `>` at the end, and `.pin=NET>` in a binding. The formatter leaves any accepted
form as written.

```
>SIG-IN = I.{U1~AMP012}.O = AMPED-SIG>;
```

### 10.2 Bare port declarations

A port may be declared without connecting.

```
>nPWR-EN;               // this net is an input to this block
AMPED-SIG>;             // this net is an output
<>i2c;                  // bidirectional
```

### 10.3 Global scope

Doubling the arrow makes a port global rather than block-scoped. A block-scoped export is
visible to the block that instantiates this one; a global export is visible design-wide.

```
>>VIN;                  // global import
3V3>>;                  // global export
GND>>;
[3V3, GND]>>;           // list form, identical to the two lines above
```

---

## 11. Directives

### 11.1 Form

```ebnf
directive = "&" [ "~" | "!" ] identifier [ "=" ( value | match_ref ) ] ;
```

A directive attaches a constraint to whatever its statement declares: the nets of a chain
in a chain statement, or a pin in a pin-map line. `&CASUAL` and `&STUB` take no value;
every other directive requires one.

Directives take the same strength ladder as fields (§9.2).

```
&~IMP=50R;              // a target the router should aim for
&IMP=50R;               // normal
&!IMP=50R;              // a constraint the fabricator shall meet
```

Directives accumulate per key. `&IMP` from one statement and `&CURRENT` from another both
apply to a shared net; only same-key collisions at equal strength are conflicts
(**E-12**).

### 11.2 Scope

A directive is written at the end of the statement, before the terminator.

```
>SIG3-IN[0:3] = [[.{R?~100kR-0603}.]] =* COMBINED-OUT> &IMP=50R;
```

A directive covers every net in the declared chain, without exception:

- every net produced by a `[[ ]]` replication in that chain, not merely one of them
- every segment of the chain, including across `^`

It does not cover:

- the nets of a `:` binding's chain, which is its own scope (§7.4)
- nodes internal to a part or block

```
SW-NODE = ({C?~100nF-0603: .=GND}.)*4 == 3V3
        = S.{Q?~FFET123: .G=nPWR-EN}.D = PWR-SWITCHED &CURRENT=3A;
```

`SW-NODE`, `3V3` and `PWR-SWITCHED` carry `&CURRENT=3A`. `GND` and `nPWR-EN` do not.

To exclude part of a chain, split it into its own statement. Because the statement is the
scope unit, reflowing a statement across lines never changes its constraints.

### 11.3 Net directives

| Directive | Value | Meaning |
|---|---|---|
| `&IMP` | resistance, optional `D` suffix | Characteristic impedance. |
| `&CURRENT` | current | Maximum steady-state current. |
| `&PEAK` | current | Maximum transient current. |
| `&VOLTAGE` | voltage | Maximum net voltage. |
| `&MAXDELAY` | time | Maximum propagation delay. |
| `&CLASS` | identifier | Net class membership (§11.9). |
| `&MATCH` | group, or group with overrides | Delay-matching membership (§11.4). |
| `&LAYER` | identifier | Preferred or required layer. |
| `&SHIELD` | net name | Net that shall shield this one. |
| `&TYPE` | `GROUND` | Marks a ground net (§5.3). |
| `&STUB` | *(none)* | This net is deliberately referenced once (§11.8). |
| `&RAIL` | *(none)* | Marks a power rail for rendering, whatever the net's name or class. |

```
USB-DP = MCU-DP &IMP=90RD;
3V3 &CURRENT=3A &VOLTAGE=5V;
CLK &MAXDELAY=600ps &SHIELD=GND;
VSYS-PROT &RAIL &CURRENT=2A;
```

`&RAIL` is display-only: it adds no electrical claim and brings no check with
it. A renderer decides which nets are supplies by evidence and by spelling —
`&CLASS=power`, a `&TYPE=POWER` source pin, a `3V3`-shaped name — and those
heuristics remain; `&RAIL` is the explicit override for a rail they miss, such
as a switched or divided supply under a project-local name.

An unknown directive name is error **E-13**.

### 11.4 Delay matching

Matching is a relation across a set of nets with a designated reference. It uses a named
group.

```
match ddr-addr {
    @src       = U1;
    @dest      = [U5, U6, U7];
    @tolerance = 5ps;
};

ADDR[0:15] &MATCH=ddr-addr;
DDR-CLK    &MATCH=ddr-addr;
```

| Field | Meaning |
|---|---|
| `@src` | The reference. A designator; the pin is the one where the net meets it. |
| `@dest` | Destination designator, or a list of them. |
| `@tolerance` | Permitted deviation. A scalar applies to every destination; a list applies element-wise. |
| `@offset` | Required delay relative to the reference. Default `0`. Positive means later. |

Tolerance shall be a time, never a length. What matters electrically is propagation delay,
and identical lengths do not give identical delays: a microstrip and a stripline on one
board see different effective permittivities. A tolerance given as a length is error
**E-18**.

A scalar tolerance requires every destination to match the source within that figure — a
star topology. A list gives each destination its own budget, which is how a flyby chain is
expressed.

```
match ddr-addr-flyby {
    @src       = U1;
    @dest      = [U5, U6, U7];
    @tolerance = (5ps)*3;
};
```

A member may offset or tighten its own constraint at the point of use. Overrides take the
strength ladder.

```
DDR-CLK &MATCH={ddr-addr: @!offset=10ps};
```

The clock shall arrive 10ps after the data and within the tolerance the group otherwise
defines.

A match group may contain another. For the purposes of an outer group, the delay of an
inner group is the delay of its reference member.

```
match ddr-clk-pair {
    @src       = U1;
    @dest      = [U5];
    @tolerance = 2ps;
    match ddr-addr {
        @src       = U1;
        @dest      = [U5, U6, U7];
        @tolerance = 5ps;
    };
};
```

A member's delay is trace delay plus `&PINDELAY` at both ends. A tolerance is a budget
against that total.

### 11.5 Pin delay

`&PINDELAY` declares the delay from pad to die for a pin. It belongs in the part, where it
is written once from the datasheet, and is declared weakly so a module or multi-board
assembly can override it.

```
part ddr-chip {
    [1:16]: DQ[0:15]<> &~PINDELAY=12ps;
};

{U5~ddr-chip: .DQ[0] &PINDELAY=18ps; }      // override at a call site
```

### 11.6 Pin type and default net

`&TYPE` on a pin declares electrical character. The arrow declares direction. The two axes
are orthogonal and combine freely.

| `&TYPE` | Meaning |
|---|---|
| *(omitted, no arrow)* | `PASSIVE`. Claims nothing; skipped by drive checks. |
| *(omitted, arrow present)* | `SIGNAL`. |
| `POWER` | Sits on a power net. `>` provides the rail, `<` consumes it. |
| `OPENDRAIN` | Many drivers permitted on one net. |
| `NC` | Shall not be connected. Weakly implies `&STUB`. |

```
part ldo {
    1: VIN<   &TYPE=POWER;      // consumes a rail
    2: GND<   &TYPE=POWER;
    3: VOUT>  &TYPE=POWER;      // provides one
    4: EN<;                     // SIGNAL
    5: NC     &TYPE=NC;
};

part resistor-0603 {
    1: A &CASUAL;               // PASSIVE
    2: B &CASUAL;
};
```

A pin that can release a bus is `<>`; there is no separate tri-state type. E-01 fires only
on multiple `>` pins.

Marking a pin `POWER` brings three checks with it, requiring no annotation in any design
that uses the part: a net with `POWER<` pins and no `POWER>` source is error **E-27**; two
`POWER>` pins on one net is error **E-28**; and a `POWER>` net with no consumers is warning
**W-09**. A ground net (§5.3) is exempt from E-27.

An arrow and a conflicting `&TYPE` are caught by **E-12**.

#### Default nets

`&NET` names the net a pin joins when nothing binds it. Declared weakly, so a binding at a
call site overrides it.

```
part cool-mcu {
    1: VCC< &TYPE=POWER &~NET=3V3;
    2: GND< &TYPE=POWER &~NET=GND;
};

{U1~cool-mcu};                          // VCC→3V3, GND→GND
{U2~cool-mcu: .VCC=1V8; };               // VCC→1V8, GND→GND
```

`&NET=?` unbinds a pin. No net is created, so no `&STUB` is required.

```
{U5~iso7741: .GNDB=?; }                  // deliberately floating this side
```

`&TYPE=NC` and `&NET=?` both leave a pin unconnected for different reasons: `NC` means the
datasheet forbids connection, and connecting it is error **E-25**; `?` means the author
declined to connect it here, and is always legal.

### 11.7 Casual pins and swap groups

`&SWAP` names a permutable index rather than a set of pins. Every array tagged with one
index permutes together, which is what makes a ganged swap expressible.

```
part buffer4 {
    [1:4]: IN[1:4]<  &SWAP=ch;
    [5:8]: OUT[1:4]> &SWAP=ch;
};
```

Exchanging channels 2 and 3 permutes `IN` and `OUT` identically.

```
part opamp-quad {
    [1:4]:   INP[1:4]< &SWAP=ch;
    [5:8]:   INN[1:4]< &SWAP=ch;
    [9:12]:  OUT[1:4]> &SWAP=ch;
};

part fpga-bank {
    [1:48]: IO[1:48]<> &SWAP=bank0;
};

part nand-quad {
    [1:8]:  A[1:4]<  &SWAP=gate;      // gates swap with each other
    [9:12]: B[1:4]<  &SWAP=gate;
    [13:16]:Y[1:4]>  &SWAP=gate;
};
```

`&CASUAL` grants two things: the pin may be chosen by a `.` terminal (§7.3), and it joins a
weak implicit group `&~SWAP=CASUAL`. Being weak, the implicit group is replaced by any
`&SWAP` written on the same line, and it is scoped per declaration line, so two independent
elements in one part do not become mutually swappable.

```
part dual-resistor {
    [1:2]: A[1:2] &CASUAL;      // group CASUAL#1
    [3:4]: B[1:2] &CASUAL;      // group CASUAL#2 — pin 1 and pin 3 do not swap
};
```

Overriding the swap group does not revoke `.` eligibility.

All members of a swap group shall carry compatible directives; a group whose members
disagree is frozen and generates warning **W-08**. Swaps performed during layout are
written back to source by `manta annotate` (§13.6).

### 11.8 Stubs

A net referenced exactly once is error **E-26**. A real net is written at least twice, once
at each end, so a single reference is almost always a typo — including a mistyped harness
member, which would otherwise create a new one.

`&STUB` declares the single reference deliberate.

```
TP7 = U3.OUT &STUB;             // a test point
```

A stub carries exactly one pin. `&STUB` on a net with more than one pin is error **E-33**.

`&TYPE=NC` implies `&STUB` weakly.

### 11.9 Net classes

A net class carries directives for many nets at once. Per-net directives override class
directives at equal strength.

```
netclass power {
    &CURRENT  = 3A;
    &!VOLTAGE = 60V;
};

3V3 &CLASS=power;
5V  &CLASS=power;
12V &CLASS=power &CURRENT=8A;      // overrides the class current
```

### 11.10 Instance directives

A directive written bare in an instance's binding list — no pin in front of it —
annotates the instance itself. Instance scope exists as of revision 1.5, and one
directive has it.

| Directive | Value | Meaning |
|---|---|---|
| `&EDGE` | `LEFT`, `RIGHT`, `TOP` or `BOTTOM` | Which sheet edge the connector faces. |

```
{J1~CONN-6P: &EDGE=LEFT; .VIN = VPOS; .GND = GND; };
```

The value set is fixed, so it is upper case (§2.6); `&EDGE=left` is error
**E-34**. `&EDGE` is display-only, exactly as `&RAIL` is: it changes what a
renderer draws and nothing that any check reads. It travels on the component's
`edge` key in the netlist (§15.4), emitted only when written.

Directives take the strength ladder at instance scope as everywhere else
(§11.1): a stronger `&EDGE` replaces a weaker one, and two at equal strength
with different values are error **E-12**. Any other directive written bare in a
binding list is error **E-13** — including every directive that is legal on a
net or a pin, since none of them says anything about an instance. Before 1.5
such a directive was accepted and ignored; no meaning has been removed, because
the form never had one.

---

## 12. Harnesses

### 12.1 Types and assignment

The `harness` keyword defines a type.

```
harness i2c-bus {
    SDA<> &TYPE=OPENDRAIN;
    SCL<  &TYPE=OPENDRAIN;
};
```

A type is assigned to an identifier with `&HARNESS`.

```
i2c &HARNESS=i2c-bus;
```

Members are accessed as `name.member`.

```
I2C-SDA = i2c.SDA;
I2C-SCL = i2c.SCL;
```

Assigning a whole harness assigns every member pairwise by name.

```
USB = MCU-USB;                  // equivalent to member-by-member assignment
```

### 12.2 Member lists

A harness type shall not be assigned to a bare pin range; that is error **E-38**. Members
are written as an ordered list, so the mapping is stated where it is read.

```
[12:13]: USB.[+,-]<>;
[20:22]: i2c.[SDA,SCL,ALERT]<>;
```

`NAME.[member, …]` selects members in the order written. Its length shall equal the width
of the pin range.

### 12.3 Implied harnesses

A harness need not be declared. Writing `i2c.SDA` with no prior definition implies a
harness with that member, and further members accrue as they are used.

```
i2c.SDA = MCU-SDA;              // i2c is implied, with member SDA
i2c.SCL = MCU-SCL;              // member SCL accrues
```

A mistyped member is caught by **E-26** (§11.8): it is referenced exactly once, where a
real member is written at least twice.

### 12.4 The built-in `diff` type

`diff` is a built-in harness type with exactly two members, `+` and `-`, always in that
order.

```
USB &HARNESS=diff;

USB.+ = MCU-USB.+;
USB.- = MCU-USB.-;
```

`+` and `-` are member names in the harness namespace and do not collide with the
leading-hyphen identifier rule of §2.3.

A `diff` harness carries an implicit intra-pair skew constraint in time. `&IMP` applied to
a `diff` shall carry the `D` suffix; a single-ended impedance on a differential pair is
error **E-14**.

### 12.5 Harness-carried directives

A harness type may carry directives, which apply to every identifier assigned that type.
This is the preferred way to constrain a repeated interface.

```
harness usb2 {
    D &HARNESS=diff;
    &!IMP     = 90RD;
    &MAXDELAY = 600ps;
};

usb-host &HARNESS=usb2;         // inherits 90RD and 600ps
usb-dev  &HARNESS=usb2;
```

---

## 12A. Connectors, cables and mating

A board describes what is on it. A loom is not on it: it is a separate thing to
build, with its own bill of materials, and the question of whether the two fit is
one nobody can answer from either alone.

### 12A.1 A cable is a declaration

```
cable jumper-8way {
    #length = 300mm;

    {J1~JST-PH-8-PLUG}.P[1:8]
        = [[ .{C%[1:8]~JST-PH-8-CRIMP}. = .{W%[1:8]~WIRE-22AWG-RED}.
           = .{C%[9:16]~JST-PH-8-CRIMP}. ]]
        = P[1:8].{J2~JST-PH-8-PLUG};
};
```

A cable body is a chain, exactly as a block body is, so replication, groups,
ranges and bindings all work in one. That is what keeps an eight-way loom to a
single statement.

A cable may hold only a cable connector, a wire or a crimp (§9.7). Anything else
is error **E-44**. Wires and crimps are ordinary parts, so each carries an MPN
and appears on a BOM: a loom is bought as much as a board is.

A cable is a top in its own right. `manta link --top jumper-8way` produces the
loom's netlist and BOM, and never the board's. Two rules that are right for a
board are wrong for a loom and do not apply to one: **E-24**, which requires a
ground net, and **E-20**, which requires a footprint of every fitted part.

### 12A.2 Mating

One field per side, so the two never appear on the same declaration:

| Field | Written on | Meaning |
|---|---|---|
| `@mate` | a `boardconnector` | which cable is fitted here |
| `@mates` | a `cableconnector` | which board connector it plugs into |
| `@map` | either | how the pins line up; absent means one to one |

`@mate` takes the strength ladder like any field, so a part may carry a weak
default and a call site override it:

```
part BACKPLANE-OUT { @type = boardconnector; @~mate = jumper-8way; };

.{J3~BACKPLANE-OUT: @mate = short-jumper; }.
```

A `@map` is a list of pairs. Each element is a pin number or a range, and a range
pairs element-wise with its opposite — descending included, which is how a
reversed ribbon is written.

```
@map = [[2,3],[3,2],[7,8],[8,7]];    // a null modem
@map = [[1:20],[20:1]];              // a reversed ribbon
```

### 12A.3 What is checked

Structural fit, always:

- the cable named by `@mate` exists and is a cable;
- one of its connectors declares `@mates` naming this board connector's part —
  otherwise **E-45**;
- the pins line up: a count mismatch with no `@map`, or a `@map` naming a pin
  that does not exist, is **E-46**.

And electrically, when the loom's far end plugs back into a connector on this
same board — a board plugged into another copy of itself, which one board's
source is enough to describe. Each conductor is followed from the near board
pin, through the loom's wires and crimps, to the far board pin, and the result
is judged as though the two had been wired together directly, because once the
loom is fitted they have been:

- two pins that both drive are **E-47**;
- a supply meeting a ground is **E-48**.

Wire ampacity, insulation voltage and "every connector must be mated" are
deliberately not built in. They are one-line checks in a `.mantaRules` file,
because the derating a project accepts is a project's decision and not a
language's:

```
check unmated for component {
    when    component.type == boardconnector;
    require has(component.mate);
    error   "{component} is a connector with nothing plugged into it";
};
```

### 12A.4 Assembly output

`manta link --assembly` additionally writes a netlist, and with `--bom` a bill of
materials, for every cable a connector on the board mates with. They are separate
files named for the cable. The board's own outputs are byte-identical with and
without the flag: a loom is not part of a board, and a layout tool has nowhere to
put a wire.

---

## 13. Designators and annotation

### 13.1 Annotation is a separate command

The compiler does not annotate. `manta compile` and `manta link` never write to a source
file; annotation is `manta annotate` (§15.7), run deliberately by the author.

Two properties follow:

- Compilation and linking are side-effect-free. A build leaves the working tree clean.
- Nothing in the netlist or in ERC depends on a designator existing. An un-annotated design
  shall compile, link and check completely. Unassigned instances carry internal identities
  derived from their block instance path.

A reference to an existing instance (§7.2) requires an assigned designator, since `{U3}`
needs `U3` to exist.

### 13.2 Auto-assignment

A designator written with `?` is unassigned.

```
{U?~STM32F0QA5}
```

`manta annotate` assigns the next free number for that prefix and writes it back.

```
{U7~STM32F0QA5}
```

### 13.3 Replicated instances

Where one statement instantiates a part or block N times, the annotator writes a range
designator: one token carrying N designators.

```
(.{BLK?~my-block}.)*4          // before
(.{BLK%[1:4]~my-block}.)*4     // after
```

Ranges may be non-contiguous, using the list form of §8.5, so a group that grows never
forces the renumbering of designators already assigned.

```
BLK%[1:4,9:10]
```

A group of one collapses: a single instance annotates to `BLK1`, never `BLK%[1:1]`.

### 13.4 Designators inside a replicated block

A designator is annotated in the scope where it is written. If `my-block` contains `R?` and
`C?`, each is annotated once, to `R1` and `C1`, however many times the block is
instantiated.

```
block my-block {
    >IN = .{R1~10kR-0603}. = .{C1~100nF-0603: .=GND} == OUT>;
};
```

An instance's identity is its path, and designator uniqueness is per scope. `R1` inside
`my-block` and `R1` at top level are different components.

```
BLK1.R1     BLK2.R1     BLK3.R1     BLK4.R1
```

Export flattens the path to the single unique string a BOM and a layout tool require. The
flat name is derived at export and never stored in source (§13.7). Being path-based it is
stable under insertion: adding a component inside `my-block` renumbers nothing outside it.

The flattening template is `@FLATFORMAT` on a block, defaulting to path components joined
by `_`.

```
block my-block {
    @FLATFORMAT = "$COMPONENT$_$INSTANCE$";      // R1_BLK2
};
```

### 13.5 Reopening

Changing an assigned designator back to `?` releases it for reassignment on the next
`manta annotate`. This is the mechanism for renumbering after a layout reorganisation.
Reverting a range designator releases every member of the group.

```
{U7~STM32F0QA5}    →    {U?~STM32F0QA5}
BLK%[1:4]          →    BLK?
```

### 13.6 Stability and swap back-annotation

Once assigned, a designator is not changed by any tool. Adding, removing or reordering
components does not renumber existing parts.

Parts marked `@fitted=FALSE` receive designators: the footprint is on the board and the
fabrication data requires it.

`manta annotate` also reconciles swaps performed during layout (§11.7). A router that
exchanges two members of a swap group has that exchange written back to source.

### 13.7 Derived information is not written to source

No tool writes descriptive text into a source file. A part's description, an inferred
replication count, a `&~NET` default that was taken, or the generated name of an unnamed
node are derivable, and belong in an editor as hover text or inlay hints.

The only writes any tool makes to source are designator assignment (§13.2, §13.3) and swap
reconciliation (§13.6). Both are semantic values that are part of the design.

---

## 14. Substitution

### 14.1 When substitution happens

`$…$` is evaluated at link time. A field may be overridden at an instantiation (§9.2), so
the value a substitution resolves to depends on how the enclosing block was instantiated,
which is known only during elaboration.

- A `.mantaO` object carries substitutions unevaluated.
- A block instantiated twice with different parameters produces two elaborations from one
  source.
- Every substitution error is reported at link, alongside ERC.

### 14.2 Form

```
$ expression $
```

The closing `$` is required, since substitution is most useful inside an identifier and a
bare `$NAME` cannot be delimited from a longer identifier.

```
R?~$res-val$-0603
GPIO[$n + 1$]
@fitted=$fit-amp & width > 4$
[$width * 2$]
```

Every operator inside `$…$` is an operator: the delimiters place the lexer in a distinct
mode, so `+ - * / ^ | & ! < > =` carry their arithmetic meaning there and their manta
meaning everywhere else. Comments are not recognised inside, and `/` is always division.

### 14.3 Operators

| | Operator | Operands | Result |
|---|---|---|---|
| 1 | `( )` | | grouping |
| 2 | unary `-`, `!` | integer, boolean | negate, logical not |
| 3 | `^` | integer | exponent, right-associative |
| 4 | `*` `/` | integer | integer |
| 5 | `+` `-` | integer | integer |
| 6 | `<` `>` `<=` `>=` | integer | boolean |
| 7 | `=` `!=` | integer or boolean | boolean |
| 8 | `&` | boolean | and |
| 9 | `~` | boolean | exclusive or |
| 10 | `\|` | boolean | or |

Tighter binds first; brackets override.

```
$ (a + b) * 2 $
$ n ^ 2 $
$ !fitted $
$ width > 4 & depth > 4 $
$ mode = 3 | override $
```

`/` is integer division truncating toward zero. Division by zero is error **E-39**. A
negative exponent is error **E-42**.

Comparison is the bridge between the two domains: it takes integers and yields a boolean.
`&`, `~` and `|` take booleans only, and an integer operand there is error **E-40**.

Only integers and booleans are computed with. A dimensioned value, string or list may be
substituted but not used as an operand; doing so is error **E-41**.

### 14.4 Units

Arithmetic operates on bare integers. A unit is written outside the delimiters, as source
text following the substitution.

```
$100 * 2$R              // 200R
$base-width$mm
R?~$val * 3$R-0603
```

### 14.5 Operands

An operand is an integer literal, a boolean literal, or a `#` or `@` field visible in the
current scope (§9.3). There is no path syntax; crossing scopes is the generator's task.

`-` inside `$…$` is always subtraction. A field whose name contains a hyphen is referenced
by quoting it.

```
$"min-supply" + 1$      // the field min-supply, plus one
$min - supply$          // one field minus another
```

Substitution is read-only; nothing is assigned. An undefined field is error **E-29**, never
an empty string.

### 14.6 Position

Substitution is legal only in a value position: inside an identifier, as a field value, as
an array index, or as a directive value. It shall not produce a statement, a declaration or
an operator. That is error **E-15**.

```
R?~$val$-0603           // legal
@fitted=$fit-amp$       // legal
GPIO[$n$]               // legal
&CURRENT=$amps$A        // legal
$"C?~100nF-0603;"$      // ERROR E-15
```

Every part declaration therefore remains present in the source, which is what makes
designator write-back possible (§13).

Because a substitution occupies one delimited span on one line, source positions map
through it by length alone.

### 14.7 Rendering

| Result | Substituted as |
|---|---|
| integer | decimal, no leading zeros |
| boolean | `TRUE` / `FALSE` in a system field, `true` / `false` in a user field |
| dimensioned value | as written, SI-substituted |
| string | verbatim, without quotes |
| list | comma-separated, without brackets |

List rendering allows a list field to drop directly into `%[...]`, `[...]` and `@dest`
positions.

```
#rails = [GND, -1V, -5V];
({C?~10uF-0805: .=%[$rails$]}.)*3
```

---

## 15. Compilation and linking

### 15.1 Model

Manta uses a two-stage model.

| Stage | Input | Output |
|---|---|---|
| `manta compile` | one `.manta` source | one `.mantaO` object |
| `manta link` | many `.mantaO`, plus a named top-level block | one `.mantaNets` |
| `manta annotate` | sources, plus a `.mantaNets` | rewritten sources |
| `manta export` | one `.mantaNets` | a layout-tool netlist |
| `manta fmt` | sources | re-indented sources |

### 15.2 Compilation

Per source file:

1. **Lex and parse.** Syntax errors are reported here.
2. **Validate locally.** Well-formedness, and every rule requiring no external name.
3. **Emit object.** `.mantaO`, with external names unresolved and substitutions
   unevaluated.

A `.mantaO` object carries declarations, their interfaces, and their bodies in intermediate
form. It is not a netlist fragment: a block cannot be elaborated until instantiated, since
`[[ ]]` derives its count from the call site and `#!` overrides change what is emitted.

Separate compilation is most valuable for part libraries, which are large, stable, shared
between projects, and fully specified.

### 15.3 Linking

The linker is given a set of objects and the name of the top-level block. It then:

1. resolves every referenced name against the declarations in all supplied objects
2. elaborates the top-level block, instantiating recursively and monomorphising each block
   against its parameters and connection widths, evaluating `$…$` against the fields in
   force at each instantiation
3. resolves global imports and exports
4. builds the netlist
5. runs ERC (§16)
6. emits `.mantaNets`

ERC runs at link and not at compile: no-driver, no-source, multiple-driver and
unpowered-net are whole-design properties and cannot be evaluated one object at a time.

### 15.4 File format

`.mantaO` and `.mantaNets` are JSON, validated against a published JSON Schema, and both
carry a `version` field naming the language revision they target.

Each entry in a net's `pins` array carries both `pin`, the physical package pin
a layout tool routes to, and `logical`, the name the part declares for it. It
also carries `type` and `direction`, the pin's electrical character after the
strength ladder (§11.6) and the direction its arrow declares (§10). Those two
cannot be recovered from a netlist any other way, because the part declaration
is not part of the interchange, and a layout tool needs them: KiCad puts them on
the pad and its design-rule check reads them. Both are optional, so a netlist
written before they were emitted remains valid; a reader that finds neither
shall assume `PASSIVE` and `none`.

A netlist may also carry a top-level `swaps` array recording the exchanges a
router made within a swap group, which is what `manta annotate --swaps` reconciles
back to source (§13.6); it is optional, and its absence makes `--swaps` a no-op.

Revision 1.3 adds what a renderer needs, all of it optional so an older netlist
still validates. Each component carries a `pins` array — its declared pins in
declaration order, so a pin on no net survives into the interchange — and a
`section` naming the render section (§4.7) it was instantiated under. A top-level
`blocks` array records the instance hierarchy: each entry's `path`, `block`,
`section`, resolved `ports` and `localNets`, the last being the block-local
spellings a definition page displays. The `version` field names the language
revision the emitting toolchain implements.

Revision 1.5 adds one optional key of the same display-only kind: a component
carries `edge` — `LEFT`, `RIGHT`, `TOP` or `BOTTOM` — when its instance wrote
`&EDGE` (§11.10), and no such key otherwise. A value-less net directive such as
`&STUB` or `&RAIL` appears in a net's `directives` object with an empty string
as its value.

```json
{
  "version": "1.0",
  "kind": "mantaNets",
  "top": "power-and-signal",
  "components": [
    {
      "designator": "U1",
      "path": ["power-and-signal", "U1"],
      "part": "cool-mcu",
      "fitted": true,
      "bom": true,
      "footprint": "QFP-STM32-32",
      "fields": { "value": "STM32F0QA5", "manufacturer": "ST Microelectronics" }
    }
  ],
  "nets": [
    {
      "name": "3V3",
      "pins": [
        {"designator": "U1", "pin": "1",  "logical": "VCC", "type": "POWER",   "direction": "in"},
        {"designator": "C1", "pin": "1",  "logical": "A",   "type": "PASSIVE", "direction": "none"}
      ],
      "directives": { "CURRENT": "3A", "CLASS": "power" }
    }
  ],
  "matches": [
    { "name": "ddr-addr", "src": "U1", "dest": ["U5"], "tolerance": "5ps", "offset": "0" }
  ]
}
```

### 15.5 Command line

Every command accepts `--help` and `--version`.

Common options:

| Option | Meaning |
|---|---|
| `-W<name>` | Enable warning `<name>`. |
| `-Wno-<name>` | Disable warning `<name>`. |
| `-Werror` | Treat every warning as an error. |
| `--error=<code>` | Promote one diagnostic to an error. |
| `--warn=<code>` | Demote one diagnostic to a warning. |
| `--json-diagnostics` | Emit diagnostics as JSON on stderr. |
| `-q`, `--quiet` | Suppress non-diagnostic output. |
| `-v`, `--verbose` | Report each stage. |
| `--no-colour` | Disable ANSI colour. |

#### `manta compile`

```
manta compile [options] <source.manta>...
```

| Option | Meaning |
|---|---|
| `-o`, `--output <path>` | Object output. A file for one source, a directory for several. Default: alongside each source. |
| `-I`, `--include <dir>` | Directory searched for sources named on the command line. Repeatable. |
| `--revision <rev>` | Language revision to check `@VERSION` against. Default: the implementation's own. |
| `--emit-ast` | Also write the parse tree as JSON, for tooling. |

#### `manta link`

```
manta link [options] --top <block> <object.mantaO>...
```

| Option | Meaning |
|---|---|
| `-t`, `--top <block>` | **Required.** Name of the top-level block. |
| `-o`, `--output <file>` | Netlist path. Default: `<top>.mantaNets`. |
| `-L`, `--library <dir>` | Directory of objects to resolve against. Repeatable. |
| `--bom <file>` | Also emit a BOM as CSV. |
| `--no-erc` | Skip ERC and emit regardless. |
| `--assembly` | Also write a netlist and BOM for every mated cable, as separate files. |
| `--no-emit` | Run every stage including ERC, emit nothing. |
| `--map <file>` | Write the elaboration map: instance path to designator. |

#### `manta check`

```
manta check [options] --top <block> <object.mantaO>...
```

Equivalent to `manta link --no-emit`. Accepts the same options.

#### `manta annotate`

```
manta annotate [options] <source.manta>...
```

| Option | Meaning |
|---|---|
| `-n`, `--netlist <file>` | `.mantaNets` to take assignments from. **Required.** |
| `--reopen <prefix>` | Revert every designator with this prefix to `?`. Repeatable. |
| `--start <prefix>=<n>` | Begin assignment for a prefix at `n`. Repeatable. |
| `--swaps` | Apply swap reconciliation as well as designators. |
| `--dry-run` | Report the changes without writing. |

#### `manta fmt`

```
manta fmt [options] <source.manta>...
```

| Option | Meaning |
|---|---|
| `--check` | Exit non-zero if any file would change. Write nothing. |
| `--stdout` | Write to standard output instead of in place. |
| `--diff` | Print a unified diff of the changes. |

#### `manta export`

```
manta export [options] --format <target> <netlist.mantaNets>
```

| Option | Meaning |
|---|---|
| `-f`, `--format <target>` | `kicad`, `altium`, `orcad`, `allegro`. **Required.** |
| `-o`, `--output <file>` | Output path. Default: derived from the input name. |
| `--constraints <file>` | Write directives to a separate constraint file where the target cannot carry them. |
| `--flat-format <template>` | Override `@FLATFORMAT` for hierarchical designators. |
| `--footprint-map <file>` | Map footprint names to the target's, one `name  Library:Footprint` pair per line. |
| `--footprint-lib <nickname>` | Library nickname for any footprint the map does not cover and that names no library itself. KiCad only. |

#### `manta render`

```
manta render [options] <netlist.mantaNets>
```

Renders a netlist as a clickable HTML schematic: one sheet per block definition, with
the render sections of §4.7 as titled rooms.

The rooms tile the sheet edge to edge — every room border meets a neighbour or the
sheet frame, the way a hand-drawn schematic partitions its page — and the sheet as a
whole aims for a landscape shape. Inside a room, signal flow reads left to right:
connectors and other entry points sit at the left, loads and outputs at the right,
and a part's pins face the parts they talk to, with each pin's net name printed
beside the pin name inside the body. Power rails with more than one consumer in a
room run as a named horizontal bar with taps dropping to their consumers; decoupling
capacitors hang directly from it. A net whose pins all sit in one room is drawn as a
wire; where drawing one is impossible the net's pins carry its name instead, which
connects them just as firmly. A net that crosses rooms connects by name — a plain
label at each appearance. PORT flags are reserved for signals that leave the page
entirely: a block's ports, on both the parent's sheet symbol and the definition's
own page. No-connect pins keep their cross, and single-pin `&STUB` nets keep their
label.

| Option | Meaning |
|---|---|
| `-o`, `--output <file>` | HTML output path. Default: derived from the input name. |
| `--title <text>` | Title-block text. Default: the design's top block. |
| `--pdf <file>` | Also print the sheets to PDF, through a headless Chromium found on `PATH`. |

### 15.6 Diagnostics

A diagnostic is written to stderr in the form:

```
<file>:<line>:<column>: <severity>[<code>]: <message>
```

```
power.manta:42:9: error[E-49]: '==' after an element that passes through; the chain advances with '='
power.manta:71:5: warning[W-02]: R12 is shorted: both pads land on one net
```

Under `--json-diagnostics` each is an object on its own line:

```json
{"file":"power.manta","line":42,"column":9,"severity":"error","code":"E-49",
 "message":"'==' after an element that passes through; the chain advances with '='"}
```

Diagnostics arising after substitution report the position of the substitution in the
original source.

### 15.7 Exit codes

| Code | Meaning |
|---|---|
| `0` | Success. Warnings may have been emitted. |
| `1` | One or more errors. No output written. |
| `2` | Usage error: bad option, missing required argument, unreadable input. |
| `3` | Internal error. |

### 15.8 Determinism

Given identical input, manta produces byte-identical output at every stage. There is no
timestamp, random seed, environment dependency or external process invocation. `manta
render`'s HTML is inside this guarantee; the PDF that `--pdf` prints through an external
browser is not.

---

## 16. Electrical rule checking

ERC is part of the language and runs at link. Manta's explicit intent markers — `=*`, `*=`,
`!`, `&STUB`, `&NET=?`, `&CASUAL`, `&!` — let the checker distinguish deliberate constructs
from mistakes.

### 16.1 Errors

| Code | Rule |
|---|---|
| E-01 | Two or more `>` pins drive one net, with no open-drain or bus declaration. |
| E-02 | A net has an input and nothing that can drive it — no `>` or `<>` pin, no supply, no passive. Also: an identifier ends in `-`. |
| E-04 | Array/scalar width mismatch without `=*` or `*=`. |
| E-05 | Replication width not divisible by unit arity. |
| E-06 | `[N[ ]M]` widths disagree with unit arity. |
| E-07 | Reference to a designator that is never declared. |
| E-08 | A pin appears both as a chain terminal and in the binding list. |
| E-09 | An empty binding list written with punctuation. |
| E-10 | Unknown `@` field name. |
| E-11 | Override of a locked (`!`) field. |
| E-12 | Two declarations of equal strength with different values. |
| E-13 | Unknown `&` directive name. |
| E-14 | Single-ended `&IMP` applied to a differential harness. |
| E-15 | Substitution in a non-value position. |
| E-17 | An imperial unit literal. |
| E-18 | A match tolerance or skew constraint given as a length. |
| E-20 | A fitted part with no `@footprint`. |
| E-21 | A harness name collides with a designator. |
| E-23 | A `.` terminal selects a pin without `&CASUAL`. |
| E-24 | The design declares no `&TYPE=GROUND` net. |
| E-25 | A pin marked `&TYPE=NC` is connected. |
| E-26 | A net is referenced exactly once and does not carry `&STUB`. |
| E-27 | A power net has `&TYPE=POWER<` pins and no `&TYPE=POWER>` source. |
| E-28 | Two `&TYPE=POWER>` pins on one net. |
| E-29 | Substitution of an undefined field. |
| E-30 | A name is declared in more than one object. |
| E-31 | A name is referenced but never declared. |
| E-32 | A block port carries no direction arrow. |
| E-33 | `&STUB` applied to a net with more than one pin. |
| E-34 | A system field or directive value is not upper case. |
| E-36 | An `@VERSION` constraint the toolchain cannot satisfy. |
| E-37 | `+N` or `\|N` applied to a replication. |
| E-38 | A harness type assigned to a bare pin range, or a member list whose length does not match the range. |
| E-39 | Division by zero in a substitution. |
| E-40 | An integer operand given to a boolean operator. |
| E-41 | A dimensioned value, string or list used as an arithmetic operand. |
| E-42 | A negative exponent. |
| E-43 | A `part` declaration exports a field. |
| E-44 | A cable holds a part that is not a cable connector, a wire or a crimp. |
| E-45 | A `@mate` names a cable whose connectors do not fit this one. |
| E-46 | Mating pins do not line up: a count mismatch, or a `@map` naming a pin that does not exist. |
| E-47 | Two pins that both drive are joined through a cable. |
| E-48 | A supply and a ground are joined through a cable. |
| E-49 | A connector disagrees with the element before it: `==` after an element that passes through, or `=` after one with no far side. |
| E-UNANNOTATED | An instance still carries `?` when the netlist is built. |

`E-UNANNOTATED` is the one diagnostic outside the numbered space, because it is
a property of the build rather than of the design. §13.1 requires that an
un-annotated design compile, link and check completely, and it must: `manta
annotate` takes its assignments from a `.mantaNets`, so linking has to succeed
before annotation is possible at all. Shipping a netlist of path-derived
identities is a different matter, so the diagnostic is an error by default and
is demoted for the one link that bootstraps a design:

```
manta link --top board -L build/ -Wno-unannotated -o build/board.mantaNets
manta annotate -n build/board.mantaNets src/*.manta
manta link --top board -L build/ -o build/board.mantaNets
```

The absence of the flag on the second link is what establishes that every
instance carries a designator. `--warn=unannotated` demotes it to a warning
instead. It is checked by the linker rather than by ERC, and so is unaffected by
`--no-erc`.

A **block** instance is covered as well as a device. It has no designator of its
own to appear in a BOM, but its label names a level of the hierarchy and so
appears in the instance path of every component beneath it — and from there in
the netlist, in the BOM and in whatever a layout tool calls the part. A
component exported as `BLK?7_R1` is no more shippable than a bare `?`.

### 16.2 Warnings

| Code | Rule |
|---|---|
| W-01 | A declared part has pins appearing in no chain and no binding. Catches unused sections of a multi-unit package. |
| W-02 | Both pads of a two-terminal device land on one net without a multi-pin terminal saying so. |
| W-03 | A capacitor is in series with two non-ground nets. |
| W-04 | A `&TYPE=POWER<` pin has no capacitor on its net within two nodes. |
| W-06 | A `~`-weak field is never overridden anywhere in the design. |
| W-07 | Two identifiers in one design differ only by `-` versus `_`. |
| W-08 | A swap group's members carry incompatible directives, so the group is frozen. |
| W-09 | A `&TYPE=POWER>` net has no consumers. |
| W-TYPE | A `@type` value is not a structural role but is within one edit of one (§9.7). |
| W-FOOTPRINT | A footprint reaches a layout tool with no library nickname. Export only. |

Every warning above is enabled by default except **W-06**, which is enabled with
`-WW-06` or `-Wweak-never-overridden`. A part library declares `@~footprint`
weakly on purpose — §9.2 makes a weak field "a suggestion", not an omission — so
on by default it would fire on every part in §20.1 and §20.2 and bury the
findings that matter. `-W<name>` in §15.5 exists for exactly this.

W-03 and W-04 both require recognising a capacitor, which is not a language
construct: parts are opaque and nothing marks one as capacitive. An
implementation shall document how it identifies one. The reference
implementation treats a part as a capacitor when it carries `@type = capacitor`
— the convention §9.7 establishes — or when
it is two-terminal with a `#value` dimensioned in farads. Neither warning fires
on a part it cannot classify.

### 16.3 Do-not-populate interaction

A part with `@fitted=FALSE` leaves its net open in that build. The checker shall treat nets
downstream of an unfitted series part as intentionally open and suppress **E-02** across
that boundary.

DNP affects the BOM and ERC only. The netlist is unchanged: the footprint is placed, the
pads exist, and the copper is routed.

---

## 17. Formatting

`manta fmt` is normative, and it manages indentation only. Line structure is the
author's: a binding list written across five lines stays five lines. The formatter
never:

- joins or splits lines
- adds or removes blank lines
- adds or removes a `;`, since statement boundaries are semantic (§11.2)
- reorders, respaces or realigns anything within a line
- alters case, comments, values, or the order of declarations

The only bytes it rewrites are each line's leading whitespace, plus the file-level
normalisation of §1.4: line endings become LF, and a file that lacks a final newline
gains one.

### 17.1 The depth rule

The indent unit is four spaces; tabs are not emitted. A line's depth is the number of
`{`, `(` and `[` still open at the start of the line, counted over tokens — a brace
inside a string literal or a comment does not count — and each line is indented one
unit per depth.

Three refinements:

- A line whose first token is `}`, `)` or `]` outdents to the matching open's depth,
  which is the rule that puts `};` level with its opener.
- A **continuation line** indents one unit past the depth. A line continues when it
  begins inside an unfinished unit at the current depth: a unit — a statement, a
  binding, or a binding-list head — becomes unfinished at its first token and
  finishes at `;` or `:`; an opening bracket suspends it (the bracket's contents
  take their depth from the bracket instead) and the matching close resumes it,
  still unfinished until its own terminator.
- A `--- TITLE` section marker (§4.7) takes plain depth, like any statement.

### 17.2 Comments and blank lines

A line whose first content is a comment is indented to the current depth. Interior
lines of a multi-line block comment — every line after the one carrying the `/*` —
are reproduced byte for byte, so aligned comment art survives. A blank line stays
blank: no indentation, no trailing whitespace.

The end-of-content marker (§2.8) and everything after it are reproduced byte for
byte, line endings included.

### 17.3 Invariants

`manta fmt --check` exits non-zero if any file differs from its formatting, and
writes nothing. The formatter refuses a file that does not parse: the depth rule
leans on bracket balance, and rewriting a broken file would disturb the text its
author needs to fix. Formatting is deterministic and idempotent, and a sensibly
indented file is already canonical — the formatter changes nothing at all on it.

---

## 18. Symbol reference

| Symbol | Meaning | Section |
|---|---|---|
| `=` | connect, advance node | 6.2 |
| `==` | continue on the node, after a dead-end element | 6.3 |
| `^` | adjacency, no connection | 6.4 |
| `=*` | gather array to scalar | 6.5 |
| `*=` | broadcast scalar to array | 6.5 |
| `{ }` | device instance | 7.1 |
| `~` | binds instance to part or block; weak strength; xor inside `$…$` | 7.2, 9.2, 14.3 |
| `?` | unassigned designator; unbind as `&NET=?` | 13.2, 11.6 |
| `!` | DNP prefix; locked strength; not inside `$…$` | 7.5, 9.2, 14.3 |
| `:` | introduces a binding list | 7.4 |
| `;` | statement and binding terminator | 2.7 |
| `,` | list element separator | 3.7 |
| `.` | casual terminal; harness member access; pin reference | 7.3, 12.1, 5.2 |
| `( )` | grouping | 6.7 |
| `[ ]` | array range; list literal | 8.1, 3.7 |
| `[[ ]]` | replication | 8.3 |
| `+N` `\|N` `*N` | series, parallel, node multiplicity | 8.6 |
| `%` | per-copy value; designator range | 8.5, 13.3 |
| `#` | user field | 9.1 |
| `@` | system field | 9.1 |
| `&` | directive, on a net, a pin or an instance | 11.1 |
| `>` `<` `<>` | port direction | 10.1 |
| `>>` | global scope | 10.3 |
| `$ $` | substitution | 14.2 |
| `.[a,b]` | ordered harness member list | 12.2 |
| `//` `/* */` | comments | 2.5 |

---

## 19. Grammar

```ebnf
file            = { item } ;
item            = block_def | part_def | harness_def | netclass_def | match_def
                | cable_def ;

block_def       = [ linkage ] "block" identifier
                  "{" { item | statement | section_marker } "}" ";" ;
part_def        = [ linkage ] "part" identifier "{" { field_decl | pin_map } "}" ";" ;
harness_def     = "harness" identifier "{" { member_decl | directive } "}" ";" ;
netclass_def    = "netclass" identifier "{" { directive } "}" ";" ;
match_def       = "match" identifier "{" { field_decl | match_def } "}" ";" ;
cable_def       = [ linkage ] "cable" identifier "{" { item | statement } "}" ";" ;
linkage         = "static" ;

pin_map         = pin_spec ":" pin_name [ arrow ] { directive | field_decl } ";" ;
pin_spec        = integer | "[" integer ":" integer "]" ;
pin_name        = identifier [ "[" range "]" ]
                | identifier "." "[" identifier { "," identifier } "]" ;
member_decl     = identifier [ arrow ] { directive } ";" ;

statement       = [ "extern" ] ( chain | field_decl | port_decl )
                  { directive } ";" ;
section_marker  = "---" title ;

chain           = segment { "^" segment } ;
segment         = element { connector element } ;
connector       = "=" | "==" | "=*" | "*=" ;
element         = net_expr | device | group | replication ;

(* Constraint (§6.2, §6.3): after an element that passes through -- a net,
   a device written with an exit terminal, a "+N"/"|N" group, a replication
   whose unit passes through -- the connector is "=". After an element with
   no far side -- a shunt, a one-pin attachment, a "*N" group -- it is "==".
   A connector that disagrees with the element before it is E-49. A binding
   opens with "=", "=*" or "*=": a pin passes through (§7.4). *)

group           = "(" segment ")" [ multiplicity ] ;
multiplicity    = ( "+" | "|" | "*" ) integer ;
replication     = "[[" segment "]]"
                | "[" integer "[" segment "]" integer "]" ;

device          = [ entry_terminal ] "{" instance "}" [ exit_terminal ] ;
instance        = [ "!" ] designator [ "~" identifier ]
                  [ ":" binding { ";" binding } [ ";" ] ] ;
designator      = identifier ( "?" | integer | desig_range ) ;
desig_range     = "%" "[" desig_part { "," desig_part } "]" ;
desig_part      = integer [ ":" integer ] ;
entry_terminal  = "." { "." }
                | ( "[" identifier { "," identifier } "]"
                  | identifier [ "[" range "]" ] ) "." ;
exit_terminal   = "." { "." }
                | "." ( "[" identifier { "," identifier } "]"
                      | identifier [ "[" range "]" ] ) ;
binding         = "." pin_ref [ connector segment | "=" "?" ] { directive | field_decl }
                | field_decl | directive ;
pin_ref         = [ identifier [ "[" range "]" ] ] ;

net_expr        = [ arrow ] net_name [ arrow ] ;
net_name        = identifier { "." identifier } [ "[" range "]" ]
                | identifier "." "[" identifier { "," identifier } "]"
                | "%" identifier [ "[" range "]" ]
                | "%" "[" value { "," value } "]" ;
port_decl       = net_expr | "[" net_expr { "," net_expr } "]" arrow ;
arrow           = ">" | "<" | "<>" | ">>" ;

range           = index [ ":" index ] ;
index           = integer | interpolation ;

field_decl      = [ ">" ] ( "#" | "@" ) [ "~" | "!" ] identifier [ ">" ]
                  "=" value ;
directive       = "&" [ "~" | "!" ] identifier [ "=" ( value | match_ref ) ] ;
match_ref       = "{" identifier ":" field_decl { ";" field_decl } [ ";" ] "}" ;

value           = integer | dimensioned | string | boolean
                | identifier | interpolation | list ;
list            = "[" value { "," value } "]" ;

interpolation   = "$" expr "$" ;
expr            = or_expr ;
or_expr         = xor_expr { "|" xor_expr } ;
xor_expr        = and_expr { "~" and_expr } ;
and_expr        = eq_expr { "&" eq_expr } ;
eq_expr         = rel_expr { ( "=" | "!=" ) rel_expr } ;
rel_expr        = add_expr { ( "<" | ">" | "<=" | ">=" ) add_expr } ;
add_expr        = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr        = pow_expr { ( "*" | "/" ) pow_expr } ;
pow_expr        = unary [ "^" pow_expr ] ;
unary           = [ "-" | "!" ] atom ;
atom            = integer | boolean | identifier
                | '"' identifier '"' | "(" expr ")" ;
```

`section_marker` is line-oriented, exactly as the end-of-content marker of §2.8: the
`---` shall be first on its line and `title` is free text running to the end of it
(§4.7).

---

## 20. Worked examples

### 20.1 A resistor part

Every concept in a part definition: linkage, fields at three strengths, pin map, casual
pins.

```
static part R-10k-1pct-0603 {
    @~footprint = R-0603;
    #value      = 10kR;
    #tolerance  = ±1%;
    #power      = 100mW;
    @!type      = resistor;
    #~mpn       = "RC0603FR-0710KL";
    #~cost      = 0.002;

    1: A &CASUAL;
    2: B &CASUAL;
};
```

### 20.2 A microcontroller part

Pin ranges, harness member lists, pin types, default nets, swap groups, pin delay.

```
part cool-mcu {
    @VERSION       = 1.0+;
    @~footprint    = QFP-STM32-32;
    #value         = STM32F0QA5;
    #!manufacturer = "ST Microelectronics";
    >#~source      = digikey;

    1:       VCC<        &TYPE=POWER &~NET=3V3;
    2:       GND<        &TYPE=POWER &~NET=GND;
    [3:11]:  GPIO[1:9]<> &SWAP=gpio-bank;
    [12:13]: USB.[+,-]<> &~PINDELAY=8ps;
    14:      SDA<>       &TYPE=OPENDRAIN;
    15:      SCL<        &TYPE=OPENDRAIN;
    16:      NC          &TYPE=NC;
};
```

### 20.3 A quad buffer with ganged swap

One permutable index shared by two arrays.

```
part buffer4 {
    @~footprint = TSSOP-14;
    #value      = SN74LVC125A;

    [1:4]:  IN[1:4]<   &SWAP=ch;
    [5:8]:  OUT[1:4]>  &SWAP=ch;
    13:     VCC<       &TYPE=POWER &~NET=3V3;
    14:     GND<       &TYPE=POWER &~NET=GND;
};
```

### 20.4 Harness types

Type declaration, assignment, harness-carried directives, the built-in `diff`.

```
harness i2c-bus {
    SDA<> &TYPE=OPENDRAIN;
    SCL<  &TYPE=OPENDRAIN;
};

harness usb2 {
    D &HARNESS=diff;
    &!IMP     = 90RD;
    &MAXDELAY = 600ps;
};
```

### 20.5 Net classes and matching

```
netclass power {
    &CURRENT  = 3A;
    &!VOLTAGE = 60V;
};

match ddr-addr {
    @src       = U1;
    @dest      = [U5, U6, U7];
    @tolerance = (5ps)*3;
    match ddr-clk-pair {
        @src       = U1;
        @dest      = [U5];
        @tolerance = 2ps;
    };
};
```

### 20.6 A reusable block

Ports with mandatory arrows, a chain with a shunt, a parameterised value through
substitution.

```
block rc-filter {
    #~r-value = 10;
    #~c-value = 100;

    >IN;
    OUT>;

    IN = .{R?~$"r-value"$kR-0603}.
      = .{C?~$"c-value"$nF-0603: .=GND}
      == OUT;
};
```

Instantiated twice with different parameters, producing two elaborations from one source:

```
>SIG-A = {BLK?~rc-filter: #r-value=10; }.OUT = FILTERED-A>;
>SIG-B = {BLK?~rc-filter: #r-value=47; #c-value=10; }.OUT = FILTERED-B>;
```

### 20.7 A complete board

Every remaining concept: globals, ground declaration, all five connection operators,
multiplicity of each kind, replication with per-copy values, width change, DNP, a
cross-object reference, multi-unit references, directives at three strengths, and a stub.

```
block power-and-signal {
    @VERSION     = 1.0+;
    #description = "3V3 buck with switched amplifier rail";
    >#author     = TJM;
    #!>board-rev = C;

    // ---- rails and grounds ------------------------------------------
    GND  &TYPE=GROUND;
    AGND &TYPE=GROUND;
    AGND = .{FB1~BLM18PG}. = GND;

    >>VIN;
    3V3>>;
    GND>>;

    3V3 &CLASS=power;

    // ---- buck output stage ------------------------------------------
    >nPWR-EN;

    SW = SW-NODE
        = (.{L?~MT100UFA}.)+2
        = (A.{D?~DI3643}.K)|2
        = ({C?~100nF-0603: .=GND}.)*4
        == ({C?~10uF-0805: .=%[GND,AGND,GND]}.)*3
        == 3V3
        = S.{Q?~FFET123: .G=nPWR-EN; }.D
        = PWR-SWITCHED
        &CURRENT=3A &!VOLTAGE=6V &~LAYER=inner1;

    // ---- microcontroller --------------------------------------------
    i2c &HARNESS=i2c-bus;
    USB &HARNESS=usb2;

    {U1~cool-mcu:
        .GPIO[1:9] = IO[1:9];
        .SDA       = i2c.SDA;
        .SCL       = i2c.SCL;
        .USB       = MCU-USB;
        .NC        = ?;
    };

    i2c<>;
    <>USB.+ = .{R?~50R-0603}. = MCU-USB.+;
    <>USB.- = .{R?~50R-0603}. = MCU-USB.-;

    // ---- enable RC ---------------------------------------------------
    {U1}.GPIO[1] = .{R?~50R-0603}.
                = .{C?~100nF-0603: .=GND}
               == nPWR-EN;

    // ---- single amplifier --------------------------------------------
    >SIG-IN = I.{U?~AMP012: .PWR=PWR-SWITCHED; }.O = AMPED-SIG> &IMP=50R;

    // ---- replicated amplifiers with per-copy enables ------------------
    >SIG2-IN[0:3]
        = [[ I.{U?~AMP012: EN=%AMP-EN[0:3]; PWR=PWR-SWITCHED; }.O ]]
        = IN.{U?~buffer4: .VCC=PWR-SWITCHED; }.OUT
        = AMPED-SIG2[0:3]> &MATCH=ddr-addr;

    // ---- passive summing network, gathered ---------------------------
    >SIG3-IN[0:3] = [[.{R?~100kR-0603}.]] =* COMBINED-OUT>;

    // ---- broadcast a reference to a bus ------------------------------
    VREF *= BIAS[0:7];

    // ---- a mated connector pair, not internally connected ------------
    PANEL-OUT = 1.{J?~conn-4}  ^  {J?~conn-4}.1 = PANEL-RETURN;

    // ---- unpopulated in this build -----------------------------------
    {!R?~0R-0603};

    // ---- test point ---------------------------------------------------
    TP1 = U1.GPIO9 &STUB;

    // ---- a part instantiated in another object -----------------------
    extern U9.1 = GND;

    // ---- multi-unit package ------------------------------------------
    SIG-A-IN = INA.{U20~LM324: .v-pos=3V3; .v-neg=GND; }.OUTA = SIG-A-OUT;
    SIG-B-IN = INB.{U20}.OUTB = SIG-B-OUT;
    SIG-C-IN = INC.{U20}.OUTC = SIG-C-OUT;
    SIG-D-IN = IND.{U20}.OUTD = SIG-D-OUT;
};
```

### 20.8 Building it

```
manta compile -o build/ src/*.manta
manta link --top power-and-signal -L build/ -L /usr/share/manta/lib \
           --bom build/bom.csv -o build/board.mantaNets
manta annotate -n build/board.mantaNets --swaps src/*.manta
manta export --format kicad -o build/board.net build/board.mantaNets
```

Checking without emitting, in continuous integration:

```
manta fmt --check src/*.manta
manta compile -o build/ src/*.manta
manta check --top power-and-signal -L build/ -Werror
```
