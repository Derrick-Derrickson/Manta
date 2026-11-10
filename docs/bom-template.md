# BOM templates

`manta link --bom FILE` writes one row per component: fixed columns, then a
column per `#` field. When a BOM has to take another shape — grouped by value,
columns named and ordered for whoever receives it — describe that shape in a
template, and manta writes it instead:

```sh
manta link --top board -L build/ --bom build/bom.csv \
    --bom-template board.toml --footprint-map board.fpmap
```

manta builds no supplier or assembler format in; a template is the user's.
A file may hold other tables and other `[[output]]` entries, for other
outputs or other tools. manta reads the one entry with `kind = "bom"`,
ignores everything else in the file, and refuses a file with two.
`manta check --bom-template FILE` validates a template against the design
without writing anything.

## Format

The file is TOML. manta reads the part a template needs: `key = value`,
`[table]`, `[[array of tables]]`, strings in either quote, integers,
booleans and arrays. Dotted keys, inline tables, multi-line strings, floats
and dates are refused with a line number.

```toml
[[output]]
kind      = "bom"
file      = "{board}-bom.csv"       # ignored; manta writes to --bom
group_by  = ["{value}", "{footprint}", "{field:mpn}"]
skip      = ["dnp"]
sort      = "{ref}"
separator = ","
refs_separator = ","
quote     = "needed"
columns   = [
    ["Value", "{value}"],
    ["References", "{refs}"],
    ["Footprint", "{footprint_name}"],
    ["Quantity", "{qty}"],
    ["MPN", "{field:mpn|}"],
]
```

| Key | Default | Meaning |
|---|---|---|
| `kind` | — | `"bom"` is the entry manta reads; entries of any other kind are ignored. |
| `file` | — | Ignored. The output path is `--bom`. |
| `name` | — | Ignored; a label. |
| `columns` | — | Required. `[header, template]` pairs, in output order. A header row is always written. |
| `group_by` | none | Templates whose rendered values together make a row. Absent: one row per component. |
| `skip` | none | `"dnp"` leaves unfitted parts off. `"exclude_from_bom"` is accepted; manta never lists an `@bom = FALSE` part anyway. |
| `sort` | `"{ref}"` | Rows in natural order of this template, rendered for each group's first member. |
| `separator` | `","` | Between cells. `"\t"` for TSV. |
| `refs_separator` | `","` | Between designators in `{refs}`. |
| `quote` | `"needed"` | `always`, `needed` (when a cell holds the separator, a quote or a newline), or `never`. |

Any other key in an `[[output]]` is an error: a typo in a BOM template is a
wrong BOM.

## Placeholders

| Placeholder | Value |
|---|---|
| `{ref}` | The component's flattened designator. In `sort`, the group's first. |
| `{refs}` | The group's designators, natural order, joined by `refs_separator`. |
| `{qty}` | Fitted members of the group. A DNP part counts 0 (spec §9.5). |
| `{value}` | `#value`, or the part name when it has none. |
| `{footprint}` | The package name, or `Library:Footprint` through `--footprint-map`. |
| `{footprint_name}` | The part of `{footprint}` after `:`. |
| `{lib}` | The part before `:`; empty without a map. |
| `{part}` | The part name. |
| `{type}` | `@type`. |
| `{fitted}` | `TRUE` or `FALSE`. |
| `{field:NAME}` | The `#NAME` field; empty when absent. `{field:NAME\|text}` gives `text` instead. |

`{{` and `}}` are literal braces. Placement placeholders — `{x}`, `{y}`,
`{y_up}`, `{rot}`, `{side}` — are refused, since a BOM has no positions. So
is any name not listed here, and an argument on a placeholder that takes
none.

## Rules

- Natural order throughout: `R2` before `R10`, `BLK2_R1` before `BLK10_R1`.
- A column outside `group_by` must render the same for every member of a
  group. If two `100nF` parts carry different `#mpn`, that is **E-BOM**
  naming both, never the first one's value: add the field to `group_by`, or
  make the parts agree.
- With `quote = "never"`, a cell holding the separator, a quote or a newline
  is **E-BOM**, and so is a `{refs}` column whose `refs_separator` contains
  the separator.
- Output is UTF-8 with `\n` line endings, byte-identical run to run.
- Any **E-BOM** means `link` writes nothing.
- With `--assembly`, each mated cable's BOM uses the same template.
