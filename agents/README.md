# Agent skills

Instructions for an AI working with manta. Four skills, for four different
jobs.

| Skill | For |
|---|---|
| `skills/writing-manta-designs/` | Writing a board: chains, blocks, harnesses, constraints. |
| `skills/writing-manta-parts/` | Authoring a part library, which is where most avoidable errors originate. |
| `skills/writing-manta-rules/` | Project-specific ERC checks: logic levels, budgets, library policy. |
| `skills/extending-the-compiler/` | Changing this C++ codebase. |

Each skill is a `SKILL.md` that fits in working memory, plus `references/` files
loaded only when the task needs them.

## Using them

If your harness supports skills, point it at `agents/skills/`. Otherwise read
the relevant `SKILL.md` before starting and pull in a reference when it says to.

For compiler work, `AGENTS.md`, `src/AGENTS.md` and `tests/AGENTS.md` at the
relevant directory are the shorter, always-applicable version;
`skills/extending-the-compiler/` is the deeper material.

## The one thing to internalise

**The compiler is the oracle, not your judgement.** Manta has over fifty
diagnostics because a schematic has many ways to be quietly wrong. Do not
reason about whether a design is correct — run it:

```sh
manta compile -o build/ src/*.manta
manta check --top <block> -L build/ -Werror
```

A design is finished when that is silent, and not before. Reporting a design as
done without running it is the single most likely way to be wrong here.

## The house style, in one line each

- A passive network is one chain, source to load; a shunt is passed with `==`.
- A large IC or a connector is a standing binding block, every pin bound.
- A part file is the declaration, `---`, then the datasheet excerpt — facts
  only, pad numbers checked against the footprint, nothing about any board.
- A comment explains the circuit, never the language.
