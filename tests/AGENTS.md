# Adding tests

## A diagnostic fixture

Two files, always both:

1. `tests/diag/<code>.manta` — the smallest design that provokes the code.
2. A `TEST_CASE` in `tests/unit/test_diagnostics.cpp` calling
   `expectFires("<code>")`.

The runner pushes the fixture through the whole pipeline — lex, parse, local
check, resolve, elaborate, ERC — with **every** diagnostic enabled, including
those that default to off. It asserts the named code appears among the results.

The top-level block must be called `b`. Where a rule needs two objects, add a
second fixture and pass it: `expectFires("E-30", {"E-30b"})`.

### Making a fixture actually exercise its rule

The common mistake is a fixture that passes for the wrong reason, or one that
cannot fire at all. Three that bit during development:

- **W-03** (capacitor in series between two non-ground nets) first used `==` on
  both sides of the pull-down resistors, which shorted both cap nets to ground —
  so both *were* ground and the rule correctly stayed quiet. Fixed by using `=`.
- **E-05** (replication width not divisible by unit arity) first used a *counted*
  replication, whose width comes from the brackets and always divides. It needs
  an inferred `[[ ]]` with a two-in unit and a three-wide bus.
- **E-25** (a NC pin is connected) needs the NC pin on a net with **another**
  pin. A net with one pin is not "connected" to anything.

After writing a fixture, run it and read the output. If the expected code is
there but so are five others, tighten the fixture — a fixture that fires half
the table tests nothing in particular.

## A rule change

Run both halves:

```sh
ctest --preset linux-release -R "diagnostics|example"
```

If `example` starts reporting something, the checker is probably wrong. The
example is a correct board; treat a finding there as a false positive until
proven otherwise.

## An end-to-end behaviour

`pipeline.cmake` covers the specification's build sequence. Add to it when you
add a stage or an output. Anything that writes a file should be checked for
determinism there — run it twice, compare `file(SHA256 ...)`.
