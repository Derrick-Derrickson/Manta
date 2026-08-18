# The four invariants

Properties the specification requires. Breaking one is a defect even when every
test still passes, because the tests cannot cover every path to violating them.

---

## 1. Byte-identical output for identical input

The specification is explicit: "Given identical input, manta produces
byte-identical output at every stage. There is no timestamp, random seed,
environment dependency or external process invocation."

This is enforced structurally rather than by discipline, because discipline does
not survive contact with a hurried change.

**Never iterate an unordered container where the order can reach an artifact or
a diagnostic.** `std::unordered_map` and `std::unordered_set` have
implementation-defined iteration order that differs between libstdc++ and MSVC
and can differ between runs. Use `FlatMap`/`FlatSet` from `src/base/flat_map.h`,
whose entries live in a dense vector and iterate in insertion order.

**Never put a pointer, an address or `std::hash` into a sort key or an output.**
Sort by source order, instance path, or an explicit index. `std::hash` is
permitted to be salted per process.

**Never emit a floating-point number.** `JsonWriter` has no `double` overload,
deliberately. Dimensioned values are scaled integers and render exactly.

**Sort directory scans.** `cli/driver_link.cpp` sorts each `-L` directory before
use, because filesystem order is not stable.

**Parallel work buffers per input and merges in input order.** The compile pool
gives each file its own arena, interner and diagnostic buffer, then merges in
command-line order, so `-j` cannot perturb stderr.

Verified by `tests/pipeline.cmake`, which runs compile and link twice and
compares hashes. Add any new output there.

---

## 2. The lexer never resolves grammatical ambiguity

The specification says a leading `-` "is resolved by grammatical position":

```
BIAS = -5V;             the net named -5V
#min-supply = -5V;      minus five volts
```

The same word, two meanings. The lexer scans one maximal lexeme and attaches a
classification bitset computed over **the whole thing**; the parser calls
`asNet()` or `asValue()` by position.

Because classification is whole-lexeme, `10kR-0603` simply fails the dimensioned
test and falls back to identifier, with no backtracking anywhere.

Two consequences that are easy to break:

- **Do not add context to the lexer.** Any lookahead that "helps" it decide will
  be wrong in the other position.
- **A value keeps its lexeme as well as its parsed number.** `&~NET=3V3` means
  the rail called `3V3`, not 3.3 volts, and only the directive's declared value
  type says which reading applies. `Value::text` carries the lexeme, and
  `.mantaO` serialises it alongside `num`.

The same principle governs the trailing hyphen. `1.2-` is a legal version
constraint *and* a malformed identifier; the lexer flags it and the parser
decides, so the error is reported in identifier positions only.

---

## 3. `codes.def` is the only definition of a diagnostic

`src/diag/codes.def` is an X-macro table. From it are generated the `DiagId`
enum, the message table, the `-W`/`-Wno-`/`--error=`/`--warn=` name map, and the
index the conformance suite iterates.

A code cannot be added, renamed or given the wrong default severity in one place
and not the others, because there is only one place.

Codes from the specification keep their numbers. Anything outside it uses a
lettered code — `E-SYNTAX`, `E-TYPE`, `E-IO`, `E-UNANNOTATED`, `E-INTERNAL` — so
it can never collide with the numbered space. E-03, E-16, E-19, E-35 and W-05 do
not exist in the specification and are never emitted.

Default a warning to `Severity::Ignored` only when it would fire on idiomatic
correct code. W-06 is the sole case; the reasoning is in `docs/assumptions.md`.

---

## 4. Spans stay accurate

A `Span` is file id, byte offset, byte length. Two things depend on that being
exactly right.

**The annotator edits source by byte range.** `Designator::assignmentSpan` is
the exact text after the prefix — `?`, or `7`, or `%[1:4]` — and nothing else.
Get it wrong and `manta annotate` corrupts files. It is not a reformat: every
other byte, including line endings and comments, is left untouched. This is why
CRLF survives loading and only the formatter normalises it.

**Diagnostics after substitution report the original position.** The
specification notes that a substitution "occupies one delimited span on one
line, source positions map through it by length alone". That only works if spans
are byte offsets into the original file, which is why the object format carries
them and the linker reloads the source to quote it.

When building a node, merge the spans of its parts. When synthesising one with
no source text, take the span of whatever caused it — a diagnostic with no
position is nearly useless.
