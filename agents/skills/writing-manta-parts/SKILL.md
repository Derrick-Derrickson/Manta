---
name: writing-manta-parts
description: Author a manta part or part library — pin maps, pin types, default nets, casual pins, swap groups and BOM fields. Use when adding a component to a library, converting a datasheet into a part, or diagnosing a board error that traces back to a mis-declared part.
---

# Writing manta parts

A part maps a package's physical pins to named pins and declares its fields.
It is where most avoidable board errors originate, because a part's annotations
are what every check on every net using it depends on.

```
part LDO-3V3 {
    @~footprint = SOT-23-5;
    #value      = AP2112K-3.3;
    #!type      = regulator;
    #~mpn       = "AP2112K-3.3TRG1";

    1 = VIN<  &TYPE=POWER;
    2 = GND<  &TYPE=POWER &~NET=GND;
    3 = EN<;
    4 = NC    &TYPE=NC;
    5 = VOUT> &TYPE=POWER;
};
```

Each line is `physical = logical`, with the direction arrow attached to the
logical name and directives after it. A part exports all its pins; there is no
separate export declaration.

## Get the pin types right

This is the part that matters. Every net-level check reads them.

**`&TYPE=POWER` with `>`** on a regulator output, a connector's VBUS, a battery
terminal. One such pin is what satisfies E-27 for every net it feeds. Forget it
and the board reports "unpowered net" no matter what the board does.

**`&TYPE=POWER` with `<`** on a supply input. Brings three checks with it and
needs no annotation in any design that uses the part: no source is E-27, two
sources is E-28, a source with no consumers is W-09, and a supply pin with no
nearby capacitor is W-04.

**`&TYPE=OPENDRAIN`** on an I²C or similar bus pin. Says many drivers are
permitted, so it is exempt from the multiple-driver rule and never counts as an
input waiting for a driver.

**`&TYPE=NC`** where the datasheet forbids connection. Connecting it is E-25.
It weakly implies `&STUB`, so it will not trip the single-reference rule.

**No `&TYPE` and no arrow** is `PASSIVE`, which claims nothing and is skipped by
drive checks. That is right for a resistor or a capacitor and wrong for anything
with a direction.

**No `&TYPE` but an arrow** is `SIGNAL`. A pin that can release a bus is `<>`;
there is no separate tri-state type, and E-01 fires only on multiple `>` pins.

## Default nets

`&NET` names the net a pin joins when nothing binds it. Declare it **weakly**,
so a call site can override:

```
1 = VCC< &TYPE=POWER &~NET=3V3;
2 = GND< &TYPE=POWER &~NET=GND;
```

```
{U1~cool-mcu};                  VCC→3V3, GND→GND
{U2~cool-mcu: VCC=1V8; };       VCC→1V8, GND→GND
```

This removes an enormous amount of noise from a board. Supply and ground pins
should nearly always have one.

Note that `&NET=3V3` names the *rail called 3V3*, not the quantity 3.3 volts —
the directive's type decides which reading the word takes.

## Casual pins

`&CASUAL` grants two things: the pin may be chosen by a `.` terminal, and it
joins a weak implicit swap group.

Mark both pins of a genuinely symmetric part — a resistor, a capacitor, an
inductor, a ferrite. Do **not** mark a diode's or a transistor's, which is what
E-23 protects against.

```
part R-10kR-0603 {
    @~footprint = R-0603;
    #value      = 10kR;
    #!type      = resistor;
    1 = A &CASUAL;
    2 = B &CASUAL;
};
```

The implicit group is scoped per declaration line, so two independent elements
in one package do not become mutually swappable:

```
part dual-resistor {
    [1:2] = A[1:2] &CASUAL;      // one group
    [3:4] = B[1:2] &CASUAL;      // a different one
};
```

## Swap groups

`&SWAP` names a permutable *index*, not a set of pins. Every array tagged with
the same index permutes together, which is what makes a ganged swap
expressible:

```
part buffer4 {
    [1:4] = IN[1:4]<  &SWAP=ch;
    [5:8] = OUT[1:4]> &SWAP=ch;
};
```

Exchanging channels 2 and 3 permutes `IN` and `OUT` identically. Differing
directions are expected here and do not freeze the group; differing *directives*
do, and give W-08.

## Arrays and harness members

A contiguous run of physical pins maps to an array, and the widths must match:

```
[3:11] = GPIO[1:9]<>;        nine pins, nine signals
```

Range order is significant and defines wire order. A harness member list is
written out so the mapping is stated where it is read, and its length must equal
the pin range width:

```
[12:13] = USB.[+,-]<>;
[20:22] = i2c.[SDA,SCL,ALERT]<>;
```

Pin names follow the ordinary identifier rules and may additionally be an
integer. They may **not** end in a hyphen — a part needing a negative supply
should name it `v-neg`, not `V-`.

## Fields, and what they are for

```
@~footprint = R-0603;        weak: a call site may substitute a package
#value      = 10kR;
#tolerance  = ±1%;
#power      = 100mW;
#!type      = resistor;      locked: never overridden
#~mpn       = "RC0603FR-0710KL";
#~cost      = 0.002;
#~supplier  = digikey;
```

`@footprint` is required for any fitted part; without it, E-20.

`#` is an open namespace carried to the BOM untouched. Declaring `#mpn` weakly
lets a second source be substituted at a call site without editing the part.

Two conventions the checker relies on. `#type = capacitor` is how W-03 and W-04
recognise a capacitor — the language has no notion of one otherwise. And
`#!type` locked is a good default for a part's identity, which is not something
a call site should change.

## Linkage

Mark a part `static` only when it is genuinely private to one file. Internal
linkage makes it invisible to every other object, which is exactly what you do
not want for a shared library, and the failure looks like E-31 "referenced but
never declared" from the board.

Two libraries may each declare a `static part house-resistor-0603` without
conflict; two libraries declaring the same name externally is E-30.

## Check it

A part library on its own does not link — there is no top-level block. Check it
by writing a design that uses every part, which is what `examples/blinky` does:

```sh
manta compile -o build/ parts.manta board.manta
manta check --top <block> -L build/ -Werror
```
