# Artifact schemas

JSON Schema (draft 2020-12) for the two formats the toolchain produces.

**`mantaO.schema.json`** — the object emitted by `manta compile`. It carries
declarations, their interfaces and their bodies in intermediate form, with
external names unresolved and substitutions unevaluated. It is the parse tree:
a block cannot be elaborated until instantiated, since a replication derives its
count from the call site and a locked override changes what is emitted.

**`mantaNets.schema.json`** — the netlist emitted by `manta link`. Components,
nets with their pins and directives, delay-matching groups, and an optional
`swaps` array. A cable produces one of these too: it is its own deliverable, and
its components are the housings, wires and crimps that make up a loom.

## Validating

```sh
python3 tests/validate_schema.py schema/ build/linux-release/tests/pipeline-work
```

`ctest` runs this as the `schema` target after `pipeline`, and skips cleanly
when `jsonschema` is not installed.

## Three things to know

A net pin entry carries **both** `pin`, the physical package pin a layout tool
routes to, and `logical`, the name the part declares for it. The specification's
example shows one of each and does not say which is meant; emitting both settles
it. It also carries `type` and `direction`, which no other part of the
interchange records and which a layout tool needs — KiCad puts them on the pad
and its design-rule check reads them. Both are optional, so a netlist written
before they existed still validates.

A component carries `type`, from `@type`. Five of its values are structural and
the compiler interprets them; the rest are ordinary classifications carried
through untouched. It is optional, and absent means `board_part`.

The `swaps` array is a documented extension. The specification requires
`manta annotate --swaps` to reconcile layout swaps back to source but gives the
annotator no file to read them from. It is optional, so a netlist without it
still validates and `--swaps` becomes a no-op.
