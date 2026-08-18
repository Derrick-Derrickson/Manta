# Every diagnostic, and what actually fixes it

Codes are as the specification numbers them. Anything lettered — `E-SYNTAX`,
`E-TYPE`, `E-IO`, `E-UNANNOTATED` — is outside that space and cannot collide
with it.

Each entry can be enabled, silenced or re-graded by code or by mnemonic:
`-Wno-W-03` and `-Wno-cap-in-series` are the same instruction.

---

## Connection and structure

**E-49 — `an unpaired '=='`**
`==` is a node bracket and comes in adjacent pairs around exactly one element:
`X == .{C1~cap: .=GND} == Y`. A lone `==` — the pre-1.6 "same net" spelling —
never closes. The plain join, two bare net names included, is a single `=`.

**E-04 — width mismatch**
Two sides of a connection differ in width and neither `=*` nor `*=` was written.
Manta never truncates, pads or reindexes. Either fix the widths, or say what you
meant: `=*` gathers an array onto one net, `*=` broadcasts one net across an
array. Index bases need not match — `D[1:8] = bus[0:7]` is fine, both are eight
wide and both ascend.

**E-05 — replication width not divisible by unit arity**
`[[ ]]` makes one copy per element flowing through it, so the incoming width has
to divide by the unit's input arity. A two-input unit cannot take a three-wide
bus.

**E-06 — `[N[ ]M]` widths disagree with unit arity**
The two bracketed numbers are repeated on the closing delimiter so a mismatch is
visible. `N / in_arity` must equal `M / out_arity`.

**E-37 — multiplicity applied to a replication**
`+N`, `|N` and `*N` apply to a parenthesised group, never to `[[ ]]`. Wrapping
the replication in parentheses does not launder it.

---

## Devices and pins

**E-07 — reference to a designator never declared**
`{U3}` without a `~` references an existing instance. Something has to declare
`U3` first, with `{U3~part-name}`. A designator meant for several references
should be author-assigned rather than left as `?`.

**E-08 — a pin is both a chain terminal and in the binding list**
`A{D1~diode: A=VIN}K` binds `A` twice. Pick one.

**E-09 — empty binding list written with punctuation**
`{L1~ind:}` or `{L1~ind;}`. Write the device bare: `{L1~ind}`.

**E-23 — a `.` terminal selects a pin without `&CASUAL`**
`.` means "the next unassigned pin", which is only meaningful when the pins are
interchangeable. A diode's pins are not. Either name the terminal explicitly
(`A{D1~diode}K`) or, if the part really is symmetric, mark its pins `&CASUAL`.

**E-25 — a pin marked `&TYPE=NC` is connected**
The datasheet forbids it. If you meant "I chose not to connect this", that is
`&NET=?`, which is always legal and needs no `&STUB`.

**E-31 — a name is referenced but never declared**
A part, block, netclass, match group or pin that does not exist. Check spelling,
and check the object is actually on the link line — a `static` declaration has
internal linkage and is invisible to other objects.

---

## Nets

**E-01 — two or more `>` pins drive one net**
Two outputs fighting. If they are meant to share a bus, declare them
`&TYPE=OPENDRAIN`; if one can release the bus, it should be `<>`.

**E-02 — a net has an input and no driver** *(also: an identifier ends in `-`)*
A net that is nothing but input pins. A `>` pin, a `&TYPE=POWER>` supply or any
passive pin all count as driving it, so a pull-up or a rail-tied enable will not
trip this — what does is an input nobody connected.

The same code covers an identifier ending in a hyphen, which is a lexical rule
and unrelated. The message distinguishes them.

**E-26 — a net is referenced exactly once and does not carry `&STUB`**
Almost always a typo, including a mistyped harness member. If deliberate — a
test point — write `&STUB`. Ports, globals and harness declarations are exempt.

**E-33 — `&STUB` on a net with more than one pin**
A stub carries exactly one pin. If it has two, it is a real net and the `&STUB`
is wrong.

**E-27 — a power net has consumers and no source**
Some part has to declare a `&TYPE=POWER>` pin. A regulator's output, a
connector's VBUS. Ground is exempt.

**E-28 — two `&TYPE=POWER>` pins on one net**
Two supplies shorted together. Usually a binding mistake.

**E-24 — the design declares no ground**
Add `GND &TYPE=GROUND;`. Ground is declared, never inferred, and a design may
declare several.

---

## Fields and directives

**E-10 — unknown `@` field** / **E-13 — unknown `&` directive**
Both namespaces are closed because the compiler interprets them. The diagnostic
suggests the nearest known name. If you want to carry arbitrary data, use `#`,
which is open and passes through to the BOM untouched.

**E-11 — override of a locked field**
`#!name` cannot be overridden. If a call site needs to change it, the part
should declare it normal or weak.

**E-12 — two declarations of equal strength with different values**
Make one of them stronger, or make them agree. Weak < normal < locked; the
strongest wins and only a tie is an error.

**E-34 — a fixed-set value is not upper case**
`&TYPE=power` should be `&TYPE=POWER`, `@fitted=false` should be `FALSE`. User
field values are unconstrained: `#status = power` is fine.

**E-43 — a `part` exports a field**
Export is available to blocks only.

**E-20 — a fitted part has no `@footprint`**
Either give it one, or mark the instance `@fitted=FALSE` if it genuinely is not
placed. Note that `@fitted` and `@bom` are independent.

---

## Substitution

**E-29 — substitution of an undefined field**
The field is not visible in the current scope. Remember fields flow *downward*
only — a part cannot see a field its caller declared unless the caller is a
block that encloses it.

The commonest cause is a hyphenated name: `$r-value$` is `r` minus `value`, and
both are undefined. Write `$"r-value"$`.

**E-39** division by zero · **E-40** an integer given to `&`, `~` or `|` ·
**E-41** a dimensioned value, string or list used as an operand ·
**E-42** a negative exponent.

Arithmetic is integers and booleans only. Comparison is the bridge: it takes
integers and yields a boolean. A unit goes outside the delimiters —
`$100 * 2$R`, not `$100R * 2$`.

**E-15 — substitution in a non-value position**
A substitution may sit inside an identifier, as a field value, as an array index
or as a directive value. It may not produce a statement, a declaration or an
operator.

---

## Whole-design

**E-30 — a name is declared in more than one object**
Two objects declare the same name with external linkage. Rename one, or mark it
`static` if it is meant to be private to its file.

**E-36 — an `@VERSION` constraint the toolchain cannot satisfy**

**E-21 — a harness name collides with a designator**
`i2c.SDA` and `U1.GPIO1` are spelled identically, so a harness sharing a name
with a designator makes member access ambiguous. Rename one.

**E-14 — single-ended `&IMP` on a differential harness**
Use the `D` suffix: `&IMP=90RD`.

**E-18 — a match tolerance given as a length**
What matters is propagation delay, and identical lengths do not give identical
delays — a microstrip and a stripline see different effective permittivities.
Write a time.

**E-38 — a harness member list whose length differs from its pin range**

**E-17 — an imperial unit literal**
Metric only. Package codes like `0603` are identifiers naming a footprint
family, not measurements, and are unaffected.

**E-UNANNOTATED — an instance still carries `?`**
Run `manta annotate`. To bootstrap a design that has never been annotated,
demote the check for the one link that produces the netlist to annotate from:

```sh
manta link --top board -L build/ -Wno-unannotated -o build/board.mantaNets
manta annotate -n build/board.mantaNets src/*.manta
manta link --top board -L build/ -o build/board.mantaNets
```

---

## Warnings

**W-01 — a part has pins in no chain and no binding**
Catches an unused section of a multi-unit package. Often correct to leave, but
worth a look.

**W-02 — a two-terminal device is shorted by a `==` run**
Legal and sometimes intended — a zero-ohm link, a footprint kept for a later
build. If not intended, one of those `==` should be `=`.

**W-03 — a capacitor is in series with two non-ground nets**
Usually a decoupling cap whose second pin went to the wrong net. A capacitor is
recognised by `@type = capacitor` or by being two-terminal with a farad `#value`.

**W-04 — a `&TYPE=POWER<` pin has no capacitor within two nodes**
Missing decoupling.

**W-06 — a weak field is never overridden** *(off by default)*
Enable with `-WW-06`. Asks which of your suggestions nobody took. Off by default
because a part library declares `@~footprint` weakly on purpose.

**W-07 — two identifiers differ only by `-` versus `_`**
`SIG-A` and `SIG_A` are two different nets. Almost always meant to be one.

**W-08 — a swap group's members carry incompatible directives**
The group is frozen and the router will not permute it. Compatibility is judged
on directives, not arrows — a ganged swap deliberately pairs inputs with
outputs.

**W-09 — a `&TYPE=POWER>` net has no consumers**
A regulator feeding nothing.

## Connectors and cables

**E-44 — a cable holds something that is not a cable part**
A cable takes a cable connector, a wire or a crimp, and nothing else. If a
resistor belongs in the loom, it is an inline part on a board, not a conductor.

**E-45 — a `@mate` names a cable whose connectors do not fit**
Either the name is not a cable, or none of its housings declares
`@mates = <this connector's part>`. The message lists what the cable does have.

**E-46 — the mating pins do not line up**
A pin-count mismatch with no `@map`, or a `@map` naming a pin that does not
exist on one side. Write the map, or check you have the right lead.

**E-47 — two drivers meet through a cable**
The loom's far end plugs back into this same board, and a conductor joins two
pins that both drive. Almost always a straight-through lead where a crossover
was wanted; `@map` is where the crossover goes.

**E-48 — a supply meets a ground through a cable**
The same trace, finding a supply pin connected to a ground pin. This one is a
short, not a subtlety.

**W-TYPE — a `@type` value is nearly a structural role**
`boardconector` is not `boardconnector`, and the difference is silent: the part
is simply not a connector and every mating check stops applying. The set is open
so this cannot be an error, but it is worth a word.

**W-FOOTPRINT — a footprint names no library** *(export only)*
KiCad resolves `Library:Footprint`, and a bare package name will not place.
`--footprint-map` or `--footprint-lib` supplies the library.

## One thing the checker cannot see

A net whose only driver is on the *next* card in a daisy chain reports **E-02**
on this one: ERC sees a board, and the driver is not on it. That is honest — the
board alone does have an undriven input — but a chained design carries
`-Wno-E-02` or a `&STUB` on its uplink nets.
