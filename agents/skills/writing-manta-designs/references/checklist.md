# Before saying a design is done

Run this. Do not reason about whether the design is correct — the compiler
exists to answer that, and it knows 47 ways to be wrong that you will not think
of.

```sh
manta fmt --check src/*.manta
manta compile -o build/ src/*.manta
manta check --top <block> -L build/ -Werror
```

Silent means done. Anything else means not done.

## If it is not silent

Look the code up in `diagnostics.md`. Then, before changing anything, ask which
of these you are looking at:

**The design is wrong.** The usual case. Fix the design.

**The design is right and a part is wrong.** Very common, and easy to
misattribute. A net that will not stop reporting E-27 usually means the
regulator's output was never declared `&TYPE=POWER>`. A `.` terminal that
reports E-23 usually means a symmetric part forgot `&CASUAL`. Check the part
before contorting the board.

**The check is wrong.** Rare, but it happens — this is a young implementation.
The test for it: does `examples/blinky` still pass clean? If a correct board
passes and yours does not, the difference is in your design. If you genuinely
believe the rule is wrong, say so rather than working around it, and record it
in `docs/assumptions.md` if it is decided.

## What silence does not prove

`manta check` verifies what the language can see. It does not know that your
divider ratio is wrong, that the regulator cannot supply the current, or that
the part number is obsolete. It checks connectivity and declared intent.

Say what was verified and what was not. "Passes `check -Werror` with no
findings" is a true and useful statement. "The design is correct" is not one you
are in a position to make.

## Common self-inflicted wounds

- **Forgetting `GND &TYPE=GROUND;`.** Everything downstream looks broken.
- **`static` on a library part.** Internal linkage makes it invisible to the
  board that uses it. Only mark `static` what is genuinely private to one file.
- **Writing a net once.** E-26. If deliberate, `&STUB`.
- **`$a-b$` for a field named `a-b`.** That is subtraction. Quote it.
- **`=` between two bare names.** Use `==`, or put a device between them.
- **Leaving `?` designators.** E-UNANNOTATED. Run `manta annotate`, or write
  them in by hand for a small design.
