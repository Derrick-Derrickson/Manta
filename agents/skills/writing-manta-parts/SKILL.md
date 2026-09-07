---
name: writing-manta-parts
description: Author a manta part file — pin map, pin types, casual pins, swap groups, BOM fields, and the datasheet excerpt that lives after the end-of-content marker. Use when adding a component to a library, converting a datasheet into a part, or diagnosing a board error that traces back to a mis-declared part.
---

# Writing manta parts

A part maps a package's pads to named pins and declares its fields. Every
check on every net reads those declarations, so a wrong part breaks every
board that uses it. One part per file, the declaration then `---` then the
datasheet excerpt, for anything with a datasheet of its own; commodity
passives that share a family specification may share one file and one
excerpt, as blinky's do.

```
part LDO-3V3 {
    @~footprint    = SOT-23-5;
    #value         = AP2112K-3.3;
    @!type         = regulator;
    #~mpn          = "AP2112K-3.3TRG1";
    #!manufacturer = "Diodes Incorporated";

    1 : VIN<  &TYPE=POWER;
    2 : GND<  &TYPE=POWER;
    3 : EN<   #VIH=1V4 #VIL=0V4;
    4 : NC    &TYPE=NC;
    5 : VOUT> &TYPE=POWER #SUPPLY=600mA;
};

---

# AP2112K-3.3
...
```

## Ten rules

1. **Pin numbers are the footprint's pad names.** Check the footprint, not
   only the datasheet: KiCad's diode and LED footprints put the cathode on
   pad 1, a tactile switch with four pads may have only two pad numbers, and
   an exposed pad is a pad. A pad may be lettered: `A6 : DP-A<>;` on a USB-C
   receptacle, `MP : MOUNT;` for a mechanical pad, `EP : EP<;` for an exposed
   one; a numbered run is a range, `[3:11] : GPIO[1:9]<>;`. A pin the
   footprint does not have connects nothing, and a pad the part does not
   declare is not placed.
2. **Pin names are the datasheet's names**, respelled only where the
   language forces it: `PA9`, not `USART1-TX`; `SW`, not `OUT`. A name is
   letters, digits, `_` and `-`, not ending in `-`. So `D+`/`D-` become
   `DP`/`DM`, `CTS#` becomes `nCTS`, `SHD/SD2` becomes `SHD-SD2`, `V-`
   becomes `V-NEG`, and the excerpt says so. Two pads with one datasheet
   name get the pad appended: `GND-A1`, `GND-B12`, `VBUS-A4`. Alternate
   functions go in a comment on the line.
3. **Declare every pad**, including `NC`, thermal and mechanical pads.
   A part with a member-list pin, `[5:6] : USB.[+,-]<>;`, is bound as a
   whole on the board (`.USB = MCU-USB;`).
4. **Arrow and `&TYPE` are both required on a supply pin.** `VIN< &TYPE=POWER`
   consumes, `VOUT> &TYPE=POWER` provides, `VBAT<> &TYPE=POWER` consumes and
   may also feed out. A `POWER` pin with no arrow is invisible to the supply
   checks. Type every supply pin; a rail with no sourcing pin is the board's
   problem, solved there with `&TYPE=POWER` on the net.
5. **Arrows on signals, none on passives.** `>` drives, `<` listens, `<>` can
   do either (a GPIO, a bus pin). A resistor's pins carry no arrow. `<>` on a
   pin that only ever listens hides a missing driver.
6. **`&TYPE=OPENDRAIN`** on I²C and other wired-AND pins. **`&TYPE=NC`** where
   the datasheet forbids connection.
7. **`&CASUAL` only on interchangeable pins**: both ends of a resistor,
   capacitor, inductor, ferrite, crystal. Never a diode, LED, or transistor.
   It is scoped per line, so two elements in one package stay separate.
8. **No nets in a part** (`&NET` is **E-50**), no design values (`#DRAW` on a
   header belongs at the instance), no project names anywhere in the file. A
   part must be reusable by an unrelated board unchanged. Where a part
   declares a weak default the board must be able to override, the board
   writes a *stronger* one: `@~mate` in the part, `@mate` at the instance;
   two weak declarations that differ are **E-12**.
9. **Fields.** `@~footprint` is a package name (`R-0603`, `SOT-23-5`), never a
   library path; the project's `.fpmap` translates it. `#value`, `@!type`
   (`resistor`, `capacitor`, `diode`, `led`, `mcu`, `regulator`, `connector`
   …), `#~mpn`, `#!manufacturer`, and `#tolerance`, `#voltage`, `#power`
   where they apply. Only `capacitor` is read by a check (W-04). The
   renderer draws a symbol for `resistor`, `capacitor`, `electrolytic`,
   `inductor`, `ferrite`, `diode`, `zener`, `tvs`, `led`, `crystal`,
   `nmos`, `pmos`, `npn`, `pnp`, `opamp`, `switch`, `fuse` and `testpoint`;
   anything else, `mcu` or `sensor` or `esd`, is a box, and is fine.
   `#SUPPLY` goes on the one pin declared `POWER>`, never on every
   paralleled contact of a connector; a rail with no such pin states its
   rating on the board with `&CURRENT`.
10. **Pin `#` fields carry the worst-case datasheet figures** a project's
    rules read: `#VOH #VOL #VIH #VIL` on logic pins, `#SUPPLY` on a source
    pin, `#DRAW` on a consumer pin with a fixed draw, `&~PINDELAY` where the
    datasheet gives pad-to-die delay. Typical-column numbers pass boards that
    fail.

## Swap groups, harness members, connectors

`&SWAP=name` names a permutable index; every array tagged with it permutes
together, which is how a ganged swap is expressed:

```
[1:4] : IN[1:4]<  &SWAP=ch;
[5:8] : OUT[1:4]> &SWAP=ch;
```

A harness member list states the mapping where it is read, and its length
equals the pin range: `[12:13] : USB.[-,+]<>;`.

A connector says what it is with `@type`: `boardconnector` (something plugs
in; may carry a weak `@~mate`), `cableconnector` (it plugs into something;
carries `@mates` and optionally `@map`), `wire` (pins are its cores; carries
`#csa` as an area), `crimp`. Wires, crimps and cable connectors need no
footprint. A bare header's pins are `P[1:6]<>` with no roles: which position
is ground is the board's decision.

Mark a part `static` only when it is private to one file; a static library
part is invisible to the board (E-31).

## The datasheet excerpt

Everything after `---` is documentation, reproduced byte for byte by every
tool. It is what a designer reads instead of opening the PDF, so it holds
only facts from the datasheet, each traceable to it. Write these sections,
in this order, and omit a section only when the datasheet has nothing for it:

```
# <part number>
**Source:** <manufacturer>, <document title>, <revision, date>. <package>.
## Pins            | Pin | Name | Function |  — every pad, one line each
## Absolute maximum ratings
## Recommended operating conditions
## Electrical characteristics      the rows a designer sizes against, worst case
## Application     the external components the datasheet requires, its formulas
## Package         body size, pitch, pad numbering and polarity mark
```

Rules for the prose:

- **Only what the datasheet says.** Cite the table or section for each
  block. Write "not stated" rather than a figure from memory or from a
  similar part. Never present a family-typical number as this part's.
- **Worst-case columns, with the condition** they are specified at.
- **No history, no corrections narrative, no project references.** The
  excerpt describes the part as the datasheet does today. Anything about a
  board that uses it belongs in that board's own comments.
- **Application notes are the datasheet's**, not the author's design
  advice: what must be placed close, what value the reference circuit uses,
  what the sizing formula is.
- **Keep it to what a designer needs**: usually 40 to 150 lines. A pinout
  and a maximum-ratings table for a resistor; a full electrical table and
  the application section for a regulator or an MCU.

## Check it

A part library on its own does not link. Compile it alone to catch syntax
and local errors, then check it inside a design that instantiates every
part, under `-Werror`:

```sh
manta compile -o build/ lib/*.manta
manta check --top <block> -L build/ -Werror
```

Then open the footprint the `.fpmap` names and confirm every pin number is a
pad on it. The compiler cannot see a footprint; this step is yours.
