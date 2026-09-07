# Examples

## `blinky`

A complete, working board: USB-C in, a 3V3 LDO, an STM32F042 with USB and
I²C, two indicator LEDs through a replicated block, a bicolour status LED,
two option switches, a VBUS sense divider and an SWD header. Three looms plug
into it.

It exists to be *correct*. The specification's own worked example is an
excerpt — several of its nets are genuinely undriven, and it does not pass
ERC — so it demonstrates syntax but cannot demonstrate a clean build. This one
passes everything with nothing to report:

```sh
manta fmt --check examples/blinky/*.manta
manta compile -o build/ examples/blinky/*.manta
manta check --top blinky -L build/ --rules examples/blinky/blinky.mantaRules -Werror
manta link  --top blinky -L build/ --rules examples/blinky/blinky.mantaRules \
            -Werror --assembly --bom build/bom.csv -o build/blinky.mantaNets
manta export --format kicad --footprint-map examples/blinky/blinky.fpmap \
             -Werror -o build/blinky.net build/blinky.mantaNets
manta render -Werror -o build/blinky.html build/blinky.mantaNets
```

`--assembly` writes each mated loom's netlist and BOM beside the board's
without merging them; a loom also links on its own with `--top usb-c-1m`.
`blinky.fpmap` turns a package name into one KiCad can resolve, and under
`-Werror` an unmapped footprint fails the export rather than producing a
netlist Pcbnew will refuse to place.

No `--no-erc`, no `-Wno-`, and `-Werror` throughout. `tests/example.cmake`
runs exactly that sequence, which makes this the other half of the
conformance argument: the fixtures in `tests/diag` prove each diagnostic
fires on a design that earns it, and this proves none of them fires on a
design that does not.

It is written in the house style the bundled skills teach — passive networks
as chains, large ICs and connectors as binding blocks, comments about the
circuit — and it is written to exercise the language rather than to be
minimal, so it doubles as a tour:

| Construct | Where |
|---|---|
| Parts with datasheet excerpts after `---` | `ldo.manta`, `mcu.manta`, `esd.manta` |
| A `static` part private to one file | `TESTPOINT` in `board.manta` |
| `@VERSION`, `@FLATFORMAT`, `@bom`, `@fitted` by substitution | `indicator`, `TESTPOINT`, `J3` |
| Global field import and export | `>#author`, `#!>board-rev` |
| Two declared grounds, tied through a parallel group | `AGND = (.{R9~R-0R-0603}.)|2 = GND` |
| List-form global export | `[3V3, GND]>>` |
| `=`, `==`, `^`, `=*`, `*=` | the LDO chain, the DIP switch, the PWM summer, `CFG-PULL` |
| Named, casual and pin-range terminals | `A.{D1}.K`, `.{R1}.`, `A[0:1].{D3}.K` |
| Multi-unit references | `{U3}` on the second USB line, `{SW1}` on the second pole |
| Series, parallel and hanging multiplicity | `+2` divider, `|2` ground tie, `*2` bypasses |
| Per-copy values | `.=%[GND,AGND]` |
| Replication, inferred and with stated widths | `[[ ]]` on the drives, `[2[ ]1]` on the bicolour LED |
| Range designators | `R%[6:7]`, `C%[4:5]`, `BLK%[1:2]` |
| A block with a weak parameter and substitution | `indicator`, `R-$"series-r"$kR-0603` |
| Chain bindings at a pin | `.EN = VBUS`, and the idioms in the skills |
| Deliberately unbound and DNP | `.NC = ?`, `{!R5~R-0R-0603}` |
| A second source at the call site | `#!mpn` on `R8` |
| Pin fields for the project rules | `#VOH #VOL #VIH #VIL`, `#SUPPLY`, `#DRAW` |
| Harness types, `diff`, harness-carried directives | `usb2`, `i2c-bus`, `USB &HARNESS=usb2` |
| Named pads, on a part and in a map | `A6 : DP-A<>`, `MP : MOUNT`, `@map = [[1,A4], …]` |
| Multi-pin terminals on a reference | `VBUS = [VBUS-A9,VBUS-B4,VBUS-B9].{J1}` |
| Harness member list in a part, whole-harness binding, implied harness | `I2C.[SDA,SCL]` on the Qwiic connector, `.I2C = i2c`, `swd.IO` |
| Delay matching with a member override | `match usb-pair`, `@!tolerance` |
| Net directives at three strengths | `&!VOLTAGE`, `&~LAYER`, `&SHIELD`, `&PEAK`, `&CLASS`, `&RAIL`, `&RENDER`, `&STUB` |
| Pin directives | `&SWAP=cc`, `&~PINDELAY`, `&TYPE=NC`, `&CASUAL` |
| Instance directive | `&EDGE` on every connector |
| Render sections | the `--- TITLE` rooms |
| Connectors that say what plugs in, with mirrored and named maps | `@~mate`, `@mates`, `@map = [[1:5, 5:1]]`, `[[1:4, 1:4]]` |
| Cables with wires and crimps, and a wire's cross-section | `leads.manta`, `#csa` |
| Project rules over `@type` and `#` fields | `blinky.mantaRules` |

Two constructs are deliberately absent. `extern` names an instance declared
in another object, and a design with one top block has no such instance.
`&TYPE=POWER` on a net declares a rail with no sourcing pin, such as a buck
inductor's output or a diode-OR, and every rail here is fed by one.

Designators are already assigned, so the un-annotated check passes. The two
block instances each carry their own `R1` and `D1` — a designator is annotated
in the scope where it is written, and export flattens the path to `BLK1_R1`
and `BLK2_R1`.

The datasheet excerpts for the LDO, MCU, ESD array, LEDs and USB-C
receptacle were checked against the manufacturers' documents, which each
Source line names. The passives, headers, DIP switch and Qwiic connector
carry family figures and say so.
