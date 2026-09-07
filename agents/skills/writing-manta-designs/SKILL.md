---
name: writing-manta-designs
description: Write or modify a manta schematic — a board, a block, a cable, the nets and constraints between parts. Use when asked to describe a circuit in manta, add a subcircuit, or fix diagnostics from a manta build. Covers the house style, the connection operators, and the checks that judge the result.
---

# Writing manta designs

A design is a set of named declarations describing one board. Order never
matters; a name may be used before it is declared. The compiler is the oracle:
a design is finished when this is silent, and not before.

```sh
manta fmt --check src/*.manta
manta compile -o build/ src/*.manta
manta check --top <block> -L build/ --rules <project>.mantaRules -Werror
```

## Work in this order

1. **Parts first.** Find or write every part (`writing-manta-parts` skill).
   Most board errors are a part with a wrong pin number, arrow or `&TYPE`.
2. **Board second**, following `examples/blinky/board.manta` and the style
   below. One `--- TITLE` section per functional group.
3. **Build, read every diagnostic, fix the cause.** `references/diagnostics.md`
   says what each code means and what fixes it. Never silence a check with
   `-Wno-` or `--no-erc` to make a build pass.

## House style

**Passive networks are chains.** Write a passive network as one statement
that reads from source to load, with shunts continued past by `==`:

```
VIN = .{R1~R-10kR-0603}. = .{C1~C-100nF-0603: .=GND} == EN;
3V3 = .{C2~C-10uF-0805: .=GND} == .{C3~C-100nF-0603: .=GND};
```

Not one statement per component, and not a binding per resistor pin.

**A passive that belongs to an IC pin is written at the pin**, as a chain in
the binding: `.EN = .{R5~R-100kR-0603}. = VIN;` or `.FB = .{R3~R-51kR-0603}. = 5V;`.

**Small pass-through devices sit in chains** with named terminals: a diode
`A.{D1~D-SS34}.K`, a FET `S.{Q1~FET}.D`, a ferrite `.{FB1~FERRITE}.`, an LED,
a regulator with one input and one output `VIN.{U1~LDO: .GND=GND;}.VOUT`.

**A large IC is a binding block, never a chain element.** Anything with more
than about six pins — an MCU, an ADC, a driver, a transceiver — is written
standing alone, every pin bound in the part's declaration order, unused pins
bound to `?`:

```
{U1~CH32V203C8T6:
    .VDD  = 3V3;
    .VSS  = GND;
    .PA9  = LEFT-TX;
    .PA10 = LEFT-RX;
    .PC13 = ?;
};
```

Do not write `3V3 = VDD.{U1~MCU: ...}` for such a part: the chain form hides
the supply among forty bindings and makes the IC look like a series element.

**A connector is a binding block too**, one line per position, so what each
position carries is read in one place. Put `&EDGE` first in the list. An
array pin is bound by index, `.P[3] = SDA;`, never `.P3`; a member-list pin
is bound as a whole, `.USB = MCU-USB;`.

**A device with every pin bound stands bare**: `{U1~part: ...};`. The
leading-dot form `.{U1~part: ...}.` is a chain terminal and needs a casual
pin left unbound for the dot to take.

**Comments explain the design, never the language.** Say why this value,
why this topology, what the firmware assumes, what the datasheet demands.
Never cite a spec section, explain what `==` means, or narrate edit history
("was 10k, corrected to 200k"). If a construct needs a comment to be
understood, rewrite the construct.

**Name nets for what they carry** (`BUCK-FB`, `USB-VBUS`, `LED-DRIVE[0:1]`),
not for where they go. Hand-assign designators on a small board; leave `?`
and run `manta annotate` on a large one.

## The connectors

The connector states whether the chain moved, and it has to tell the truth:

```
A = B                          both names on one net
A = .{R1~R}. = B               A and B are different nets, joined through R1
X = .{C1~C: .=GND} == Y        C1 dead-ends, so '==' stays on the node; X and Y are one net
A[0:3] = [[.{R?~R}.]] =* SUM   gather: four wires onto one net
VREF *= BIAS[0:7]              broadcast: one net to every element
A ^ B                          in one statement, not connected at all
```

`=` advances through the far side of the element before it, so that element
must have one: a net, `.{R}.`, `A.{D}.K`, a `+N`/`|N` group. `==` continues on
the near side and is legal only after an element with no far side: a shunt
whose other pin is bound inside its braces, a device attached by one pin, a
`*N` group. Either mismatch is **E-49**. The buck idiom reads attach, stay,
stay, advance:

```
SW = K.{D2~D-SS34: .A=GND;} == .{C3~C-100nF-0603: .=BUCK-BST;} == A.{L1~L-4u7H}.B
   = .{C4~C-10uF-0805: .=GND} == 5V;
```

A deliberate short is a multi-pin terminal, never a chain trick: `..{R1~R}`
takes both casual pins onto one node, `[A,K].{D1~D}` does it by name, and
`VIN = [1,2].{J5~CONN}.[3,4] = GND` parallels connector pins two per side.
W-02 is quiet about a short spelled this way and fires on one that happens
across separate statements.

## Ground, rails, references

- `GND &TYPE=GROUND;` is mandatory (**E-24**). Several grounds are allowed;
  tie them with a chain: `AGND = .{FB1~FERRITE}. = GND;`.
- A rail with `POWER<` consumers needs one `POWER>` source (**E-27**, two is
  **E-28**). The source is a regulator output or a connector's supply pin,
  declared in the part. A rail that has no such pin — it arrives through an
  inductor, a diode-OR, a resistor, or whichever connector has a supply
  plugged in — is declared on the board: `5V &TYPE=POWER;`. That is the
  fix for any E-27; never untype the pin or the part to dodge the check.
- Every net is written at least twice (**E-26**). A deliberate single
  reference is a stub: `TP1 = U2.MISO &STUB;`. Ports, globals and harness
  declarations are exempt.
- A `&TYPE=POWER<` pin wants a capacitor within two nodes (**W-04**). Put the
  decoupling on the rail in the same section as the consumer.

## Directives

A directive at the end of a statement covers every net in that chain — every
replicated copy, every `^` segment — and nothing inside a `:` binding or a
part. Split a statement to narrow the scope. Strength: `&~` weak, `&` normal,
`&!` locked; equal strength with different values is **E-12**.

```
5V &CLASS=power &CURRENT=3A;
USB-DP = MCU-DP &IMP=90RD;
BUCK-BST &RENDER=WIRE;
{J1~CONN-USB-C: &EDGE=LEFT; .VBUS = VBUS; .GND = GND; };
```

## Blocks, cables, substitution

A block's interface is its arrowed nets (`>IN`, `OUT>`, `<>i2c`); a port with
no arrow is **E-32**. Rails enter by global import (`>>GND`), never as ports.
Instantiate a block like a part; a weak `#~param` in the block is overridden
at the call site and read back with `$"param"$` — quote the name, because a
bare hyphen inside `$…$` is subtraction.

A `cable` is its own top with its own netlist and BOM. It holds only cable
connectors, wires and crimps (**E-44**). The board's connector names the loom
with `@mate`; the loom's plug names the connector with `@mates`; `@map` says
how the pins line up when it is not one to one.

Patterns for all of the above, written in the house style, are in
`references/idioms.md`. Run `references/checklist.md` before saying a design
is done.
