# Examples

## `blinky`

A complete, working board: USB-C in, a 3V3 regulator, an eight-pin
microcontroller, an I²C bus and two LEDs driven through a replicated block.

It exists to be *correct*. The specification's own worked example is an excerpt
— several of its nets are genuinely undriven, and it does not pass ERC — so it
demonstrates syntax but cannot demonstrate a clean build. This one passes
everything with nothing to report:

```sh
manta fmt --check examples/blinky/*.manta
manta compile -o build/ examples/blinky/*.manta
manta check --top blinky -L build/ -Werror
manta link  --top blinky -L build/ --bom build/bom.csv -o build/blinky.mantaNets
manta export --format kicad --footprint-map examples/blinky/blinky.fpmap \
             -Werror -o build/blinky.net build/blinky.mantaNets
```

The lead is a separate thing to build, and links on its own:

```sh
manta link --top usb-c-1m -L build/ -Werror --bom build/lead.csv \
           -o build/lead.mantaNets
```

`--assembly` on the board does both at once, writing the lead's netlist and BOM
beside the board's without ever merging them:

```sh
manta link --top blinky -L build/ --rules examples/blinky/blinky.mantaRules \
           -Werror --assembly --bom build/bom.csv -o build/blinky.mantaNets
```

`blinky.fpmap` is what turns a package name into one KiCad can resolve:
`@~footprint = R-0603` says what the part is, and the map says that KiCad calls
it `Resistor_SMD:R_0603_1608Metric`. Keeping the two apart is what lets the same
part library export to Altium, OrCAD and Allegro as well. Under `-Werror` an
unmapped footprint fails the export rather than producing a netlist Pcbnew will
refuse to place.

No `--no-erc`, no `-Wno-`, and `-Werror` throughout. `tests/example.cmake` runs
exactly that sequence, which makes this the other half of the conformance
argument: the fixtures in `tests/diag` prove each diagnostic fires on a design
that earns it, and this proves none of them fires on a design that does not.

It is also written to exercise the language rather than to be minimal, so it
doubles as a tour:

| Construct | Where |
|---|---|
| A reusable block with a weak parameter | `block indicator`, `#~series-r` |
| Substitution inside a part name | `R-$"series-r"$kR-0603` |
| Replication driven by bus width | `LED-DRIVE[0:1] = [[ … ]]` |
| A shunt continued past with `==` | the decoupling capacitors |
| Default nets from the part | the MCU's `VCC` and `GND`, never bound |
| A declared ground, and globals | `GND &TYPE=GROUND;`, `3V3>>` |
| An open-drain bus as a harness | `i2c &HARNESS=i2c-bus;` |
| A swap group | the connector's `CC1`/`CC2` |
| A deliberate single reference | `TP1 = U2.MISO &STUB;` |
| An unconnected pin, deliberately | the regulator's `NC=?` |
| Net class and per-net directives | `3V3 &CLASS=power`, `&CURRENT=600mA` |
| A connector that says what plugs in | `CONN-USB-C`, `@type = boardconnector`, `@~mate` |
| A cable, with wires and crimps | `cable usb-c-1m` |
| A wire's cross-section as an area | `#csa = 205000um2` |
| Project rules over `@type` | `every-connector-is-mated`, `conductors-are-thick-enough` |

Designators are already assigned, so the un-annotated check passes. The two
block instances each carry their own `R1` and `D1` — a designator is annotated
in the scope where it is written, and export flattens the path to `BLK1_R1` and
`BLK2_R1`.
