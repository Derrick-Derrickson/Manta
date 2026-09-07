# Before saying a design is done

Run this. Do not reason about whether the design is correct; the compiler
knows more ways to be wrong than you will think of.

```sh
manta fmt --check src/*.manta
manta compile -o build/ src/*.manta
manta check --top <block> -L build/ --rules <project>.mantaRules -Werror
manta link  --top <block> -L build/ --rules <project>.mantaRules -Werror \
            --assembly --bom build/bom.csv -o build/<block>.mantaNets
manta export --format kicad --footprint-map <project>.fpmap -Werror \
             -o build/<block>.net build/<block>.mantaNets
```

Silent means done. Anything else means not done.

## If it is not silent

Look the code up in `diagnostics.md`. Then decide which of these it is:

**The design is wrong.** The usual case. Fix the design.

**A part is wrong.** Very common. E-27 that will not go away is a regulator
whose output is not `POWER>`. E-23 is a symmetric part without `&CASUAL`. A
supply pin invisible to every check is a `POWER` pin with no arrow. A rail
with no sourcing pin is declared `&TYPE=POWER` on the board. Fix the part,
not the board.

**The check is wrong.** Rare. Does `examples/blinky` still pass? If a correct
board passes and yours does not, the difference is in your design. If you
still believe the rule is wrong, say so; never work around it silently.

## Style, once it is silent

- Every passive network is one chain, source to load. No lone
  `X = .{R}. = Y;` statements that could join a neighbouring chain.
- A passive that serves an IC pin is written in that pin's binding.
- Every IC with more than about six pins is a standalone binding block with
  every pin bound, in the part's pin order, and `= ?` on the unused ones.
- No `RAIL = PIN.{U~big-ic: ...}` chain forms on large ICs.
- Every connector is a binding block with `&EDGE` first.
- Every comment says something about the circuit. None cites the spec,
  explains an operator, or records what a value used to be.
- Every `--- TITLE` section is one functional group, in signal-flow order.
- Designators are assigned (no `?` on a finished board) and net names say
  what the net carries.

## What silence does not prove

`manta check` verifies what the language can see: connectivity, declared
intent, and whatever the project's rules read. It does not know that a
divider ratio is wrong, that an inductor saturates at the budgeted current,
that an LED's forward voltage exceeds its rail, or that a part number is
obsolete. Say what was verified and what was not: "passes `check -Werror`
with no findings" is a true statement; "the design is correct" is not one you
can make.

## Common self-inflicted wounds

- Forgetting `GND &TYPE=GROUND;`. Everything downstream looks broken.
- `==` after a net or a pass-through device, or `=` after a shunt. E-49; the
  fix is always the other spelling.
- Writing a net once. E-26. If deliberate, `&STUB`.
- A chain that starts at `U1.PIN` and runs into a series resistor:
  `U1.PA8 = .{R8~R}. = DATA;` names the pin's net exactly once. Bind the pin
  to a named net in the instance (`.PA8 = DATA-GPIO;`) and chain from that.
- `$a-b$` for a field named `a-b`. That is subtraction. Quote it.
- `static` on a library part. It is invisible to the board (E-31).
- Leaving `?` designators on a finished board. E-UNANNOTATED.
- A stale `build/` directory. `-L build/` links every object in it, deleted
  sources included, so a part-domain rule reports parts you removed. Start
  from an empty `build/` when a diagnostic names something that is gone.
