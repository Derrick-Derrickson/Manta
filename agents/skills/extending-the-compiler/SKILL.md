---
name: extending-the-compiler
description: Change the manta compiler itself — add a diagnostic, a language construct, an export backend, or fix a bug in the lexer, parser, elaborator, ERC or formatter. Use when working on the C++ in src/, not when writing manta designs.
---

# Extending the compiler

C++23, no third-party dependencies, ~14k lines. Read `src/README.md` for the
architecture before changing anything; `references/architecture.md` here goes
deeper on the parts that are subtle.

## Always

```sh
cmake --preset linux-release && cmake --build --preset linux-release -j
ctest --preset linux-release
```

A change is finished when **all four presets build clean** — `linux-release`,
`linux-debug`, `linux-asan`, `windows-release` — and the first three test clean.
Not one of them. The sanitizer catches what the others do not, and the Windows
cross-build uses GCC 13 against the others' GCC 15, which has caught real
uninitialised-variable bugs.

Zero warnings is the standard, not an aspiration.

## Where a change goes

**A new diagnostic** — one line in `src/diag/codes.def`, which generates the
enum, message table and `-W` name map. Then a fixture in `tests/diag/` and a
`TEST_CASE`. Put the check at the earliest stage that can decide it; the table
in `src/AGENTS.md` says which stage owns what.

**A new language construct** — lexer if it needs a token, `src/ast/ast.h` for
the node, parser to build it, `src/obj/mantao.cpp` for **both** directions of
serialisation, and the elaborator to give it meaning. The formatter is lexical
and needs a change only for a token with special line placement. The object
round-trip test will fail immediately if you update the writer and forget the
reader, which is the point of it.

**A new ERC rule** — `src/erc/erc.cpp`, one method, called from `run()`.

**A new declaration kind** — six places the compiler will *not* point at:
`isReservedWord` (`src/lex/dimensioned.cpp`), `atItemStart` and `parseItem`
(`src/parse/parser.cpp`), the `readItem` kind ternary (`src/obj/mantao.cpp`,
which falls back to `Block`, so a forgotten kind degrades in silence), the
`kind` enum in `schema/mantaO.schema.json`, and `kLanguageVersion`. Three it
will: the exhaustive switches in `local_check.cpp`, `formatter.cpp` and
`mantao.cpp`. `cable` is the worked example.

**A new export backend** — `src/export/exporters.cpp`, one function taking
`(const Design&, const ExportOptions&)`, plus the format name in
`parseExportFormat` and the extension in `exportExtension`. Anything a backend
needs to be told goes on `ExportOptions`, so adding one does not change the
others. A target that resolves footprints through a library table wants
`resolveFootprint` (`src/export/footprint_map.h`); one that has to recognise a
component across a re-import wants `pathUuid` (`src/export/uuid.h`), which is
derived from the instance path so a re-annotation does not orphan a placement.

## The four invariants

Breaking one is a defect even when the tests pass. `references/invariants.md`
has the full statement of each; in short:

1. **Byte-identical output.** No `unordered_map` on an output path, no floats,
   no pointers in sort keys, no timestamps. Use `FlatMap`.
2. **The lexer never resolves grammatical ambiguity.** It classifies a whole
   lexeme; the parser picks by position.
3. **`codes.def` is the only definition of a diagnostic.**
4. **Spans are byte ranges and must stay accurate.** The annotator edits source
   by byte range, and post-substitution diagnostics map back through position
   arithmetic.

## Both halves of a rule change

`tests/diag/` proves each check fires when it should. `examples/blinky` proves
none fires when it should not. Changing a rule needs both re-run:

```sh
ctest --preset linux-release -R "diagnostics|example"
```

If `blinky` starts reporting something, treat it as a false positive until
proven otherwise — it is a correct board.

This is not theoretical. Two rules were over-firing when first written: E-02
flagged every pull-up on the board, and E-26 flagged every block port. Both were
found by writing a design that was supposed to pass and did not.

## When the specification and the implementation disagree

`docs/spec.md` is the authority. But it contradicts itself in places, and
`docs/assumptions.md` records every such point with the resolution.

Before "fixing" behaviour that looks wrong, check whether it is already a
decided question. If it is a new one, resolve it the same way: follow the stated
rule where the rule is clear, widen the grammar only where the language plainly
requires it, and write the decision down with its reasoning. Do not silently
special-case.

## Reading the code

Comments explain *why*, and cite the specification when the reason is "because
the language says so". If you find yourself asking why something is done a
strange way, the comment above it is usually the answer — the strange ways are
mostly load-bearing.

Three that look like bugs and are not:

- The lexer classifying `10kR-0603` as an identifier and `4k7R` as a resistance
  without choosing between the readings.
- a multi-pin terminal (`..{R1}`, `[A,K]{D1}`) uniting several pins of one part onto one node -- how a device gets shorted, deliberately and without W-02.
- `.mantaO` keeping a numeric value's lexeme alongside its number, because
  `&NET=3V3` means a rail and not a voltage.
