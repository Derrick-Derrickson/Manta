---
name: writing-manta-rules
description: Write project-specific ERC checks in a .mantaRules file, and decorate a design with the '#' fields they read. Use when asked to check logic-level compatibility, current or power budgets, library policy, or any electrical rule the built-in checks do not cover.
---

# Writing manta rules

The built-in checks are the ones that follow from the language: two drivers on a
net, a not-connected pin that is connected, a name written once. The interesting
checks on a real board depend on what a project decided its components'
parameters mean, and those go in a `.mantaRules` file.

`docs/rules.md` is the specification. This is how to use it.

## Decorate first, then check

Attributes are **`#` user fields**, which are already legal manta. Nothing needs
registering, and a decorated design compiles and links with no rules file
present — so decorating is safe to do before any rule exists.

A `#` field on a pin map line applies to every pin that line produces:

```
part MCU-48 {
    @~footprint = LQFP-48;
    [1:48]: IO[1:48]<> #VOH=2V4 #VOL=0V4 #VIH=2V0 #VIL=0V8;
};

part LDO-3V3 {
    5: VOUT> &TYPE=POWER #SUPPLY=600mA;
};
```

Take the figures from the datasheet's *worst case*, not its typical column. A
rule checking typical numbers passes boards that fail.

A budget has two shapes. A rail with a sourcing pin sums `#DRAW` against that
pin's `#SUPPLY`. A rail with none — it arrives through an inductor, a diode-OR
or a connector — carries `&CURRENT` on the net or its class, and the rule
reads it off the net: `when has(net.CURRENT); require sum(pins.DRAW) <=
net.CURRENT;`. Write both, as blinky does, or a declared rail is never
checked at all.

## The shape of a check

```
rules logic-levels {
    #VOH : voltage;      // declare the quantity, so units are checked
    #VIH : voltage;

    check drive-high for net.driver -> net.receiver {
        when    (driver.direction == out) | (driver.direction == bidir);
        when    (receiver.direction == in) | (receiver.direction == bidir);
        when    has(driver.VOH) & has(receiver.VIH);
        require driver.VOH >= receiver.VIH;
        error   "{driver} drives {net} to {driver.VOH},"
                " but {receiver} needs at least {receiver.VIH}";
    };
};
```

`when` decides whether the check applies. `require` is what must then be true.
The message says what went wrong with the actual numbers in it. Include
`bidir` on both sides: a GPIO, a bus pin and a connector position are all
`<>`, so a guard that only accepts `out` and `in` examines nothing on a real
board. The closed sets are `direction` = `in out bidir none` and `type` =
`passive signal power opendrain nc ground`; a pin is never paired with
itself.

## Four things that will bite you

**Guard with `has()`, always.** An absent attribute makes a comparison false, so
an unguarded `require d.VOH >= r.VIH` fires on every pin that simply has not
been decorated yet. That is noise, and noise gets rules switched off.

**Declare the quantity of every field you use.** `#VOH : voltage;` is what turns
`sum(pins.DRAW) <= min(pins.VOH)` into an error instead of a comparison that
silently always fails. The quantities are `voltage current resistance capacitance
inductance power frequency time length area temperature number boolean text`.

**Put one-sided guards first in a pin-pair check.** They are hoisted out of the
quadratic inner loop and used to filter each side once. A 200-pin power rail is
40,000 pairs otherwise.

**Name checks carefully.** A check's name *is* its diagnostic code, so it is
what someone types in `-Wno-`. `drive-high` is good; `check1` is not. It may not
collide with a built-in code or mnemonic.

## What the language leaves to you

Two checks are deliberately not built in, because the answer is a project's and
not a language's. Both read what `@type` and `@mate` put on a component.

```
check every-connector-is-mated for component {
    when    component.type == boardconnector;
    require has(component.mate);
    error   "{component} is a connector and nothing says what plugs into it";
};

check conductors-are-thick-enough for part {
    // IPC-2221, external layer, 1oz copper, 30C rise. A number worth arguing
    // with, which is exactly why it lives here.
    when    part.type == wire;
    when    has(part.csa);
    require part.csa >= 100000um2;
    error   "{part.name} is {part.csa}, thinner than this project allows";
};
```

A rule that mentions cable parts often needs the converse too: a footprint rule
written for a board will fire on every wire and crimp until it says
`when part.type != wire;`.

## Both halves, every time

A rule that never fires is useless. A rule that always fires is worse, because
people switch it off and then it protects nothing.

So write two designs for every rule: one that violates it and one that does not.
Run both.

```sh
manta compile -o build/ src/*.manta
manta check --top board -L build/ --rules project.mantaRules -Werror
```

Rules run at link, so `compile` takes no `--rules`. The `#` fields a design
carries are ordinary manta and compile on their own.

`tests/rules/` follows exactly this pattern — `violations.manta` and
`clean.manta` against one rules file — and `examples/blinky` carries a real
`blinky.mantaRules` that must stay silent.

## Domains

| `for` | Iterates | Use it for |
|---|---|---|
| `net` | each net | budgets, anything aggregated over a net |
| `net.a -> net.b` | pin pairs on a net | anything relating one pin to another |
| `component` | each component | per-part policy on an instantiated design |
| `part` | each part declaration | library policy |

Every domain runs at link, `part` included.

## Aggregates

`sum` `min` `max` `count` `any` `all`, with `pins.ATTR` projecting and
`where` filtering:

```
require sum(pins.DRAW where direction == in) <= sum(pins.SUPPLY);
```

Over an empty collection `sum` is zero in the declared unit and `min`/`max` are
absent — chosen so a guard does not have to special-case an undecorated net.
