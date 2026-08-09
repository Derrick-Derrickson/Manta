# Changing the compiler

Read `src/README.md` first for the architecture. This file is the rules.

## Which stage does a check belong to?

The specification is explicit that ERC runs at link and not at compile, because
no-driver, no-source, multiple-driver and unpowered-net are whole-design
properties that cannot be evaluated one object at a time.

Put a check at the **earliest stage that can decide it**:

| Stage | File | Checks that belong here |
|---|---|---|
| Lex | `lex/lexer.cpp` | Malformed tokens, unterminated constructs |
| Parse | `parse/parser.cpp` | Anything visible in the token stream: E-09, E-37 |
| Compile | `sema/local_check.cpp` | Decidable from one file, no external names: E-08, E-10, E-13, E-18, E-32, E-34, E-38, E-43 |
| Link — resolve | `link/symbols.cpp` | E-30, E-31, E-36 |
| Link — elaborate | `link/elaborate.cpp` | Needs the instantiated design: E-04..E-07, E-11, E-12, E-21..E-23, E-29, E-39..E-42 |
| Link — ERC | `erc/erc.cpp` | Whole-design electrical properties: E-01, E-02, E-20, E-24..E-28, E-33, W-01..W-09 |
| Link — mating | `link/mating.cpp` | Needs a second design, the cable: E-44..E-48 |

A check that needs a part's pins needs the symbol table, so it cannot be at
compile. A check that only compares text in one statement should not be at link.

`E-UNANNOTATED` is deliberately in `cli/driver_link.cpp` rather than in the ERC
pass: it is a property of the build, not of the design, and must run even under
`--no-erc`.

Mating is its own stage rather than part of ERC because it needs something ERC
does not have: a *second* elaborated design. A cable named by `@mate` is compiled
on its own, and only then can the two be compared. `MateChecker` takes the
elaboration function as a parameter rather than reaching for the linker's object
list, which is what keeps it testable.

## Adding a diagnostic

1. Add one line to `src/diag/codes.def`. That generates the enum, the message
   table and the `-W`/`-Wno-` name map.
2. Report it with `diags_.report(DiagId::X, span, args...)`, chaining `.note()`
   for a secondary location.
3. Add a fixture at `tests/diag/<code>.manta` and a `TEST_CASE` in
   `tests/unit/test_diagnostics.cpp`.
4. Check `examples/blinky` still passes `check -Werror` clean.

Codes from the specification keep their numbers. Anything not in it uses a
lettered code — `E-SYNTAX`, `E-TYPE`, `E-IO`, `E-UNANNOTATED` — so it can never
collide with the numbered space.

Default a warning to `Severity::Ignored` only when it would fire on idiomatic
correct code; W-06 is the sole example, and the reasoning is in
`docs/assumptions.md`.

## Determinism

Every change to an output path has to preserve byte-identical output.

- Never iterate a `std::unordered_map`/`set` where the order can reach an
  artifact or a diagnostic. Use `FlatMap`/`FlatSet`.
- Never put a pointer value, an address, or `std::hash` into a sort key or an
  output. Sort by source order, instance path or an explicit index.
- Never emit a `double`. `JsonWriter` has no such overload on purpose.
- Directory scans must be sorted; `cli/driver_link.cpp` sorts each `-L`
  directory before use.
- Anything parallel buffers per-input and merges in input order.

`tests/pipeline.cmake` runs compile and link twice and compares hashes. If you
add a stage that writes something, add it there too.

## Spans

Every node carries the byte range it came from. This is load-bearing in two
places:

- Diagnostics after substitution must report the position of the substitution
  in the *original* source. Positions map through a substitution by length
  alone, which only works if spans are byte offsets.
- The annotator rewrites source by byte range. `Designator::assignmentSpan` is
  the exact text after the prefix and nothing else — get it wrong and annotate
  corrupts files.

When you build a node, merge the spans of its parts. When you synthesise a node
that has no source text, take the span of whatever caused it.

## The arena

AST and IR nodes live in an arena that never runs destructors. `Arena::make`
static-asserts trivial destructibility, so a `std::string` or `std::vector`
member will not compile. Use `SymbolId` for names and `std::span` over arena
storage for child lists; build into a local `std::vector` and `commit()` it.

## Things that look like bugs but are not

- **The lexer does not know what a token means.** `10kR-0603` is a part name and
  `4k7R` is a resistance and the lexer classifies both without choosing. Adding
  context to the lexer will break the other reading somewhere.
- **`==` joining an element's own terminals is intentional.** It is how a device
  gets shorted, which is legal and warned about, not an error.
- **`.mantaO` keeps a numeric value's lexeme as well as its number.** `&~NET=3V3`
  means the rail called `3V3`, not 3.3 volts. Same word, two readings, and the
  directive's declared type says which.
- **CRLF survives loading.** Only the formatter normalises, and it does so by
  rewriting the whole file.
