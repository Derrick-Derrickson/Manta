# Idioms

Patterns that come up on every board, written in the house style: passive
networks as chains, large ICs as binding blocks, comments about the circuit.

## Decoupling

A shunt has no far side, so `==` continues past it. One rail, one chain:

```
3V3 = .{C1~C-10uF-0805: .=GND} == .{C2~C-100nF-0603: .=GND} == .{C3~C-100nF-0603: .=GND};
```

Several identical caps on one node are a multiplicity group, annotated with
a range designator; `*N` hangs copies off the node and each copy's far pin is
settled by its own binding, per copy if they differ:

```
3V3 = ({C%[4:6]~C-100nF-0603: .=GND}.)*3;
3V3 = ({C%[7:8]~C-100nF-0603: .=%[GND,AGND]}.)*2;
```

## Pull-ups, pull-downs, RC at a pin

```
3V3 = .{R3~R-10kR-0603}. = nRESET = .{C4~C-100nF-0603: .=GND};
BOOT0 = .{R9~R-10kR-0603}. = GND;
```

The resistor passes through, the cap dead-ends. E-02 never fires on a pulled
net: a passive pin counts as driving it.

## A network that belongs to an IC pin

Write it in the pin's binding, so the reader finds it at the pin:

```
{U2~MP1584EN-LF-Z:
    .VIN  = VPOS = .{C1~C-10uF-0805: .=GND} == .{C6~C-100nF-0603: .=GND};
    .EN   = .{R5~R-100kR-0603}. = VPOS;
    .FB   = .{R3~R-51kR-0603: .=5V;} == .{R4~R-10kR-0603: .=GND;};
    .COMP = .{R17~R-100kR-0603}. = .{C2~C-470pF-0603}. = GND;
    .FREQ = .{R6~R-200kR-0603}. = GND;
    .BST  = .{C3~C-100nF-0603}. = SW;
    .SW   = SW = K.{D2~D-SS34: .A=GND;} == A.{L1~L-4u7H}.B = .{C4~C-10uF-0805: .=GND} == 5V;
    .GND  = GND;
    .EP   = GND;
};
```

A bare pin binding is only for a pin whose network lives elsewhere. A rail
that only reaches an IC through an inductor has no `POWER>` pin on it, so the
board declares it: `5V &TYPE=POWER &CLASS=logic-5v;`.

## Dividers and level shifting

```
VBUS = .{R1~R-100kR-0603}. = VBUS-SENSE = .{R2~R-47kR-0603}. = GND;
WS2812-Y = .{R8~R-330R-0603}. = WS2812-DATA;
```

## LEDs

Series resistor then diode, anode to cathode, in one chain:

```
LED-DRIVE = .{R1~R-1kR-0603}. = A.{D1~LED-0603}.K = GND;
```

A common-anode RGB is one part with three chains rooted at its cathodes:

```
5V = A-COM.{D4~RGB-LED-CA:
    .K-R = .{R10~R-560R-0603}. = RGB-R-GPIO;
    .K-G = .{R11~R-300R-0603}. = RGB-G-GPIO;
    .K-B = .{R12~R-300R-0603}. = RGB-B-GPIO;
};
```

## Crystal

```
XTAL-IN = .{Y1~CRYSTAL-8MHZ}. = XTAL-OUT;
XTAL-IN = .{C11~C-33pF-0603: .=GND};
XTAL-OUT = .{C12~C-33pF-0603: .=GND};
```

## A large IC

Every pin, in the part's declaration order, unused pins `= ?`. Pins that
feed a series element are bound to a named net and chained from it
elsewhere, because a net must be written twice:

```
{U1~CH32V203C8T6:
    .VBAT  = 3V3;
    .PA8   = WS2812-GPIO;    // TIM1_CH1 drives the strip through U7
    .PA9   = LEFT-TX;        // USART1
    .PC13  = ?;
    .SWDIO = SWDIO;
};
```

## A connector

```
{J1~CONN-USB-C: &EDGE=LEFT; .VBUS = VBUS; .GND = GND; .CC1 = CC1; .CC2 = CC2; .DP = USB-DP; .DM = USB-DM; };
CC1 = .{R13~R-5k1R-0603}. = GND;
CC2 = .{R14~R-5k1R-0603}. = GND;
```

Paralleled positions are a multi-pin terminal: `VPOS = [1,2].{J2~CONN-6P}.[5,6] = GND;`.

## Series, parallel, hanging

```
(.{R?~R-100kR-0603}.)+2          two in series along the chain
(A.{D?~D-SS34}.K)|2               two in parallel: entries common, exits common
({C?~C-100nF-0603: .=GND}.)*4    four hanging off the node
```

`+N` and `|N` pass the chain through; `*N` does not.

## Replicated channels

`[[ ]]` makes one copy per element of the bus flowing through it; a scalar
binding broadcasts, `%` gives each copy its own:

```
LED-DRIVE[0:1] = [[{BLK%[1:2]~indicator: #series-r = 1;}.DRIVE]];
>SIG[0:3] = [[ I.{U?~AMP: .EN=%AMP-EN[0:3]; .PWR=3V3; .GND=GND; }.O ]] = OUT[0:3]>;
LED-DRIVE[0:3] = [4[ A[0:1].{D%[1:2]~LED-BICOLOUR}.K ]2] = LED-K[0:1];
```

State the widths, as in the last line, when the unit is not one-in one-out.

## Differential pairs and buses

```
USB &HARNESS=usb2;                 // the type carries &!IMP=90RD and &MAXDELAY
USB = MCU-USB;                     // whole-harness assignment, member by member
i2c &HARNESS=i2c-bus;
3V3 = .{R4~R-10kR-0603}. = i2c.SDA;
3V3 = .{R5~R-10kR-0603}. = i2c.SCL;
```

A harness need not be declared: `swd.IO` and `swd.CLK` written twice each
imply one, and a mistyped member is caught by E-26.

## Test points and stubs

```
TP1 = U2.MISO &STUB;                // a net referenced once, on purpose
3V3 = .{TP2~TESTPOINT};             // a real pad, on the board, no BOM line
```

## Not fitted, second source, unbound

```
{!R20~R-0R-0603: .A = BOOT0; .B = 3V3; };      // placed, not fitted
.{R7~R-10kR-0603: #!mpn = "ERJ-3EKF1002V"; }.  // second source at the call site
{U5~ISO7741: .GNDB = ?; }                       // deliberately floating
```

## Multi-unit packages and `^`

One designator, several statements; bindings once, on the declaring one:

```
SIG-A = INA.{U3~LM324: .V-POS=3V3; .V-NEG=GND; }.OUTA = OUT-A;
SIG-B = INB.{U3}.OUTB = OUT-B;
```

`^` places two things in one statement without joining them, for a group
whose internal relationship is not electrical: `1.{J3~HDR-2} ^ {J4~HDR-2}.1`.

## Parameterised blocks

```
block indicator {
    #~series-r = 1;
    >DRIVE;
    >>GND;
    DRIVE = .{R1~R-$"series-r"$kR-0603}. = A.{D1~LED-0603}.K = GND;
};
```

A designator inside a block is annotated once in that scope; export flattens
the path to `BLK1_R1`, `BLK2_R1`.

## A cable

```
cable usb-c-1m {
    #length = 1m;
    {P1~USB-C-PLUG}.P[1:4]
        = [[.{X%[1:4]~CRIMP}. = .{W%[1:4]~WIRE-24AWG}. = .{X%[5:8]~CRIMP}.]]
        = P[1:4].{P2~USB-C-PLUG};
};
```

A net whose only driver is on the next board of a daisy chain is driven
through the connector, so the connector's positions are declared `<>`: a
bidirectional pin counts as a driver, and E-02 stays quiet without a flag.
