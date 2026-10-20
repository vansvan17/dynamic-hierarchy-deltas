# Mapping the Supplied STMBench7 Artifact

This note records what the supplied DomLock/STMBench source actually constructs. It is meant to stop the scalable synthetic experiment from being mislabeled as an STMBench result.

## Source path inspected

The relevant code is under:

```text
DomLock_Artifacts/STMBench/stmbench7 baseline.c++/sb7_lock/src
```

`data_holder.cc` creates composite parts first, then one module, a recursive assembly tree, and links base assemblies to composite parts. Each composite part owns atomic parts, and atomic parts have a ring plus random connections.

The default small parameters in `parameters.cc` are:

- 1 module;
- 3 children per assembly;
- 7 assembly levels;
- 3 composite-part references per base assembly;
- 50 composite parts per module;
- 20 atomic parts per composite part; and
- 6 random outgoing connections per atomic part, in addition to the ring.

From the formulas in `parameters.h`, this gives 729 base assemblies, 364 complex assemblies, 50 composite parts, and 1,000 atomic parts before considering manuals/documents and duplicate references.

## Important separation

The assembly/module/component relation is the containment hierarchy used by the benchmark traversals. The atomic-part connection graph contains cycles by construction. Therefore it must not be fed blindly into a DAG-only hierarchy engine as if every application edge were a lock-hierarchy edge.

The edited DomLock artifact assigns pre/post numbers during recursive traversal and keeps a visited set for atomic connections. That is different from maintaining a dynamic DAG's exact transitive closure.

## Integration boundary

The current sparse engine accepts a hierarchy snapshot as `(node_count, edges)` and returns a validated updated snapshot. A clean STMBench adapter should:

1. give stable integer IDs to the module, assemblies, composite parts, and any atomic objects that are intended to participate in hierarchical locking;
2. export only the hierarchy relation selected for locking;
3. translate structural-modification operations into batch additions/deletions;
4. call the sparse prepare/validate/apply path; and
5. repair DomLock/MID interval metadata only after the structural batch validates.

The choice in step 2 is a semantic decision, not a file-format task. In particular, atomic connection edges should be excluded or condensed unless the intended locking hierarchy explicitly supports cyclic application graphs.

## Scaling STMBench-shaped data

The legacy `small`, `medium`, and `big` switches mainly change composite and atomic part counts; the assembly branching factor and depth remain fixed. Reaching 1--10 million hierarchy nodes therefore requires a stress configuration or generator rather than merely invoking the default small benchmark. The current CSR generator supplies that stress layer while preserving the important properties for this phase: a rooted sparse hierarchy, shared descendants, deterministic updates, and controllable locality.
