# How to read manta

Manta is a text schematic. A file is a set of declarations; a `block` is a
board or subcircuit, a `part` maps a package's pads to named pins, a `cable`
is a loom. Order never matters. Every statement ends in `;`. `//` comments.
Text after a line that is exactly `---` at top level is documentation, not
manta.

## The chain is the whole language

A chain joins elements left to right. Elements are nets (bare names) and
devices (`{...}`). The connector between two elements says whether the chain
moved to a new node:

| Connector | Meaning |
|---|---|
| `A = B` | A and B are one net |
| `A = .{R1~R-10k}. = B` | A enters R1, the chain leaves through R1's other pin, B is a different net |
| `A = .{C1~C-100n: .=GND} == B` | C1's far pin went to GND inside the braces, so the chain stayed on A; B is A |
| `=*` / `*=` | a bus gathered onto one net / one net broadcast to a bus |
| `A ^ B` | in one statement, not connected |

Rule: `=` after an element with a far side (a net, a pass-through device);
`==` after an element with no far side (a shunt, a one-pin attachment, a `*N`
group). A shunt ladder reads `3V3 = .{C1: .=GND} == .{C2: .=GND};`.

## Devices

`ENTRY.{DESIGNATOR~PART: bindings}.EXIT`

- `~PART` declares a new instance; `{U3}` without `~` refers to one already declared.
- `ENTRY.` / `.EXIT` are pins the chain passes through. `A.{D1~D-SS34}.K`
  goes in at the anode, out at the cathode. A bare `.` is "the next unused
  interchangeable pin", legal only on pins marked `&CASUAL` (resistors,
  capacitors). `..{R1}` puts both pads on one node, a deliberate short.
  `[A,K].{D1}` does the same by name; `A[0:1].{D3}.K` enters two pins as a bus.
- **Bindings** inside the braces connect the other pins: `.PIN = NET;`. A
  binding is itself a chain rooted at the pin, so `.FB = .{R3: .=5V;} == .{R4: .=GND;};`
  is a divider hanging off FB. `.PIN = ?` leaves a pin deliberately unconnected.
  `.P[3] = X` binds an array element; `.USB = MCU-USB` binds a member-list pin
  whole (USB.+ to MCU-USB.+ and so on); `.USB.+ = X` binds one member.
- `!` before a designator: placed but not fitted. `?` as a number: not yet annotated.
- A standing device `{U1~MCU: ...};` with no entry or exit is the normal form
  for a large IC or a connector: every pin appears in its binding list.

Reading a big binding list: each line is one pin and the net it lands on;
`?` is unused; a chain after `=` is a network hanging off that pin.

## Nets, ports, rails

- A net exists by being named. `U1.PA9` names the net at that pin.
- `>SIG` input port, `SIG>` output, `<>SIG` bidirectional, `>>GND` global
  import, `3V3>>` global export, `[3V3, GND]>>` both. Ports are a block's
  interface; rails cross blocks as globals.
- `GND &TYPE=GROUND;` declares a ground. `5V &TYPE=POWER;` declares a rail
  that no pin sources (it arrives through an inductor, a diode or a connector).
- `harness i2c-bus { SDA<> &TYPE=OPENDRAIN; ... }` declares a bus type;
  `i2c &HARNESS=i2c-bus;` assigns it; `i2c.SDA` is a member net. `diff` is
  built in with members `+` and `-`. A type carrying `&HARNESS=diff` is a
  pair. `swd.IO` used twice implies a harness without declaring one.

## Groups, arrays, replication

- `(.{R?~R}.)+2` two in series; `(...)|2` two in parallel; `({C?: .=GND}.)*4`
  four hanging off the node. `%[GND,AGND]` gives each copy its own value.
- `BUS[0:3]` is four wires. `[[ ... ]]` makes one copy of its contents per
  wire flowing through; `[4[ ... ]2]` states the in and out widths.
- `R%[6:7]` is a designator carrying several numbers, one per copy.

## Fields and directives

- `@name` system field (`@footprint`, `@fitted`, `@bom`, `@type`, `@mate`,
  `@mates`, `@map`, `@VERSION`); `#name` user field, carried to the BOM.
- Strength: `~` weak, none normal, `!` locked. Strongest wins; equal and
  different is an error. `#!mpn` at an instance overrides the part's `#~mpn`.
- `&NAME=value` at the end of a statement is a directive on every net in that
  chain: `&CURRENT`, `&VOLTAGE`, `&IMP=90RD` (differential), `&CLASS=name`
  (a `netclass` of directives), `&MATCH=group`, `&STUB` (referenced once on
  purpose), `&RAIL` and `&RENDER=WIRE|LABEL` (drawing only). In a part, a pin
  line carries `&TYPE=POWER|OPENDRAIN|NC`, `&CASUAL`, `&SWAP=idx`,
  `&PINDELAY`; in a binding list, `&EDGE=LEFT` on a connector.
- `--- TITLE` inside a block is a drawing section, nothing electrical.
- `$"name"$` substitutes a field at link; inside `$…$` a hyphen is minus.

## Parts

```
part LDO-3V3 {
    @~footprint = SOT-23-5;          // package name; the .fpmap says what KiCad calls it
    #value = AP2112K-3.3;  @!type = regulator;  #~mpn = "AP2112K-3.3TRG1";
    1 : VIN<  &TYPE=POWER;           // pad 1, pin VIN, consumes a rail
    5 : VOUT> &TYPE=POWER #SUPPLY=600mA;   // provides one; #fields feed project rules
    [6:13] : PA[0:7]<>;              // eight pads, an array of pins
    A6 : DP-A<>;                     // pads may be named
    MP : MOUNT;
};
```

Arrow = direction (`>` drives, `<` listens, `<>` either, none = passive).
`&TYPE=POWER` + arrow makes the supply checks apply. Everything after `---`
is the part's datasheet excerpt.

## Cables

`cable name { {P1~PLUG}.P[1:4] = [[.{X%[1:4]~CRIMP}. = .{W%[1:4]~WIRE}. = .{X%[5:8]~CRIMP}.]] = P[1:4].{P2~PLUG}; }`
is a four-conductor loom. A board connector's `@mate = name` says which loom
plugs in; the plug's `@mates = PART` says what it plugs into; `@map` pairs
pins when they are not one to one.

## Decoding one statement

```
.SW = SW = K.{D2~D-SS34: .A = GND;} == A.{L1~L-4u7H}.B = .{C4~C-10uF: .=GND} == 5V;
```

Pin SW is net SW. D2's cathode sits on SW and its anode is bound to GND, so
the chain stays on SW (`==`). L1 enters at A and leaves at B, a new node
(`=`). C4 hangs off that node to GND, so the chain stays (`==`), and that node
is named 5V. Result: a buck output stage, catch diode on the switch node,
inductor in series, bulk cap on the rail.

## Reading a design

1. Find `GND &TYPE=GROUND`, the rails (`&TYPE=POWER`, `&CLASS`), and the ports.
2. Walk each `--- SECTION` in order; a block reads source to load.
3. For each standing device, read its binding list as a pin table.
4. For each chain, apply the connector rule to find where nodes change.
5. Cross-check names: the same net name anywhere in a block is one net; a
   name written once is a mistake unless it carries `&STUB`.
