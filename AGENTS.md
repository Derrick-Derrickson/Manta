# Working in this repository

A compiler for the Manta Schematic Definition Language. C++23, no third-party
dependencies, cross-compiled for Windows with mingw-w64.

## Build and test

```sh
cmake --preset linux-release && cmake --build --preset linux-release -j
ctest --preset linux-release
```

Presets: `linux-release`, `linux-debug`, `linux-asan` (ASan+UBSan),
`windows-release` (mingw-w64 cross). **A change is not finished until all four
build clean and the first three test clean.** The sanitizer preset catches
things the others do not, and the Windows preset uses GCC 13 against the
others' GCC 15 — it has caught real uninitialised-variable bugs the newer
compiler missed.

There is no separate lint step. Warnings are errors in practice: the build is
kept at zero warnings under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wold-style-cast`.

## Where things are

| Path | What |
|---|---|
| `src/` | The compiler. See `src/README.md` for the architecture and `src/AGENTS.md` for the rules. |
| `tests/` | Unit tests, conformance fixtures, end-to-end pipeline. See `tests/AGENTS.md`. |
| `docs/spec.md` | The language specification, editorially corrected. **The authority.** |
| `docs/assumptions.md` | Every point where the spec underdetermines behaviour, and what was decided. |
| `examples/blinky/` | A complete board that must pass everything with no findings. |
| `schema/` | Published JSON Schemas for the two artifact formats. |
| `agents/` | Skills for writing manta designs and for extending this compiler. |

## Non-negotiables

These are properties the specification requires, and breaking one is a defect
even when every test still passes.

**Output is byte-identical for identical input.** No `std::unordered_map` where
the order can reach an artifact — use `FlatMap` from `src/base/flat_map.h`,
which iterates in insertion order. No floating point on any output path. No
timestamps, no environment reads, no `std::hash`. Sort with an explicit key
derived from source order or instance path.

**The lexer never resolves grammatical ambiguity.** `-5V` is a net name in one
position and a value in another. The lexer classifies a whole lexeme and hands
over a bitset; the parser picks by position. Do not add lookahead to the lexer
to "help".

**`src/diag/codes.def` is the only place a diagnostic is defined.** The enum,
the message table, the `-W`/`-Wno-` name map and the conformance test index all
generate from it.

**Errors mean no output.** Exit 1 writes nothing. Exit codes are fixed: 0
success, 1 errors, 2 usage, 3 internal.

## Before claiming a change works

Run the checks, and read what they say rather than only their exit status. In
particular:

- `ctest --preset linux-asan` — the same tests, but memory bugs surface.
- `cmake --build --preset windows-release` — a different compiler generation.
- `examples/blinky` must still pass `check -Werror` with **no** findings. If a
  change to ERC makes it report something, the checker is probably wrong, not
  the example.

## Conventions

Comments explain *why*, and cite the specification section when the reason is
"because the language says so" — `// Spec 6.3: ...`. Do not comment what the
code plainly does.

Match the surrounding style. Four-space indent, `lowerCamelCase` for functions
and variables, `UpperCamelCase` for types, trailing `_` on private members.

New AST nodes must be trivially destructible: they live in an arena that never
runs destructors, and `Arena::make` static-asserts it. Use `std::span` over
arena storage for child lists, not `std::vector`.
