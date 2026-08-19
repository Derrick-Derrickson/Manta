# Idioms

Patterns that come up on every board, written the way manta wants them.

## Decoupling

A shunt has no exit terminal, so `==` continues the chain past it. That is what
makes a run of capacitors read naturally:

```
3V3 = .{C1~C-10uF-0805: .=GND}
   == .{C2~C-100nF-0603: .=GND}
   == .{C3~C-100nF-0603: .=GND};
```

Several identical caps on one node are a multiplicity group. `*N` asserts only
that N copies hang off the current node; each copy's other terminals are settled
by its own bindings:

```
3V3 = ({C?~C-100nF-0603: .=GND}.)*4;
```

And when the far ends differ, supply them per copy:

```
({C?~C-10uF-0805: .=%[GND,AGND,GND]}.)*3
```

## Pull-ups and pull-downs

```
3V3 = .{R1~R-10kR-0603}. = nRESET;
```

`=` joins on both sides: the rail to one pin of the resistor, and the
resistor's far pin to `nRESET`.

Do not expect E-02 on the pulled net: a passive pin counts as driving it, which
is what stops the rule firing on every pull-up ever written.

## Series-parallel

```
(.{L?~ind}.)+2          two in series along the chain
(A{D?~dio}K)|2          two in parallel: entries common, exits common
```

`+N` and `|N` pass the chain through the group. `*N` does not — it hangs copies
off one node.

## Replicated channels

`[[ ]]` makes one copy per element of the array flowing through it. The count
follows from the unit's arity and the connection width, so it is never written:

```
>SIG[0:3] = [[ I{U?~AMP012: PWR=3V3; GND=GND}O ]] = OUT[0:3]>;
```

Four copies. Each amplifier gets its own signal because the bus is indexed; all
four share `3V3` and `GND` because a scalar broadcasts.

To give each copy something different, use `%`:

```
[[ I{U?~AMP012: EN=%AMP-EN[0:3]; PWR=3V3}O ]]
```

Where the unit is not one-in one-out, state the widths and let the compiler
check them:

```
[4[ I{U?~splitter}O[0:1] ]8]     // 1-in 2-out, 4 copies, 8 out
```

## Differential pairs

`diff` is built in, with members `+` and `-` in that order:

```
USB &HARNESS=diff;
<>USB.+ = .{R1~R-50R-0603}. = MCU-USB.+;
<>USB.- = .{R2~R-50R-0603}. = MCU-USB.-;
```

An impedance on a pair takes the `D` suffix — `&IMP=90RD`. Single-ended is
**E-14**.

Better, put the constraint on the harness type so every instance inherits it:

```
harness usb2 {
    D &HARNESS=diff;
    &!IMP     = 90RD;
    &MAXDELAY = 600ps;
};

usb-host &HARNESS=usb2;
usb-dev  &HARNESS=usb2;
```

## Buses

A harness need not be declared. Writing `i2c.SDA` implies one, and further
members accrue as used — which is why a mistyped member is caught by E-26
rather than silently becoming a new net.

```
i2c &HARNESS=i2c-bus;
<>i2c;
3V3 = .{R4~R-10kR-0603}. = i2c.SDA;
```

Assigning a whole harness assigns members pairwise by name:

```
USB = MCU-USB;
```

## Delay matching

A group with a reference and destinations. Tolerance is a **time**, never a
length:

```
match ddr-addr {
    @src       = U1;
    @dest      = [U5, U6, U7];
    @tolerance = 5ps;
};

ADDR[0:15] &MATCH=ddr-addr;
DDR-CLK    &MATCH={ddr-addr: @!offset=10ps};
```

A scalar tolerance is a star topology — every destination within that figure of
the source. A list gives each its own budget, which is how a flyby chain is
written: `@tolerance = (5ps)*3`.

## Constraints on many nets

```
netclass power {
    &CURRENT  = 3A;
    &!VOLTAGE = 60V;
};

3V3 &CLASS=power;
12V &CLASS=power &CURRENT=8A;      // per-net wins at equal strength
```

## Not-fitted parts

```
{!R1~R-0R-0603};                   // sugar for @fitted=FALSE
{!BLK1~audio-stage}OUT;            // cascades to every part within
```

DNP affects BOM and ERC only. The netlist is unchanged: the footprint is placed,
the pads exist, the copper is routed. Nets downstream of an unfitted series part
are treated as intentionally open, so E-02 is suppressed across that boundary.

`@fitted` and `@bom` are independent:

```
@fitted=FALSE; @bom=TRUE;      // DNP resistor: on the BOM, flagged, quantity zero
@fitted=TRUE;  @bom=FALSE;     // printed antenna: exists, nothing purchased
```

## Multi-unit packages

One designator, several statements. Bindings are declared once, on the statement
that declares the instance; later references inherit them:

```
SIG-A-IN = INA{U20~LM324: v-pos=3V3; v-neg=GND; }OUTA = SIG-A-OUT;
SIG-B-IN = INB{U20}OUTB = SIG-B-OUT;
```

This is the case where the designator must be author-assigned rather than `?`,
since `{U20}` needs `U20` to exist.

## Things that are not connections

`^` places elements in one statement without connecting them. Use it for a
physically associated group whose internal relationship is not expressed here —
a mated connector pair, for instance:

```
PANEL-OUT = 1{J1~conn-4} ^ {J2~conn-4}1 = PANEL-RETURN;
```

Where a connection *is* intended, use `==`.

## Parameterised subcircuits

Declare the parameter weakly so a call site may override it, and quote the name
inside the substitution if it contains a hyphen:

```
block indicator {
    #~series-r = 1;
    >DRIVE;
    DRIVE = .{R1~R-$"series-r"$kR-0603}. = LED-A;
    LED-A = A{D1~LED-0603}K = GND;
};
```

A designator inside a block is annotated once in its own scope, however many
times the block is instantiated. Export flattens the path, so these become
`BLK1_R1` and `BLK2_R1`.
