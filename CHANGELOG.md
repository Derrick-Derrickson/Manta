# Changelog

## Unreleased

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
