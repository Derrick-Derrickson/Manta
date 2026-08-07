# Documentation

**`spec.md`** — the Manta Schematic Definition Language, revision 1.0. This is
the authority: when the implementation and this document disagree, the document
is right and the implementation has a bug.

It carries six editorial corrections against the revision as published, listed
in the note at its head. Two grammar productions were written more narrowly than
the language they describe; four worked examples contradicted rules stated
elsewhere in the same document. No rule changed meaning.

**`assumptions.md`** — every point where revision 1.0 underdetermines behaviour
or contradicts itself, with the resolution taken and the reasoning. Read this
before concluding the implementation is wrong about something: a good number of
apparent bugs are documented decisions.

The entries worth knowing before writing any design:

- **C1** — a *capacitor* has no definition in the language, yet two warnings need
  one. `#type = capacitor` is the convention adopted.
- **C2** — what counts as "driven" for E-02, and why a pull-up does.
- **D4** — W-06 is off by default, and why.
- **D5** — un-annotated designators are an error at link, and the bootstrap that
  makes annotation possible anyway.
