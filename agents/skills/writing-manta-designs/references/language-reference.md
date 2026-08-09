# Language reference, condensed

The full specification is `docs/spec.md` and is the authority. This is a working
summary for quick recall; go to the specification for anything subtle.

## Lexical

Identifiers are case sensitive: `SDA`, `sda` and `Sda` are three names. A `-` may
appear inside an identifier and as its first character, but not as the last.

A leading `-` is resolved by position: in a net position it is an identifier, in
a value position a negative number.

```
BIAS == -5V;            the net named -5V
#min-supply = -5V;      minus five volts
```

Reserved, lowercase only: `block part harness netclass match cable static extern`.
`Block` and `PART` are ordinary identifiers.

Every statement ends in `;`, including the closing brace of a definition.
Whitespace is insignificant and a statement may span any number of lines.

Comments are `//` to end of line and `/* */`, which do not nest.

A line of exactly `---`, outside any declaration, ends the manta content of the
file. Everything after it is documentation — a datasheet, usually — and is never
tokenised. Every tool reproduces it byte for byte.

## Values

| Quantity | Suffix | Examples |
|---|---|---|
| Resistance | `R` | `100R` `4k7R` `1M5R` |
| Capacitance | `F` | `100nF` `10uF` |
| Inductance | `H` | `2u2H` `100nH` |
| Voltage | `V` | `3V3` `-5V` `48V` |
| Current | `A` | `750mA` `3A` |
| Power | `W` | `250mW` |
| Frequency | `Hz` | `100MHz` `2G4Hz` |
| Length | `m` | `5mm` `100um` |
| Area | `m2` | `1mm2` `2m2` — a prefix squares with the unit |
| Time | `s` | `10ns` `1ms` |
| Temperature | `C` | `85C` `-40C` |

SI prefixes are `p n u m k M G T`. `u` is micro; `µ` is not accepted. An SI
prefix may replace the decimal point, and both spellings mean the same thing:
`4k7R == 4.7kR`, `3V3 == 3.3V`. The formatter emits the substituted form.

Metric only. An imperial literal is E-17. Package codes like `0603` are
identifiers naming a footprint family, not measurements.

Tolerance is `±1%` or `+-1%`. Strings use `"..."` with `\" \\ \n \t`. Booleans
are `true`/`false` in user fields, `TRUE`/`FALSE` in system fields. Lists are
`[a, b, c]`.

## Structure

```
file = { block | part | harness | netclass | match | cable }
```

No implicit file-level block, and a filename means nothing. Order of declaration
is irrelevant.

`static` gives internal linkage — visible only within its own object, and exempt
from the duplicate-name rule. Everything else is external.

## Fields

| Sigil | |
|---|---|
| `@` | System. The compiler interprets it; unknown names are E-10. |
| `#` | User. Carried to BOM and documentation untouched. |

Strength goes between sigil and name: `@~name` weak, `@name` normal, `@!name`
locked. Strongest wins; equal strength with different values is E-12;
overriding locked is E-11.

Fields flow **downward only**. A field on a block is visible to everything
instantiated within it; a field on a part stays there.

Import from global with the arrow toward the name, export with it away:
`>#author = TJM;` and `#!>board-rev = C;`. Export requires locked strength, and
a part may not export at all.

System fields: `@footprint`, `@fitted`, `@bom`, `@type`, `@VERSION`,
`@FLATFORMAT`, the mating fields `@mate`, `@mates` and `@map`, and inside a match
group `@src`, `@dest`, `@tolerance`, `@offset`.

`@type` says what a part is. Unstated it is `board_part`. Five values are
structural and the compiler interprets them — `board_part`, `boardconnector`,
`cableconnector`, `wire`, `crimp` — and anything else is an ordinary
classification carried to the BOM untouched. A near miss on a structural role is
W-TYPE, because the set being open means a typo cannot be an error and its
consequence is silent.

## Cables

A `cable` is a loom: its own deliverable, with its own netlist and BOM. Its body
is a chain, exactly as a block's is, and it may hold only a cable connector, a
wire or a crimp.

```
part JST-8-PLUG  { @type = cableconnector; @mates = BACKPLANE-OUT; [1:8] = P[1:8]<>; };
part JST-8-CRIMP { @type = crimp;  1 = A &CASUAL; 2 = B &CASUAL; };
part WIRE-22AWG  { @type = wire; #csa = 1mm2; 1 = A &CASUAL; 2 = B &CASUAL; };

cable jumper-8way {
    {J1~JST-8-PLUG}P[1:8]
        = [[ .{C%[1:8]~JST-8-CRIMP}. = .{W%[1:8]~WIRE-22AWG}.
           = .{C%[9:16]~JST-8-CRIMP}. ]]
        = P[1:8]{J2~JST-8-PLUG};
};
```

A board connector says which loom plugs in:

```
part BACKPLANE-OUT { @type = boardconnector; @~mate = jumper-8way; };
```

The pins go one to one unless `@map` says otherwise. A map is a list of pairs
whose elements may be ranges, and a range may descend:

```
@map = [[2,3],[3,2]];      // a null modem
@map = [[1:20],[20:1]];    // a reversed ribbon
```

## Pins

```
part cool-mcu {
    1       = VCC<        &TYPE=POWER &~NET=3V3;
    2       = GND<        &TYPE=POWER &~NET=GND;
    [3:11]  = GPIO[1:9]<> &SWAP=gpio-bank;
    [12:13] = USB.[+,-]<>;
    16      = NC          &TYPE=NC;
};
```

`&TYPE` declares electrical character, the arrow declares direction, and the two
are orthogonal:

| | |
|---|---|
| omitted, no arrow | `PASSIVE` — claims nothing, skipped by drive checks |
| omitted, arrow present | `SIGNAL` |
| `POWER` | on a power net; `>` provides, `<` consumes |
| `OPENDRAIN` | many drivers permitted |
| `NC` | shall not be connected |

`&NET` names the net a pin joins when nothing binds it — declare it weak.
`&NET=?` unbinds: no net is created, so no `&STUB` is needed.

`&CASUAL` makes a pin eligible for the `.` terminal and puts it in a weak
implicit swap group, scoped per declaration line.

## Ports

| Written | |
|---|---|
| `>SIG` / `SIG<` | input |
| `<SIG` / `SIG>` | output |
| `<>SIG` / `SIG<>` | bidirectional |
| `>>VIN` / `3V3>>` | global, design-wide |

The formatter emits leading `>` at the start of a statement, trailing `>` at the
end, and `pin=NET>` in a binding.

## Directives

`&NAME=value` at the end of a statement, before the terminator. Same strength
ladder as fields.

Net: `&IMP` `&CURRENT` `&PEAK` `&VOLTAGE` `&MAXDELAY` `&CLASS` `&MATCH`
`&LAYER` `&SHIELD` `&TYPE` `&STUB`.
Pin: `&TYPE` `&NET` `&PINDELAY` `&CASUAL` `&SWAP`.
Harness: `&HARNESS`.

`&CASUAL` and `&STUB` take no value; everything else requires one.

## Designators

`U?` unassigned, `U7` numbered, `BLK%[1:4,9:10]` a range carrying several. A
reference `{U3}` requires `U3` to exist. Reverting to `?` releases the numbers.

A designator is annotated in the scope where it is written, so a block's `R1` is
one component per block instance. Export flattens the path with `@FLATFORMAT`,
defaulting to `_`-joined.

## Toolchain

```
manta compile -o build/ src/*.manta
manta link --top <block> -L build/ --bom bom.csv -o board.mantaNets
manta annotate -n board.mantaNets src/*.manta
manta export --format kicad -o board.net board.mantaNets
manta fmt src/*.manta
manta check --top <block> -L build/ -Werror
```

Exit codes: 0 success, 1 errors (nothing written), 2 usage, 3 internal.
