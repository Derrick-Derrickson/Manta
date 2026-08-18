---
name: writing-manta-designs
description: Write or modify a manta schematic — boards, blocks, chains, harnesses, nets and electrical constraints. Use when asked to describe a circuit in manta, add a subcircuit to an existing design, or fix diagnostics a manta build reported. Covers the connection operators, replication, fields, directives and the checks that will judge the result.
---

# Writing manta designs

Manta describes one board: its components, their interconnections, and the
electrical constraints on those interconnections. A design is a set of named
declarations; order never matters and a name may be used before it is declared.

## Work in this order

1. **Read `docs/spec.md`** for anything you are unsure of. It is the authority.
2. **Write the parts first**, or find them. Most errors in a design trace back
   to a part whose pins are typed wrongly. See the `writing-manta-parts` skill.
3. **Write the board**, following the shape in `examples/blinky/board.manta`.
4. **Build it, and read what the compiler says.** Not optional:

```sh
manta compile -o build/ src/*.manta
manta check --top <block> -L build/ -Werror
```

A design is finished when that is silent. `examples/blinky` is a complete board
that achieves it; copy its structure.

## The five connection operators

This is where most mistakes are made, so learn them properly.

```
A = B                     both names are one net
A = .{R1~res}. = B        A and B are different nets, joined through R1
X == .{C1~cap: .=GND} == Y   the pair brackets C1 onto the node; X and Y are one net
A ^ B                     placed in one statement, not connected at all
A[0:3] =* B               gather: four wires shorted onto one
A *= B[0:7]               broadcast: one net to every element
```

`=` is **the plain join**: it connects the exit of the left element to the
entry of the right. Through a device the node advances; between two bare names
there is nothing to advance through, so `A = B` simply puts both names on one
net — that is also how a net gets a second name.

`==` **never appears alone** — an unpaired `==` is **E-49**. An adjacent pair
of `==` brackets exactly one element onto the current node without advancing
it. Bracket a shunt and the chain runs past it; bracket a net name and the
node is named; bracket a two-terminal device and it is **shorted** — legal,
and gives you W-02. The close may fall at the end of the statement
(`Y == .{R2~res}. ==;`). What lets a chain continue past a shunt is exactly
the bracket, because a shunt has no exit terminal:

```
VIN = .{R1~res}. == .{C1~cap: .=GND} == EN;
```

`R1`'s far pin, `C1`'s exposed pin and `EN` are one node. `VIN` is not.

## Ground, and rails

Ground is declared, never inferred. A design that declares none is **E-24**:

```
GND &TYPE=GROUND;
```

A rail needs a source. A net with `&TYPE=POWER` consumers and no `&TYPE=POWER`
supply is **E-27** — so the regulator part must declare its output `POWER>`.
That one annotation satisfies the check for every net it feeds.

## Every net needs two references

A net written exactly once is **E-26**, because a real net is written at least
once at each end. A single reference is almost always a typo — including a
mistyped harness member, which would otherwise silently create a new net.

When a single reference is deliberate, say so:

```
TP1 = U2.MISO &STUB;
```

Ports, globals and harness type declarations are exempt: they are connected from
outside the block.

## Directives are per statement

A directive at the end of a statement covers **every net in that chain** — every
copy a replication made, and every segment across `^`. It does not reach through
a `:` binding or into a part.

```
SW = SW-NODE = S{Q1~fet: G=nEN}D = SWITCHED &CURRENT=3A;
```

`SW`, `SW-NODE` and `SWITCHED` carry the current limit. `nEN` does not. To
exclude part of a chain, split it into its own statement — the statement is the
scope unit, so reflowing across lines changes nothing.

## Blocks

A block is a reusable subcircuit. Its interface is the set of nets carrying a
direction arrow, and the arrow is mandatory — a port without one is **E-32**.

```
block rc-filter {
    #~r-value = 10;              // weak: a call site may override
    >IN;
    OUT>;
    IN = .{R1~R-$"r-value"$kR-0603}. == .{C1~C-100nF: .=GND} == OUT;
};
```

Instantiate it exactly like a part. A block instantiated twice with different
parameters produces two elaborations from one source:

```
>SIG-A = {BLK1~rc-filter: #r-value=10; }OUT = FILTERED-A>;
>SIG-B = {BLK2~rc-filter: #r-value=47; }OUT = FILTERED-B>;
```

A `--- TITLE` line inside a block body names a render section: purely
presentational, no effect on connectivity or checks. `manta render <netlist>`
draws the schematic as clickable HTML, with each section as a titled room.

## Cables

A loom is not on the board, so it is not in the board's block. A `cable` is its
own declaration and its own deliverable: it links on its own and produces its own
netlist and BOM, with the wires and crimps as real line items.

```
cable jumper-8way {
    {J1~JST-8-PLUG}P[1:8]
        = [[ .{C%[1:8]~JST-8-CRIMP}. = .{W%[1:8]~WIRE-22AWG}.
           = .{C%[9:16]~JST-8-CRIMP}. ]]
        = P[1:8]{J2~JST-8-PLUG};
};
```

A cable body is a chain, exactly as a block's is, which is why replication and
ranged designators keep eight conductors to one statement. It may hold only a
cable connector, a wire or a crimp — anything else is **E-44**.

The board says which loom plugs in with `@mate`, the loom says what it plugs into
with `@mates`, and the linker checks that the two fit. When the far end plugs
back into another connector on this same board — a card that daisy-chains into a
copy of itself — each conductor is followed through the loom and judged as though
the two had been wired directly, because once the lead is fitted they have been.

One consequence to expect: a net driven only from the *next* card reports E-02 on
this one, since ERC sees a board and the driver is not on it. Put `&STUB` on the
uplink nets or carry `-Wno-E-02`.

## Substitution

`$…$` is evaluated at link, against the fields in force where the block was
instantiated. Arithmetic is integers and booleans only.

**A hyphen inside `$…$` is always subtraction.** A field name containing one
must be quoted, and forgetting is the most common substitution mistake:

```
$"r-value"$        the field r-value
$r - value$        the field r, minus the field value
```

Units go outside the delimiters: `$100 * 2$R` is 200 ohms.

## Before you say it works

Run the checklist in `references/checklist.md`. When a diagnostic fires and you
do not recognise it, look it up in `references/diagnostics.md` — every code,
what it means, and what actually fixes it.

For patterns that come up repeatedly — decoupling, pull-ups, differential pairs,
replicated channels, delay matching — see `references/idioms.md`.
