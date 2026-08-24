# Prototype Workspaces as whole-file copy-on-write over an immutable Base state

The prototype for issue #1 merges one sparse per-Workspace upper tree over one immutable, materialized Base state.
It copies a complete file into the upper tree on its first mutation and records removals as persistent tombstones in SQLite.

The experiment does not use chunk-level deduplication or a content-addressed store because its verdict does not depend on that storage machinery.
It also rejects the Project source as a live lower layer because later Project changes would make Workspaces irreproducible.

This decision applies only to the throwaway prototype.
It provides evidence for the production design but does not commit production to this architecture.
The prototype implementation stays off `main`.
