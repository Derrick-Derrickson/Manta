# manta

A compiler for the Manta Schematic Definition Language, specification revision 1.3.

Manta is a plain-text language for describing electronic schematics: the
components on one printed circuit board, their interconnections, the electrical
constraints on those interconnections, and the looms that plug into it.

```
block power-and-signal {
    GND &TYPE=GROUND;
    >nPWR-EN;

    SW = SW-NODE
        = (.{L?~MT100UFA}.)+2
        = ({C?~100nF-0603: .=GND}.)*4
       == 3V3
        = S{Q?~FFET123: G=nPWR-EN; }D
        = PWR-SWITCHED
        &CURRENT=3A &!VOLTAGE=6V;
};
```

## Building

Needs a C++23 compiler and CMake 3.24 or later. No third-party dependencies.

```sh
cmake --preset linux-release
cmake --build --preset linux-release -j
ctest --preset linux-release
```

Cross-compiling for Windows with mingw-w64 produces one standalone `.exe` with
no runtime DLLs to ship alongside it:

```sh
cmake --preset windows-release
cmake --build --preset windows-release -j
```

Cross-compiling for 64-bit ARM Linux needs `g++-aarch64-linux-gnu`:

```sh
cmake --preset linux-arm64-release
cmake --build --preset linux-arm64-release -j
```

Other presets: `linux-debug`, `linux-asan` (AddressSanitizer + UBSan).

`tools/make-pdf.py` renders a specification to PDF for a release, using
python-markdown and headless Chromium.

## Using it

The two-stage model of specification §15.1:

| Command | Input | Output |
|---|---|---|
| `manta compile` | one `.manta` source | one `.mantaO` object |
| `manta link` | many `.mantaO`, plus a top-level block or cable | one `.mantaNets` |
| `manta check` | the same | nothing; runs every stage including ERC |
| `manta annotate` | sources, plus a `.mantaNets` | rewritten sources |
| `manta export` | one `.mantaNets` | a layout tool's netlist |
| `manta render` | one `.mantaNets` | a clickable HTML schematic |
| `manta fmt` | sources | re-indented sources |

`link` and `check` also take `--rules <file>`: a `.mantaRules` file of
project-specific checks — logic-level compatibility, current budgets, library
policy — over `#` fields the design carries. Because `#` is already the open
namespace, a decorated design compiles with no rules file, so rules are pure
checking rather than a build dependency. See `docs/rules.md`.

A complete build:

```sh
manta compile -o build/ src/*.manta
manta link --top power-and-signal -L build/ --bom build/bom.csv -o build/board.mantaNets
manta annotate -n build/board.mantaNets --swaps src/*.manta
manta export --format kicad --footprint-map board.fpmap -o build/board.net build/board.mantaNets
manta render -o build/board.html build/board.mantaNets
```

`render` draws the netlist as a clickable schematic: one HTML file, one sheet
per block definition, with any `--- TITLE` section markers in the source as
titled rooms. Click a net to trace it across pages. `--pdf board.pdf` also
prints the sheets through a headless Chromium.

A loom is a separate thing to build, and links on its own:

```sh
manta link --top usb-c-1m -L build/ --bom build/lead.csv -o build/lead.mantaNets
```

A board connector says which loom plugs into it with `@mate`, and the linker
checks that the two fit — the pin counts, the map, and, when the far end plugs
back into another copy of this same board, what each conductor meets when it
gets there. `--assembly` additionally writes every mated loom's netlist and BOM
beside the board's, as separate files:

```sh
manta link --top power-and-signal -L build/ --assembly --bom build/bom.csv
```

And in continuous integration:

```sh
manta fmt --check src/*.manta
manta compile -o build/ src/*.manta
manta check --top power-and-signal -L build/ -Werror
```

An instance still carrying `?` when the netlist is built is an error. To
bootstrap a design that has never been annotated, demote the check for the one
link that produces the netlist `annotate` reads from:

```sh
manta link --top power-and-signal -L build/ -Wno-unannotated -o build/board.mantaNets
manta annotate -n build/board.mantaNets src/*.manta
```

From then on the flag is not needed, and its absence is what proves every
instance has a designator.

`manta <command> --help` documents each command's options. Every diagnostic can
be enabled, silenced or re-graded by code or by mnemonic: `-Wno-W-03` and
`-Wno-cap-in-series` are the same instruction.

## How it fits together

```
.manta ──lex──▶ tokens ──parse──▶ AST ──sema──▶ IR ──emit──▶ .mantaO
                                                                │
  .mantaO × N ──link: resolve ▸ elaborate ▸ substitute ▸ net-build ▸ ERC──▶ .mantaNets
                                                                │
                            ┌──────────────┬────────────────────┼──────────────┐
                        annotate         render               export           fmt
```

| Directory | Contents |
|---|---|
| `src/base` | Arena, string interner, insertion-ordered maps, union-find |
| `src/source` | File loading, BOM and CRLF handling, byte-offset spans |
| `src/diag` | `codes.def` — the one table every diagnostic is generated from |
| `src/lex` | Lexer and the SI dimensioned-value parser |
| `src/parse` | Recursive-descent parser for the §19 grammar |
| `src/sema` | Per-file checks; the closed field and directive registries |
| `src/obj` | `.mantaO` reading and writing |
| `src/link` | Symbol table, field scopes, substitution, elaborator, netlist, mating |
| `src/erc` | The §16 rules |
| `src/fmt`, `src/annotate`, `src/export` | The three source- and output-side tools |
| `src/render` | The schematic renderer: classification, rooms, layout idioms, SVG in HTML |

Longer notes live beside the code: `src/README.md` for the architecture,
`AGENTS.md` and `src/AGENTS.md` for the rules a change has to respect, and
`agents/` for skills covering how to write manta designs, author part libraries,
and extend this compiler.

Four decisions shape the implementation, and are documented where they live:

- **The lexer never resolves ambiguity.** `-5V` is a net name in one position
  and a value in another (§2.3), so the lexer classifies a whole lexeme and the
  parser picks the reading its position calls for. No backtracking anywhere.
- **A chain element evaluates to a *bundle*** of node handles, and the
  connectors of §6 are operations on bundles merged through a union-find. §6.3's
  uniform `==` rule — which shorts a two-terminal device written with `==` on
  both sides — falls out of that rather than needing a special case.
- **Substitutions travel unevaluated** in a `.mantaO` and are evaluated at
  elaboration (§14.1), because what a `$…$` resolves to depends on how the
  enclosing block was instantiated.
- **Determinism is structural.** Insertion-ordered maps rather than
  `unordered_map`, a hand-rolled JSON writer with fixed key order and no
  floating point, and a parallel compile that merges diagnostics in
  command-line order. §15.8 requires byte-identical output, and the test suite
  checks it by running each stage twice and comparing.

## Examples

`examples/blinky` is a complete board — USB-C in, a 3V3 regulator, an eight-pin
microcontroller, an I²C bus and two LEDs through a replicated block — plus the
USB-C lead that plugs into it, with its own wires and crimps. It passes
`fmt --check`, `check -Werror`, `link`, the mating checks and all four exports
with nothing to report, and `manta render` draws it as five titled rooms plus
one shared page for the indicator block. The specification's own worked example is an excerpt that does not
pass ERC, so this is what a clean build actually looks like. See
`examples/README.md`.

## Tests

```sh
ctest --preset linux-release
```

- `tests/unit` — lexer, SI values, parser, object round trip, and the
  conformance suite.
- `tests/diag` — one fixture per diagnostic code. Specification §1.3 requires an
  implementation to "implement all diagnostics in section 16, reporting the code
  given there"; `test_diagnostics` asserts each of the 39 numbered errors and 9
  warnings fires on its own fixture. The five mating errors need the full link
  driver, because a mating check compiles a *second* design, so they live in
  `tests/cable` and are driven through the binary instead.
- `tests/spec` — the worked examples of §20, with the four corrections listed in
  `docs/assumptions.md` §B and nothing else changed.
- `tests/pipeline.cmake` — the §20.8 build sequence end to end through the real
  binary, plus the §15.8 determinism checks and the guarantee that formatting a
  source does not change the netlist it produces.
- `tests/example.cmake` — `examples/blinky` under `-Werror` with no suppressions.
  The fixtures prove each diagnostic fires on a design that earns it; this
  proves none fires on a design that does not.

`docs/spec.md` is the specification, editorially corrected against this
implementation; the corrections are listed in its opening note and justified in
`docs/assumptions.md`. `docs/rules.md` specifies the user-rules language.

## Specification notes

`docs/assumptions.md` records every point where revision 1.0 underdetermines
behaviour or contradicts itself, with the resolution taken and why.

Two of its productions are written more narrowly than the language they
describe, and are read wider here: a terminal may carry a range (which is what
gives a replicated unit its arity), and a bracketed index may select a single
wire. Four of its worked examples contradict its own rules — op-amp pins named
`V+`/`V-`, an unbraced instantiation, terminals naming pins the part does not
declare, and an unquoted hyphenated field inside a substitution. In every one of
those the rule is followed and the example is corrected in `tests/spec`.

Two things are worth knowing before writing a design. A *capacitor* has no
definition in the language, yet two warnings need one, so `@type = capacitor` is
the convention adopted. And `$a-b$` is subtraction, never a reference to a field
named `a-b` — that needs quoting, as `$"a-b"$`.

## Licence

Copyright (C) 2026 Tom.

manta is free software: you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See the [GNU General Public License](LICENSE) for more details.

**Your designs are yours.** Output produced by running manta — netlists, BOMs,
exported files — is your design data, not a derived work of the compiler. This
licence places no conditions on it whatsoever. manta embeds none of its own code
in what it emits, so no runtime exception is needed to say so; it is said here
only to save anyone the trouble of working it out.
