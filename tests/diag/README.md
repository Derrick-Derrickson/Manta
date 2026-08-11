# Conformance fixtures

One file per diagnostic code. Spec 1.3 requires a conforming implementation to
"accept every construct in this document and reject every construct this
document declares an error", and to "implement all diagnostics in section 16,
reporting the code given there".

Each fixture is named for the code it must provoke. `test_diagnostics` runs the
appropriate stage over each one and asserts that the code fires. A fixture whose
name ends in `-clean` must produce *no* diagnostic of that code: those guard
against a rule that fires too eagerly.

`E-SYNTAX` covers every syntax error, so a bare `E-SYNTAX.manta` cannot say
which one. Its suffixed siblings hold the cases where *how many* diagnostics
come out is the thing under test, and `test_parse` names them directly rather
than through the `expectFires` loop.
