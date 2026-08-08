# The Manta Rules Language

**Companion specification, revision 1.0**

A `.mantaRules` file states checks a project wants enforced that the language
cannot know about. It is a **toolchain extension**, not part of manta: nothing
in `docs/spec.md` depends on it, and a design compiles and links whether or not
any rules file is present.

---

## 1. Why it exists

The 47 checks of the language specification are fixed, because they are the ones
that follow from the language itself: a net with two drivers, a pin marked
not-connected that is connected, a name written once. The interesting checks on
a real board are not like that. They depend on what a project decided its
components' parameters mean.

Two examples, which this language was shaped around:

- **Logic levels.** Every driver on a net has to clear every receiver's input
  threshold. A 3V3 CMOS output feeding a 5V TTL input is a mistake no amount of
  connectivity checking can see, because the connectivity is perfect.
- **Current budgeting.** What a rail can supply, against the sum of everything
  drawing from it.

Both have the same shape: attributes on pins, aggregated over a net, compared.

## 2. Attributes are `#` fields

A rules file reads **`#` user fields**, which are already legal manta. §9.1 of
the language specification makes `#` the open namespace — "carried to BOM and
documentation untouched, unknown names are permitted" — which is exactly what a
user-defined attribute is.

Three consequences follow, and they are the reason for the choice:

- Nothing has to be registered. `#VOH = 2V4` is legal today.
- A decorated design **compiles and links with no rules file**. Rules are pure
  checking, and can be added to a project without changing how it builds.
- Attributes inherit the strength ladder, scoping and override rules that fields
  already have.

A `#` field on a pin map line applies to every pin that line produces, so a wide
bus states a figure once:

```
part MCU-48 {
    @~footprint = LQFP-48;
    [1:48] = IO[1:48]<> #VOH=2V4 #VOL=0V4 #VIH=2V0 #VIL=0V8;
    49     = SDA<>      &TYPE=OPENDRAIN #VOL=0V6;
};

part LDO-3V3 {
    5 = VOUT> &TYPE=POWER #SUPPLY=600mA;
};
```

A call site overrides one the way it overrides any other field — by declaring a
stronger one, since overriding is the strength ladder and not a separate
mechanism:

```
{U1~MCU-48: IO[3] #!VOH=3V0; };
```

## 3. A rules file

```
rules logic-levels {
    #VOH : voltage;      // output high, worst case
    #VIH : voltage;      // input threshold, high

    check drive-high for net.driver -> net.receiver {
        when    driver.direction == out & receiver.direction == in;
        when    has(driver.VOH) & has(receiver.VIH);
        require driver.VOH >= receiver.VIH;
        error   "{driver} drives {net} to {driver.VOH},"
                " but {receiver} needs at least {receiver.VIH}";
    };
};
```

### 3.1 Grammar

```ebnf
file        = { rules_def } ;
rules_def   = "rules" identifier "{" { field_decl | check_def } "}" ";" ;

field_decl  = "#" identifier ":" quantity ";" ;
quantity    = "voltage" | "current" | "resistance" | "capacitance"
            | "inductance" | "power" | "frequency" | "time" | "length"
            | "temperature" | "number" | "boolean" | "text" ;

check_def   = "check" identifier "for" domain "{" { clause } "}" ";" ;
domain      = "part" | "net" | "component"
            | "net" "." identifier "->" "net" "." identifier ;
clause      = "when" expr ";"
            | "require" expr ";"
            | ( "error" | "warning" ) string { string } ";" ;
```

### 3.2 Field declarations are type assertions

`#VOH : voltage;` does **not** register anything — the field was already legal.
It states the quantity, so that units are checked:

```
require sum(pins.DRAW) <= min(pins.VOH);
```

reports *`'<=' compares current with voltage`* rather than silently always
being false. A field a rule uses shall be declared.

### 3.3 Domains

| `for` | Iterates | Bindings |
|---|---|---|
| `part` | each `part` declaration | `part`, `pins` |
| `net` | each net | `net`, `pins` |
| `component` | each component | `component`, `pins` |
| `net.a -> net.b` | ordered pin pairs on one net | `net`, `a`, `b`, `pins` |

**Every rule runs at link**, including a `part` rule. A check has the whole
design in hand, there is one place to look for one, and `manta check` alone is
complete. `manta compile` does not take `--rules`.

A pin is never paired with itself, so a bidirectional pin is not asked to clear
its own threshold.

### 3.4 Clauses

`when` guards: every one must hold or the check does not apply. `require` is
what must then be true. `error` or `warning` gives the message and the severity.
Adjacent string literals concatenate, so a long message wraps without escapes.

A check with no `require` can never fire, and one with no message could not say
anything; both are errors in the rules file.

### 3.5 Expressions

Comparison and arithmetic over quantities, booleans and names.

`has(x)` tests presence — explicitly, so a voltage never has an implicit
truthiness of its own. An **absent** attribute makes a comparison *false* rather
than an error, so a guard written with `has()` decides whether a rule applies.

Aggregates: `sum`, `min`, `max`, `count`, `any`, `all`. Over an empty
collection, `sum` is zero in the declared unit, `count` is zero, `any` is false,
`all` is true, and `min`/`max` are absent — chosen so a guard does not have to
special-case a net nobody decorated.

`pins.DRAW` projects an attribute over a collection. A filter applies to the
elements before projection, so `sum(pins.DRAW where direction == in)` reads as
"sum DRAW over the pins that are inputs".

Arithmetic requires matching units for `+` and `-`; `*` and `/` scale by a bare
number. There is no unit algebra: volts times amps is refused rather than
silently producing watts.

### 3.6 Properties

| On | Available |
|---|---|
| a pin | `direction`, `type`, `name`, `physical`, `component`, `part`, and its `#` fields |
| a net | `name`, `ground`, `global`, `direction`, `pins`, and its directives |
| a component | `designator`, `part`, `name`, `footprint`, `fitted`, `bom`, `pins`, and its `#` fields |

A bare word that is not a binding and not a property stands for itself, which is
what makes `direction == out` work with no enumeration to declare.

### 3.7 Messages

`{...}` holes take a binding, a dotted path from one, or an aggregate over such
a path: `{driver}`, `{driver.VOH}`, `{sum(pins.DRAW)}`.

## 4. Diagnostics

**A check's name is its diagnostic code.** `check drive-high` reports as
`error[drive-high]` and answers to `-Wno-drive-high`, `--warn=drive-high` and
`--error=drive-high`, exactly as a built-in code does.

A name that collides with a built-in code or mnemonic is refused: `-Wno-E-22`
has to mean one thing.

Because options are parsed before rules are read, a `-W` name that matches no
built-in is held and resolved once the rules load. One that no rule claims is a
usage error — so a typo in `-Wno-drive-hgih` is reported rather than quietly
leaving the check switched on.

## 5. Using it

```sh
manta compile -o build/ src/*.manta
manta check  --top board -L build/ --rules project.mantaRules -Werror
manta link   --top board -L build/ --rules project.mantaRules -o build/board.mantaNets
```

Note that `compile` takes no `--rules`: the design's `#` fields are ordinary
manta and compile fine on their own.

`--rules` is repeatable. Findings are deterministic: domains are enumerated in
netlist order, so the same design and the same rules give the same output every
run.

## 6. Performance

The pin-pair domain is quadratic in the pins on a net — a 200-pin power rail is
40,000 pairs per check. Guards that mention only one binding are hoisted out of
the inner loop and used to filter each side once, which makes the common case
linear in practice. Writing the cheap, one-sided guards first costs nothing and
helps:

```
when driver.direction == out & receiver.direction == in;   // hoisted
when has(driver.VOH) & has(receiver.VIH);                  // hoisted
require driver.VOH >= receiver.VIH;                        // per pair
```
