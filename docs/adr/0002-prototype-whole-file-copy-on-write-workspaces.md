# Prototype Workspaces as whole-file copy-on-write over an immutable Base state

The prototype for issue #1 resolves Workspace contents by merging one sparse
per-Workspace upper tree over one immutable, materialized Base state, copying a
complete file into the upper tree on its first mutation and recording removals
as persistent tombstones in SQLite.

Alternatives considered and rejected for this experiment: chunk-level
deduplication and a content-addressed store, which add storage machinery the
verdict does not depend on; and treating the Project source as a live lower
layer, which would make Workspace behavior track later Project changes and stop
being reproducible.

This decision is scoped to the throwaway prototype. It is evidence for the
production design, not a commitment to it, and the prototype implementation is
kept off `main`.
