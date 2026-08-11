# Specification notes

Revision 1.0 leaves some points underdetermined, and in a few places its worked
examples contradict its own rules. Every such point is resolved here, in one
place, so a reader can tell what the implementation decided and why.

The resolutions fall into two kinds. Where the **grammar's wording is narrower
than the language it describes**, the implementation accepts the wider language
and the wording should be corrected. Where an **example is simply wrong**, the
implementation follows the rules and the example is corrected.

---

## A. Grammar wording that is narrower than the language

These are not extensions. The rules elsewhere in the specification, and the
constructs the examples depend on, both require the wider reading; only the
production is written too tightly.

### A1. A terminal may carry a range

The grammar gives `terminal = "." | identifier [ "[" index "]" ]`, a single
index. But a terminal's width is what gives a replicated unit its arity, so a
multi-output unit cannot be expressed at all without a range there:

```
[4[ I{U?~splitter}O[0:1] ]8]      // 1-in 2-out unit: 4 copies, 8 out
```

**Resolution.** A terminal accepts a range. The production should read
`terminal = "." | identifier [ "[" range "]" ]`.

### A2. A bracketed index may select one wire

The grammar defines `range = index ":" index`, admitting no one-element form.
But single indices appear throughout: `GPIO[$n$]` as a substitution position,
`DQ[0]` as a pin-delay override, `{U1}GPIO[1]` as a terminal.

**Resolution.** `[i]` is accepted and means the one wire `i`. It is recorded
distinctly from `[i:i]` so tooling can reproduce whichever was written. The
production should read `range = index [ ":" index ]`.

---

## B. Errors in the worked examples

In each of these the rules are unambiguous and the example contradicts them. The
implementation follows the rules; the example is corrected in `tests/spec`.

### B1. Op-amp supply pins named `V+` and `V-`

The multi-unit package examples bind the supply pins of an LM324 as `V+` and
`V-`. Neither is producible by the identifier grammar: a name may not end in a
hyphen, and `+` is not an identifier character at all.

**Resolution.** The rule stands unmodified — a trailing hyphen is an error
wherever it appears, including on a pin. The part in `tests/spec/lib.manta`
names the rails `v-pos` and `v-neg`, which the identifier grammar produces
cleanly and which read better anyway.

### B2. An instantiation written without braces

Every device in the grammar is wrapped in braces. But the complete-board example
declares an unfitted resistor as a bare statement:

```
!R?~0R-0603;
```

**Resolution.** The rule stands: a device is always braced. The example is
corrected to `{!R?~0R-0603};`, which parses as a device with no terminals — it
declares the part and connects nothing, which is exactly what was meant.

### B3. Terminals naming pins the part does not declare

The complete-board example instantiates the quad buffer with terminals `I` and
`O`. The quad buffer, declared earlier in the same document, has pins `IN`,
`OUT`, `VCC` and `GND`. There is no `I` and no `O`.

**Resolution.** This is reported as **E-31**, which is the correct response to a
terminal naming a pin the part does not have. No prefix matching is introduced:
it would silently accept `I` where `IN` was meant on a part that has both. The
example is corrected to `IN{U?~buffer4}OUT`.

### B4. An unquoted hyphenated field name inside a substitution

The reusable-filter example declares `#~r-value = 10` and then writes
`.{R?~$r-value$kR-0603}.`. But a hyphen inside `$…$` is *always* subtraction,
and a field name containing one must be quoted — so as written this is the field
`r` minus the field `value`, and both are undefined.

**Resolution.** The rule stands, and the example is corrected to `$"r-value"$`.
An actual subtraction is conventionally written with spaces — `$r - value$` — so
the two readings are never confusable on sight.

---

## C. Rules whose inputs the specification does not define

### C1. What counts as a capacitor — W-03 and W-04

Two warnings depend on recognising a capacitor:

- **W-03** — a capacitor is in series with two non-ground nets.
- **W-04** — a `&TYPE=POWER<` pin has no capacitor on its net within two nodes.

Nothing in the language marks a part as capacitive; parts are opaque.

**Resolution.** A part is treated as a capacitor when either holds:

1. it carries a `@type` field whose value is `capacitor` — the convention the
   specification itself establishes when it writes `@!type = resistor`; or
2. it is two-terminal and carries a `#value` dimensioned in farads.

Both warnings are individually suppressible (`-Wno-W-03`, `-Wno-cap-in-series`).
Neither ever fires on a part manta cannot classify, so a design adopting no
convention loses two warnings rather than gaining false ones.

### C2. What counts as "driven", for E-02

E-02 is "a net has an input and no driver". Taken to mean only that a `>` pin is
present, it fires on every pull-up, every pull-down, every divider and every
enable tied to a rail — which is to say on every real board, making it useless.

**Resolution.** A net is treated as driven when any of three things is true:

1. it has a `>` output pin;
2. it has a `&TYPE=POWER>` supply pin — an enable tied to a rail is tied, not
   floating; or
3. it has a `PASSIVE` pin, meaning something is attached that the checker cannot
   reason about, which is exactly what a pull-up or a filter looks like from
   here.

What is left is the case the rule exists for: a net consisting of nothing but
input pins, which is an input somebody forgot to connect. That still catches
every such net in the specification's own §20.7 excerpt.

### C3. Where `annotate --swaps` reads its input

The specification requires that a router exchanging two members of a swap group
has that exchange written back to source, but gives `manta annotate` only
`-n <netlist>`. No file in the pipeline carries swap decisions.

**Resolution.** `.mantaNets` carries an optional top-level `swaps` array, and
`--swaps` reads it from there. The JSON Schema marks it optional, so a netlist
produced without it still validates and `--swaps` becomes a no-op.

### C4. Export dialects

`--format` names four targets — `kicad`, `altium`, `orcad`, `allegro` — without
specifying a dialect for any of them.

| Target | Format written |
|---|---|
| `kicad` | KiCad S-expression netlist (`.net`) |
| `altium` | Protel/Altium netlist: `[` component `]` blocks, `(` net `)` blocks |
| `orcad` | OrCAD PCB II flat netlist |
| `allegro` | Allegro Telesis: `$PACKAGES` / `$NETS` |

Directives the target format cannot carry go to the `--constraints` sidecar
rather than being dropped silently.

### C5. Which pin a netlist entry names

The netlist example shows a physical pin number for one component
(`{"designator": "U1", "pin": "1"}`) and a logical pin name for another
(`{"designator": "C1", "pin": "A"}`).

**Resolution.** Both are emitted: `pin` is the physical package pin, which is
what a layout tool needs, and `logical` is the name the part declares. Each
entry also carries `type` and `direction`, which no other part of the
interchange records and which a layout tool needs: KiCad puts them on the pad
and its design-rule check reads them. Both are optional, so a netlist written
before they existed still validates.

### C6. Footprint names a layout tool can resolve

A part says `@~footprint = R-0603`, which names a package. KiCad resolves a
footprint as `Library:Footprint` against its footprint library table, and a bare
name either fails to place or places with a warning on every later update. The
specification says nothing about how one becomes the other, and it should not:
the four export targets name footprints differently, and a part library that
hard-codes any one of them stops serving the other three.

**Resolution.** The translation lives beside the design, not in the part.
`manta export --footprint-map <file>` reads a table of `name  Library:Footprint`
pairs, and resolution takes the first of these that applies:

1. an entry in the map file;
2. otherwise the raw name, if it already names a library;
3. otherwise `<--footprint-lib>:<raw>`, when a default nickname was given;
4. otherwise the raw name, and `W-FOOTPRINT`.

The map is only a rename table, so it applies to every target. The default
nickname and the warning are KiCad's alone, because only KiCad resolves
`Library:Footprint`. A map entry that names no library is refused outright: it
would produce exactly the unresolvable name the file exists to prevent.

`W-FOOTPRINT` is a warning and not an error because the netlist is still worth
having — `-Werror=footprint` is how a project refuses to ship one that will not
place. It is reported once per distinct footprint, not once per component.

The map cannot translate a *pinout*, only a name. A footprint whose pads are
`A1` and `B1` will not serve a part declaring pins `1`..`4`, and choosing one
that does is a question about the design rather than about export.

### C7. Component identity across a re-import

A layout tool matches a netlist component to a footprint already on the board
either by reference designator or by UUID. Manta emitted no UUID, so only the
first was possible, and re-annotating a design orphaned every placement.

**Resolution.** The KiCad netlist carries an RFC 4122 version 5 UUID per
component and per sheet, derived from the component's **instance path** rather
than its designator — which is the point: renaming `U3` to `U7` must not move
the part, and moving a part between blocks must.

Version 5 rather than a scheme of manta's own, so the UUID for a given path can
be recomputed by anyone without reading manta's source. The namespace UUID is
`ae130b25-7266-4fdc-8da0-a1330e189542`, generated once and frozen; changing it
would orphan every footprint manta has ever placed.

Nothing here is random or clock-derived, so spec 15.8 still holds: the same
design exports byte-identically every time.

### C8. Which name a net's pin entry uses for its component

A designator is unique only within its block, so instantiating a block twice
gives two components called `R1`. A net's pins name their component by one
string and nothing else, and a reader given `R1` twice cannot tell them apart —
so every pin of the second copy is lost.

**Resolution.** `.mantaNets` and the BOM name a component by its flattened
instance path whenever it is nested, which §13.4 already calls "the single
unique string a BOM and a layout tool require". The local designator is not
lost: a component entry also carries its `path`, whose last element is exactly
that.

### C9. What a part is, and how far the set is closed

`@type` had to interpret `boardconnector` and `wire` while leaving `resistor`
and `ferrite` alone. A closed set would reject the classifications a design
legitimately carries; a wholly open one would let `boardconector` silently
disable every mating check.

**Resolution.** The set is open. Five values are structural and are interpreted;
anything else is an ordinary classification, carried to the BOM untouched. A
value that is not structural but is within one edit of one, or matches one after
case folding, is warning **W-TYPE** — a typo cannot be an error when the set is
open, but its consequence is silent, and that is worth a word.

`@type` is a system field yet is emitted in the netlist, given a BOM column and
exposed to rules as `component.type`. Without that, moving `type` out of the `#`
namespace would have removed it from both, and the project rules that check
mating depend on it.

### C10. Which end of a loom is plugged in where

A cable with two identical housings mates with a board connector at either end,
and nothing in the source distinguishes them.

**Resolution.** The first connector in elaboration order is taken as the near
end and the other as the far end. They are interchangeable by construction — two
identical housings on one loom — so there is nothing to choose between them, and
the choice is deterministic.

Deciding the far end *on the board* is the same question and is not: the far end
is the first board connector, other than the one the loom is already plugged
into, whose part the far housing names in `@mates`. A board with two identical
outgoing sockets and one loom is therefore resolved arbitrarily but consistently.
Where that matters, the two sockets are different parts.

### C11. How far a conductor is traced

**Resolution.** One hop. A conductor is followed from a board pin, through the
loom's wires and crimps, to the pin at the far end, and no further. When the far
end plugs back into this same board — a board plugged into another copy of
itself — that is enough to see the whole loop, because the second copy is the
same design.

What is deliberately *not* done is to treat the chain as N cards deep. A signal
that passes straight through and would collide with itself at the next hop is
not reported. That check needs a policy about what a chain is for, which belongs
in a project's rules rather than in the language.

One consequence worth stating: a net whose only driver is on the next card
reports **E-02** on this one, because ERC sees a board and the driver is not on
it. That is honest — the board alone does have an undriven input — but it means
a daisy-chained design carries `-Wno-E-02` or a `&STUB` on its uplink nets.

### C12. Which name a flat net carries

A net may be spelled differently in different scopes, and instantiating a block
twice gives two nets both locally called `LED-ANODE`. The flat netlist needs one
name per net and no name twice: KiCad and every other importer merge nets **by
name**, so a duplicate would short two separate conductors on the real board.

**Resolution.** A net's flat name is its outermost-scope spelling, first seen at
that depth in source order, unprefixed. Only a net whose every spelling lives
inside child instances takes a flat prefix — instance path plus local spelling,
`BLK1.LED-ANODE` — mirroring how designators flatten (§13.4). Pin-derived
fallback names use the flat designator for the same reason: two instances of one
block both hold an `R1`, and `R1.2` twice is the same short. The block records'
`localNets` still carry the local spellings; displaying them is what they are
for.

---

## D. Smaller points

### D1. `±` is not ASCII

Source outside comments and string literals is said to be ASCII, yet tolerances
are written `±1%`. `±` (U+00B1) is accepted, as is the `+-` spelling offered
alongside it. `µ` remains rejected, as stated explicitly, and is diagnosed by
name rather than as a generic bad character.

### D2. Decimal values

Only integers are defined, but `#~cost = 0.002` appears. Decimal values are
accepted wherever a value is expected. Arithmetic inside `$…$` remains
integer-only, and a decimal used as an operand there is E-41.

### D3. Reserved diagnostic codes

E-03, E-16, E-19, E-35 and W-05 appear nowhere in the specification. They are
reserved in the diagnostic table and never emitted.

### D4. W-06 is off by default

The common options list `-W<name>` as "Enable warning `<name>`", which only means
something if some warnings begin disabled. **W-06** — a `~`-weak field never
overridden anywhere in the design — is the one that needs it. A part library
declares `@~footprint` weakly on purpose, so on the specification's own part
examples this would fire on every part and drown the findings that matter.

It is enabled with `-WW-06` or `-Wweak-never-overridden`, and answers a real
question when asked: which suggestions did nobody take? Every other warning is
on by default.

### D5. Un-annotated designators are an error at link

The specification says an un-annotated design "shall compile, link and check
completely", and there is a good reason it has to: `manta annotate` reads its
assignments from a `.mantaNets`, so linking must work *before* annotation is
possible at all. Nothing in the netlist or in ERC depends on a designator
existing, and unassigned instances carry identities derived from their block
instance path.

That is right as a property of the language, but shipping a netlist full of
path-derived identities is not what anyone wants from a build. So **E-UNANNOTATED**
is an error by default, and demotable:

```sh
# Bootstrap: link once with the check demoted, so there is a netlist to
# annotate from.
manta link --top board -L build/ -Wno-unannotated -o build/board.mantaNets
manta annotate -n build/board.mantaNets src/*.manta

# From here on the flag is not needed, and its absence is what proves the
# design is fully annotated.
manta compile -o build/ src/*.manta
manta link --top board -L build/ -o build/board.mantaNets
```

`--warn=unannotated` demotes it to a warning instead, and `-Wno-unannotated`
silences it entirely, which is the setting a strictly conforming run wants.

This is a toolchain policy rather than one of the section 16 rules, so it is
checked in the linker and not in the ERC pass, and it runs even under
`--no-erc`.

### D6. A bare revision in `@VERSION`

The forms given are `1.2+`, `1.2-` and `0.2-1.2`, but not bare `1.2`. A bare
revision is accepted and pins exactly that revision, which is the reading that
makes the three documented forms a complete lattice.

### D7. Two rules share the code E-02

E-02 covers both "a net has an input and no driver" and "an identifier ends in
`-`". They are unrelated, and the second is lexical while the first is
whole-design. Both report E-02, with messages that distinguish them.

### D8. A dotted name is not a bare net name

`=` is required to have a device, group or replication on at least one side, and
two bare net names joined by `=` is E-22. But a pin reference such as `U3.OUT`
*is* a device terminal — a pin belongs to exactly one net, so the reference names
that net at that pin — and a harness identifier stands for its members. Both are
therefore exempt, which is what makes `TP7 = U3.OUT &STUB;`,
`extern U5.1 = GND;` and `USB = MCU-USB;` all well formed.

---

## E. Renderer decisions

`manta render` draws a netlist, and the specification says what a netlist means,
not how to draw one. Every choice the drawing depends on is recorded here. None
of them affects any other tool: the netlist is the truth and the drawing is a
view of it.

### E1. Which symbol a part is drawn as

Nothing in a netlist says "this is a resistor"; parts are opaque. Classification
takes three tiers, extending C1's convention: the `@type` field (an open set per
C9, matched case-insensitively), then the legacy `#type` user field, then — for
an untyped two-pin part — the unit of `#value`: ohms a resistor, farads a
capacitor, henries an inductor, hertz a crystal.

A classification word is honoured only when the pins can back it up: a diode
needs `A`/`ANODE` and `K`/`CATHODE`, a MOSFET gate/drain/source, a BJT
base/collector/emitter, an op-amp its two inputs, output and recognisable
supplies. When the word speaks but the pins cannot honestly carry the symbol,
the part draws as a generic box — and the later tiers do not reinterpret it,
because the word already said what the part is. A one-pin part is a test point;
anything unclassified is a box, which is never wrong, only plain.

### E2. What counts as a rail

Rail bars and pull-ups need to know which nets are supplies. A net is a rail on
evidence — a `&CLASS=power` directive, or a `&TYPE=POWER` source pin — or by
spelling, matched whole-name and case-folded against the page-local display
name: `3V3`-shaped names (`^\d+V\d*$`), the words `VBUS`, `VCC`, `VDD`, `VEE`,
`AVDD`, `VIN`, `VOUT`, `VBAT`, and `V-<word>`. Per page, because a block's local
`VCC` is a rail on its own page whatever its flat name became. A net sharing its
displayed spelling with a rail or ground net takes the same mark: on a
schematic, labels connect by name.

### E3. A block definition renders once

One page per block *definition*, drawn from the first instance in netlist
order — so the parameter values shown are that instance's — with every instance
listed on the page (`instances: BLK1, BLK2`) and each sheet symbol on the parent
linking to the one page. The alternative, a page per instance, is exactly the
duplication the hierarchy exists to avoid; where two instances differ, the
netlist has both and the page says whose values it shows.

### E4. There is no router

Placement is a fixed set of idioms — decoupling ladders against a rail bar,
pull-ups and pull-downs, chain runs — with one collision axis per element kind.
A wire that still cannot be placed is not drawn: the connection keeps its net
labels, which connect by name, exactly as on a hand-drawn schematic. A missing
wire is therefore never a missing connection; clicking either label still
highlights the whole net.

---

## F. Language design decisions

Where a revision adds a construct, the shape it took was a choice among workable
alternatives. The reasoning is recorded here, so a later reader can tell what was
weighed rather than re-deriving it.

### F1. A binding is a chain rooted at a pin

Revision 1.4 lets the right-hand side of a binding be a segment rather than a
single net name (§7.4). The problem is that the components belonging to a pin —
a decoupling capacitor, a feedback divider, a pull-up, a boot-strap cap — could
not be written where the pin is. Three shapes were available.

**Leave authors to hoist by hand.** Nothing is added to the language: each
supporting component goes in its own statement, reaching back to the pin by a
dotted reference. This already worked, and it is what the new form compiles to.
But it scatters one device's support across a body, and the scattered statements
are ordered by nothing. A regulator with six such pins reads as six unrelated
statements plus one instantiation, and the association a schematic makes obvious
is carried only by the designator spelling.

**A nested statement construct.** Give the binding list a statement grammar of
its own, so a binding could hold something statement-shaped with its own
terminator and its own directive scope. This is the general answer, and it is
too general: it introduces a second place where connectivity is written, with
its own scoping question at every level of nesting, and it invites a syntax for
things a binding has no business declaring — ports, fields at block scope,
sections. It also has no obvious stopping point, since a nested statement
containing an instance would nest again.

**Resolution.** A binding is a chain rooted at a pin of the enclosing instance,
and `PIN` *connector* *segment* is defined to mean what `D.PIN` *connector*
*segment*`;` means in the enclosing body. That definition is the whole feature:
it adds no scoping rule, no second connectivity syntax and no new kind of
element, only a position where an existing one may be written.

The equivalence is what makes it cheap. Because a binding's chain is exactly the
hoisted statement, directive scope needs no rule beyond the one §11.2 already
states — a statement is the scope unit, so a binding is its own scope and the
enclosing statement's directives do not reach into it. ERC sees ordinary nets
and ordinary pins, so E-22, E-26, E-27 and the rest apply unchanged. A device
declared inside a binding is an instance of the enclosing body, so it takes that
body's active render section (§4.7) and is annotated with everything else, with
no traversal that knows about bindings. `PIN = NET`, the only form legal before
1.4, is the degenerate case where the segment is one net element, and takes the
same path it always did.

The restrictions follow from the same equivalence rather than from taste. A
binding is one segment, so `^` — which partitions a *statement* into segments —
has nothing to partition and does not appear. All four connectors of §6 open a
binding because a segment admits all four. And `PIN = NET` does not trip E-22
for the reason §6.2 already gives: the left side is a pin reference, and a pin
is a device terminal.

The §19 production was widened at the same time to admit two forms the language
had always accepted and the grammar did not: a pin carrying directives or fields
with no right-hand side (`DQ[0] &PINDELAY=18ps`, §11.5) and the unbind
`PIN = ?` (§11.6). Both are of the kind recorded in section A — the wording was
narrower than the language — and are corrected here rather than listed there
because the production they belong to was being rewritten anyway.
