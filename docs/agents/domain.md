# Domain docs

This repository uses a single-context domain-documentation layout.

## Before exploring

- Read `CONTEXT.md` at the repository root when it exists.
- Read ADRs under `docs/adr/` that affect the area being changed.

Proceed silently when these files do not exist.
Create them lazily through the `/domain-modeling` skill when terminology or architectural decisions are resolved.

## Layout

```text
/
|-- CONTEXT.md
|-- docs/
|   `-- adr/
`-- src/
```

## Use glossary vocabulary

Use domain terms as defined in `CONTEXT.md` in issue titles, proposals, hypotheses, and test names.
Reconsider terms that are absent from the glossary, or record a genuine terminology gap for `/domain-modeling`.

## Surface ADR conflicts

Explicitly identify output that conflicts with an existing ADR.
Name the ADR and explain why reopening the decision may be justified.
