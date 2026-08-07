# Tests

```sh
ctest --preset linux-release      # or linux-debug, linux-asan
```

Seven targets, each proving something different.

| Target | What it establishes |
|---|---|
| `unit.test_dimensioned` | Every SI form in the specification's table parses, both spellings of a fractional value agree, and canonical rendering is idempotent. |
| `unit.test_parse` | Every worked example parses, plus targeted checks on each grammar construct. |
| `unit.test_object` | `.mantaO` round-trips byte-exactly, and compiling twice gives identical bytes. |
| `unit.test_diagnostics` | **The conformance suite.** Each of the 39 errors and 8 warnings fires on its own fixture. |
| `pipeline` | The specification's own build sequence end to end through the real binary, plus determinism and the guarantee that formatting does not change a netlist. |
| `example` | `examples/blinky` under `-Werror` with no suppressions. |
| `unit.test_pinfields` | `#` fields on pins: line-wide application, call-site override, and the strength ladder. |
| `unit.test_rules` | The user-rules language: both motivating checks fire on a violating design and stay silent on a correct one. |
| `schema` | Emitted artifacts validate against the published JSON Schemas. Skips cleanly if `jsonschema` is not installed. |

## The two halves of conformance

`unit.test_diagnostics` proves each check **fires when it should**. `example`
proves **none fires when it should not**. Neither is worth much alone: a checker
that never fires is useless, and one that always fires is worse than useless
because people switch it off.

When you change a rule, both directions need re-checking. It is easy to fix a
false positive by weakening a rule until its fixture stops passing, and easy to
fix a false negative by strengthening one until `blinky` starts complaining.

## Layout

- `unit/` — one binary per file, using the ~80-line harness in `harness.h`.
  Deliberately not a third-party framework: it needs no network at configure
  time and cross-compiles wherever the compiler does.
- `diag/` — one fixture per diagnostic code, named for it.
- `spec/` — the specification's worked examples, with the four corrections
  listed in `docs/assumptions.md` §B and nothing else changed.
- `rules/` — one rules file, plus a design that violates it and one that does
  not. Both are needed: a rule that never fires is useless and one that always
  fires is worse.
- `pipeline.cmake`, `example.cmake` — driven through `cmake -P` so they run the
  same way on every platform.
