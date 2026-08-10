# Changelog

## 1.3.0 — 2026-08-10

### Language, revision 1.3

- **`--- TITLE` names a render section.** Inside a block body, the statements
  after a marker belong to the section it titles, until the next marker or the
  end of the block. The title is free text to the end of the line — `//` is not
  special in it — and a section is purely presentational: connectivity, ERC and
  the netlist's electrical content are untouched. At the top level a bare `---`
  is still the §2.8 end-of-content marker; inside a block it is an error, since
  a section needs a title. `manta fmt` writes a marker at body indentation with
  a blank line above.

### `manta render`: the netlist on a page

`manta render [-o out.html] [--title T] [--pdf out.pdf] <netlist.mantaNets>`
draws a clickable schematic: one self-contained HTML file, one SVG sheet per
block *definition*. A block instantiated N times draws once; the green sheet
symbols on the parent page all link to that one page, which lists its
instances and shows the first one's parameter values. Section markers become
titled rooms. Classic symbols — resistor, capacitor, inductor, diode, LED,
MOSFET, BJT, op-amp, crystal and more, chosen from `@type`, the legacy
`#type`, or the unit of `#value`, and only where the pin names can back the
symbol up — plus rail bars with their decoupling ladders, pull-ups and
pull-downs, chain runs, dark-red net labels, yellow port flags and ground
glyphs. Click a net to highlight it across pages, a component for its fields,
a sheet symbol to open its block's page.

The HTML is byte-deterministic, inside the §15.8 guarantee. `--pdf` prints it
through a headless Chromium found on PATH and is explicitly outside that
guarantee: the PDF is whatever the browser makes of the page.

The choices the drawing depends on — symbol classification, what counts as a
rail, representative instances, the no-router label fallback — are recorded in
`docs/assumptions.md` §E.

### Netlist additions, schema 1.3

All optional, so an older netlist still validates:

- **Per-component `pins` arrays**, in declaration order — a pin on no net,
  including `NC` pins, now survives into the interchange.
- **`section`** on components and block records, from the render sections.
- **A top-level `blocks` array** — path, block, section, resolved ports and
  block-local net spellings: the hierarchy a renderer rebuilds from.
- **`version` tells the truth.** It now names the language revision the
  toolchain implements, where it always wrote `1.0`.

### The linker connects what it always claimed to

Three shipped defects, each producing a quietly wrong netlist:

- **A chain terminal on a block or part reference united nothing.**
  `FEED = {B1~inner}IN` and `TX{U2} == TXD{J1}` both looked for the empty side
  of the join and silently connected nothing, so every port bound that way was
  left floating — in blinky, the entire indicator fan-out. The join is now
  real: a pin and its net are one (§5.2), and block port terminals also accept
  ranges and whole arrays as part terminals always did. Netlists gain
  connections that were always claimed in source.
- **`>>` never crossed a scope.** §10.3 says a global export is visible
  design-wide, but no cross-scope connection existed at all: a child block's
  `>>GND` still made a private one-pin net, and blinky's LED cathodes floated
  straight through `-Werror`. Every `>>NAME` in a design now unites on one
  representative per spelling. Blinky has exactly one GND, cathodes on it.
- **Two instances of one block emitted duplicate net names.** An importer that
  merges nets by name — KiCad does — would have shorted the two LED channels
  on the real board. A net's flat name is now its outermost spelling, and a
  block-local net flattens under its instance path as `BLK1.LED-ANODE`,
  the same pattern §13.4 requires of designators. Expect net names in
  netlists and exports to change accordingly.

`examples/blinky` now carries section markers, so `manta render` on it shows
the board as rooms — USB-C power in, regulator, MCU, I2C, indicators — and
`tests/example.cmake` asserts the cross-boundary connectivity that the first
two fixes restore.

## 1.2.0 — 2026-08-09

### Licence: GPL-3.0-or-later

manta is now free software. The evaluation licence 1.1.0 shipped under always
said this was the intent; this is following through on it.

What that means in practice: you may use, study, modify and redistribute manta
freely, and anyone who distributes a modified version has to offer its source on
the same terms. It does not reach someone who runs a modified manta behind a
network service without ever distributing a binary — that would need AGPL, and
remains available as a later choice.

**Your designs are not covered.** A netlist, a BOM or an exported file is your
design data, not a derived work of the compiler. manta embeds none of its own
code in what it emits.

### Language, revision 1.2

- **`cable`** is a declaration kind. Its body is a chain, exactly as a block's
  is, so replication and ranged designators keep an eight-way loom to a single
  statement. It may hold only a cable connector, a wire or a crimp (E-44).
- **A cable is its own deliverable.** `manta link --top <cable>` produces its
  netlist and its BOM, with wires and crimps as real line items. E-24 (a ground
  net) and E-20 (a footprint per part) do not apply to one.
- **`@type`** says what a part is: `board_part` by default, and the structural
  roles `boardconnector`, `cableconnector`, `wire` and `crimp`. The set stays
  open, so `@type = regulator` is ordinary and travels to the BOM untouched.
- **Mating.** A board connector declares `@mate = <cable>`; a cable connector
  declares `@mates = <part>`, with an optional `@map`. The compiler checks the
  fit (E-45, E-46) and, when the loom's far end plugs back into this same board,
  follows each conductor through it and applies the rules that would apply had
  the two been wired together directly (E-47, E-48) — which is how a board that
  plugs into another copy of itself is checked from one board's source.
- **`--assembly`** additionally writes a netlist and BOM for every mated cable,
  as separate files. The board's own outputs are byte-identical with and without
  it.
- **An area unit**, `m2`, so a wire's cross-section is a quantity a rule can
  check rather than a bare number.
- **Range values**, `1:20` inside a list, so a twenty-way `@map` is one pair.

Wire ampacity and "every connector must be mated" are deliberately not built in;
`examples/blinky/blinky.mantaRules` shows both as project rules.

### A KiCad netlist a board can be laid out from

`manta export --format kicad` produced a valid S-expression netlist that KiCad
could not actually use. Four things were in the way.

- **Footprints now name a library.** KiCad resolves a footprint as
  `Library:Footprint`; manta wrote bare package names, so components either
  failed to place or warned on every update. `--footprint-map <file>` maps
  package names to the target's, `--footprint-lib <nickname>` supplies a
  default, a name that already names a library passes through, and anything
  still unqualified is `W-FOOTPRINT`. The translation lives beside the design
  rather than in the part, so one part library still serves all four targets.
- **Nets carry pin function and type.** `(pinfunction …)` and `(pintype …)` are
  emitted per node, which KiCad puts on the pad and its design-rule check reads.
  This needed `type` and `direction` on each pin in `.mantaNets`, both optional,
  since the part declaration is not part of the interchange.
- **Components have a stable identity.** Each carries an RFC 4122 version 5
  UUID and a sheet path derived from its instance path, so re-annotating a
  design no longer orphans a placed footprint. Nothing random or clock-derived:
  export stays byte-identical, verified across x86-64 and ARM64.
- **Hierarchical components keep their connections.** A designator is unique
  only within its block, so two instances of one block both held an `R1` and a
  reader could not tell them apart — every pin of the second copy was silently
  lost. `.mantaNets` and the BOM now name a component by its flattened instance
  path, which §13.4 already required of "the single unique string a BOM and a
  layout tool require".

`examples/blinky` ships `blinky.fpmap`, and `tests/example.cmake` resolves every
footprint and every pin against KiCad's installed libraries when they are
present — the same lookup Pcbnew performs.

Recorded in `docs/assumptions.md` as C6, C7 and C8.

### An un-annotated designator now fails the build

- **A block instance written `BLK?` is E-UNANNOTATED.** The check only ever
  looked at `Component::designator`, and a block instance is not a component —
  so it slipped through and reached the netlist, the BOM and the layout tool as
  `BLK?7_R1`. It is an error at link, alongside the unassigned devices, and
  `-Wno-unannotated` still allows the one link that bootstraps a design, as
  §13.1 requires.
- **A range designator on a block resolves.** `instantiateBlock` handled only
  the `Numbered` form, so `BLK%[1:2]` — which §13.3 defines as the *annotated*
  form, one token carrying N designators — was treated as unassigned and became
  `BLK?2`, `BLK?3`. It now hands its members out one per copy, exactly as a
  device does, so `examples/blinky` exports `BLK1_R1` and `BLK2_R1` as
  `board.manta` has always said it should.

`tests/spec` declares a block and never instantiates one, which is how both
survived; `tests/pipeline.cmake` now instantiates one both ways.

## 1.1.0 — 2026-08-08

First release.

### Language, revision 1.1

- **End-of-content marker.** A line of exactly `---`, outside any declaration,
  ends the manta content of a file. Everything after it is documentation, never
  tokenised, and reproduced byte for byte by every tool — so a part can carry
  its datasheet below its declaration.
- **`#` fields on pins.** A pin map line may carry user fields alongside its
  directives, and they apply to every pin the line produces. This is what
  user-defined rules read.

A 1.0 source is a valid 1.1 source, and the toolchain reads any object whose
revision is no newer than its own.

Six editorial corrections to the specification as published are listed in the
note at the head of `doc/manta-language-specification.pdf`: two grammar
productions were narrower than the language they describe, and four worked
examples contradicted rules stated elsewhere in the same document.

### The compiler

All six subcommands — `compile`, `link`, `check`, `annotate`, `fmt`, `export` —
and all 47 diagnostics of specification §16, each with a conformance fixture.

- Two JSON artifact formats with published schemas.
- Four export backends: KiCad, Altium, OrCAD, Allegro.
- Byte-identical output for identical input, tested by running each stage twice
  — and, as of this release, verified identical between the x86-64 and ARM64
  builds.
- Zero third-party dependencies. One statically linked binary per platform.

### User rules

`.mantaRules` states checks the language cannot know about, over `#` fields a
design carries. Four domains: net, ordered pin pairs on a net, component and
part. Aggregates with unit checking, so comparing a current against a voltage is
an error in the rules file rather than a check that silently always passes.

A check's name is its diagnostic code, so a project's rules answer to `-Wno-` and
`--error=` exactly as a built-in does.

Because `#` is already the open namespace, a decorated design compiles and links
with no rules file present: rules are pure checking, never a build dependency.

### Additions beyond the specification

- **`E-UNANNOTATED`**, an error when an instance still carries `?` at link.
  §13.1 requires that an un-annotated design link — `manta annotate` reads its
  assignments from a netlist, so one has to exist — which is why the check is
  demotable with `-Wno-unannotated` for the bootstrap run.
- **`W-06` is off by default.** A part library declares `@~footprint` weakly on
  purpose. Enable with `-WW-06`.
- A `swaps` section in `.mantaNets`, for `annotate --swaps` to reconcile. The
  specification requires the reconciliation but gives the annotator nothing to
  read it from.

### Known gaps

- No Windows ARM64 binary: the toolchain is not in the usual package
  repositories. The source builds for that target unmodified.
- `annotate --swaps` is a no-op until a layout tool writes a `swaps` section.
