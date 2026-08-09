# The compiler

```
.manta ──lex──▶ tokens ──parse──▶ AST ──sema──▶ emit──▶ .mantaO
                                                           │
 .mantaO × N ──resolve ▸ elaborate ▸ substitute ▸ nets ▸ ERC──▶ .mantaNets
                                                           │
                        ┌──────────────────────────────────┼─────────────┐
                    annotate                             export          fmt
```

## Layers, bottom up

**`base/`** — the substrate. An arena allocator for AST nodes, a string
interner turning identifiers into 32-bit ids, `FlatMap`/`FlatSet` with
insertion-ordered iteration, a union-find, and small UTF-8 and hashing helpers.
Nothing here knows about manta.

`FlatMap` exists because `std::unordered_map`'s iteration order is not portable
and the language requires byte-identical output. Entries live in a dense vector
and the open-addressed index table points into it.

**`source/`** — loading files and addressing them. A `Span` is 12 bytes: file
id, byte offset, length. Line and column are derived on demand, because
diagnostics are rare and spans are everywhere. A BOM is stripped at load; CRLF
is *kept*, because the annotator edits source by byte range and must not
disturb the line endings of lines it does not touch.

**`diag/`** — `codes.def` is an X-macro table listing every diagnostic once:
code, mnemonic, default severity, message format. Everything else generates
from it. `engine.h` buffers diagnostics rather than streaming them, so a
parallel compile can merge per-file results in command-line order and produce
identical stderr regardless of thread count.

**`lex/`** — the lexer, and `dimensioned.cpp`, which parses and renders SI
values exactly. A value is a scaled integer, never a double: binary floating
point cannot represent `4.7` or `0.002`, so a round trip through one would be
neither exact nor portable.

The lexer has two modes. Inside `$…$` every operator carries its arithmetic
meaning, `-` is always subtraction, `/` is never a comment.

**`parse/`** — recursive descent over the grammar. LL(2) once three ambiguities
are handled by lookahead: which of five things a `[` opens, whether a word
starting an element is a terminal or a net, and which reading a leading `-`
takes.

**`sema/`** — the checks decidable from one file, and the closed registries of
system field names and directive names. The open namespace is `#`, which is
carried through untouched.

**`obj/`** — `.mantaO` reading and writing. The object *is* the parse tree,
with names unresolved and substitutions unevaluated, because a block cannot be
elaborated until instantiated. A round-trip test writes, reads and writes again
and compares bytes, which is what keeps the two halves from drifting.

**`link/`** — the substance.

- `symbols` — resolution across objects, with `static` giving internal linkage.
- `fields` — the weak/normal/locked ladder and the parent-linked scope chain.
  Fields flow downward only, so lookup walks outward and assignment never does.
- `subst` — evaluating `$…$` against the fields in force.
- `part_info` — expanding a `part` declaration into concrete pins.
- `elaborate` — the core. See below.
- `netlist` — the elaborated design and the writers for netlist, BOM and map.
- `mating` — connectors and cables (§12A). Resolves `@mate` to a cable, compiles
  it on its own, and lays the two against each other. A cable is a separate
  deliverable, so this never touches the board's netlist.

**`erc/`** — the rules of specification §16, each a pass over the elaborated
design.

**`rules/`** — the user-rules language of `docs/rules.md`: a parser for
`.mantaRules` and a small interpreter over the elaborated design. It reuses the
manta lexer unchanged, since a rules file is written in the same tokens. Its
attributes are `#` fields, which are already legal manta, so a decorated design
needs no rules file to compile.

**`fmt/`, `annotate/`, `export/`** — the three tools that consume the pipeline's
output. The formatter rewrites whole files from the AST; the annotator makes
surgical byte-range edits and never reformats. `export/footprint_map` translates
a package name into whatever a layout tool calls it, and `export/uuid` gives a
component an identity that survives re-annotation, over the SHA-1 in `base/`.

**`cli/`** — option parsing, one driver per subcommand, and the platform layer
(console colour, wide argv on Windows, binary stdout).

## The central idea: bundles

Every chain element evaluates to a **bundle** — an ordered vector of node
handles for its entry terminal and another for its exit. The connectors are
then operations on bundles, merged through a union-find:

| Operator | Effect |
|---|---|
| `=` | union(left.exit, right.entry), then advance |
| `==` | the same, plus join an element's own two terminals when `==` is on both sides |
| `^` | nothing at all |
| `=*` | gather an N-wide bundle onto one node |
| `*=` | broadcast one node across an N-wide bundle |

That last clause on `==` is the whole of the shorting rule. A two-terminal
device with `==` on both sides has its own pads bridged, which is legal and
raises W-02 — and it falls out of the uniform rule rather than needing a case.

Widths propagate along a segment to a fixed point before any node is
materialised, which is how a net that omits its range takes it from the other
side of the connection.
