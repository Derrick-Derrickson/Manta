# Architecture, the parts that are subtle

`src/README.md` is the tour. This covers the five places where the design is
non-obvious and a naive change will break something.

---

## Word classification

The lexer scans one maximal *word* and computes a bitset over the whole lexeme:
`Identifier`, `Integer`, `Decimal`, `Dimensioned`, `Imperial`, `TrailingDash`,
`Reserved`, `BoolTrue`/`BoolFalse`, `UpperCase`, `LeadingDash`, `VersionSpec`.

Scanning admits a `.` only while a *number* is being built — the run since the
start, or since the last `-`, is all digits. That one rule separates:

```
4.7kR      number, dot included
0.2-1.2    version range, both dots included
U1.GPIO1   stops at the dot, so member access parses
U9.1       stops at the dot, so the pin reference parses
```

Getting this wrong is subtle: an earlier version swallowed `U9.1` whole and the
symptom was a spurious E-22 several stages later.

Unit suffixes are longest-match, so `Hz` beats `H`. Imperial suffixes are
recognised *on purpose*, so `100mil` reports E-17 rather than degrading into a
mystifying identifier.

---

## The three parser ambiguities

**What a `[` opens.** Replication `[[`, counted replication `[N[`, a range
`[lo:hi]`, a list `[a,b]`, or a port list `[3V3, GND]>>`. Two tokens of
lookahead separate them; brackets lex as single characters so the lexer does not
have to guess.

**Whether a word starts a terminal or a net.** A chain element is a device when
a `{` follows an optional terminal. `looksLikeDevice()` scans past a bracketed
index to find out.

**Which reading a leading `-` takes.** Handed to the parser by position, as
above.

---

## Bundles, and the connectors

Every chain element evaluates to an `ElemValue` with an `entry` bundle and an
`exit` bundle — ordered vectors of union-find node handles, one per wire.

```
=    unite(left.exit, right.entry)
==   the same, plus unite(element.entry, element.exit) when '==' is on both sides
^    nothing
=*   for each n in left.exit: unite(n, right.entry[0])
*=   for each n in right.entry: unite(left.exit[0], n)
```

That extra clause on `==` is the whole shorting rule. The specification says
"every element in a run of consecutive `==` lies on one net", and joining an
element's own two terminals when it sits between two `==` is exactly that — and
is what makes a two-terminal device shorted, which is legal and raises W-02.

A shunt has no exit terminal, so `elaborateSegment` falls back to its entry.
That is what lets a chain continue past one.

---

## Width inference

Widths propagate along a segment to a fixed point **before** any node is
materialised, because a net that omits its range takes it from the other side of
the connection.

`staticWidth()` gives what is knowable independently: an explicit range, a
counted replication's brackets, a device terminal's pin count. Then four passes
propagate forward and backward across the connectors, with `=*` forcing the
right side to one and `*=` the left. Anything still unknown settles at 1.

A replication's copy count is `incoming / in_arity`, and the body's arity is
measured by running `inferWidths` over the body first.

---

## Scopes and monomorphisation

Each block instantiation creates a `Scope` with its own id, its own instance and
harness tables, and a `FieldEnv` whose parent is the caller's. The body is
elaborated afresh per instantiation — that *is* the monomorphisation, and it is
why a block instantiated twice with different parameters produces two
elaborations from one source.

Named nets are keyed `(scope id, name, index)`, so two blocks' `IN` nets are
distinct without any renaming. A block's ports are simply its named nets, which
the caller unions against after the body is walked.

`FieldEnv` is parent-linked because fields flow downward only: lookup walks
outward, declaration never does.

---

## Naming a net

After the union-find settles, every node maps to its root and roots become nets.
Naming runs in two passes: the first takes any name, the second lets an
explicitly written name overwrite a pin-derived one. That implements "where a
net is also named explicitly the two are aliases, and the explicit name is used
for display, netlist output and BOM".

A pin deliberately unbound with `&NET=?` joins no net at all — its node is
excluded before nets are built, or it would surface as an empty net and trip
E-26.

---

## The formatter

Rewrites whole files from the AST, so it cannot preserve anything the AST does
not record. That is why `Range::single` exists: `[i]` and `[i:i]` mean the same
thing and must round-trip differently.

Comments are re-attached by **source offset**: before emitting any construct,
every comment lying before its span is flushed. One that shared a line with
preceding code goes back beside it; one that stood alone keeps its own line.
That reproduces comments exactly while leaving the formatter free to reflow
everything around them.

Idempotency is a test, not a hope — `fmt(fmt(x)) == fmt(x)` over the whole
corpus, plus the stronger guarantee that formatting does not change the netlist
a source produces.
