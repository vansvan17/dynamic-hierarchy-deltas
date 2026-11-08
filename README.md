# Dynamic Hierarchy Deltas

Batched edge additions and deletions on a rooted DAG, and k-bounded interval-set metadata for hierarchical locking (the "delayed interval merging" idea in `../paper/main.tex`). Exploratory research code, not a novelty claim or a production implementation.

Two files:

- `dh.hpp`: the library
  - an **exact model** for small graphs: reachability as one bitmask row per node, n ≤ 32;
  - a **sparse engine**: CSR graphs, one-merge batch apply, acyclicity and rootedness checks, a dominator tree for node-ordered DAGs, and a synthetic hierarchy and batch generator;
  - **interval sets**: one immutable post-order label per node, at most k disjoint ranges per node, closest-gap merging, full build, and a one-pass batch repair.
- `main.cpp`: one binary, `dh`, with four modes:
  - `test`: unit tests;
  - `exhaustive [N]`: every pair of single-edge updates on every DAG with N ≤ 5 nodes;
  - `cli`: the interface used by the Python oracle;
  - `scale`: the million-node experiment.

`oracle/` is an independent Python model. It does not call the C++ code.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`ctest` runs three suites:
- the unit tests;
- the exhaustive five-node check;
- 150 C++/Python cross-check cases.

## What each check establishes

- **Exhaustive (`./build/dh exhaustive 5`).** It enumerates all 1,024 DAGs consistent with one five-node topological order. It prepares every valid single-edge addition and deletion (14,167) and examines every pair (93,283).
  - Merged outcomes match both atomic serial orders in every case.
  - Naive union of the private reachability deltas is wrong in 21,294 of 86,118 jointly valid pairs, and in none of the 4,030 conservatively disjoint pairs.
  - For k = 1, 2 and 3, it also repairs interval metadata after each of the 100,285 valid batches (single edges and pairs). It checks the invariant and counts false negatives (must be 0) and false overlaps, against both exact reachability and a full rebuild.
- **Cross-check (`python3 oracle/crosscheck_cpp.py --binary build/dh --cases 500 --nodes 6 --seed 17`).** It compares edge sets, reachability, validity, the naive-union result and serial agreement with the Python model.
- **Unit tests.** They cover:
  - the emergent-path, diamond and joint-cycle cases;
  - the precondition, cycle and rootedness rejections;
  - a hand-checked dominator tree;
  - 200 random batches comparing the sparse engine with the exact model;
  - generated batches staying rooted;
  - the online versus offline merge example (12 vs 11 falsely covered labels);
  - 300 random repairs keeping the invariant.

## Scale matrix

```sh
./scripts/run_scale_matrix.sh ./build/dh 17 4 > docs/SCALE_RESULTS.csv
./build/dh scale 10000000 0.10 global 17 5 local-sharing 4   # one configuration
c++ -std=c++20 -O3 -I. scripts/k_split.cpp -o build/k_split && ./build/k_split   # repair work by update kind
c++ -std=c++20 -O3 -I. scripts/precision.cpp -o build/precision && ./build/precision 5 > docs/PRECISION.csv   # false conflicts vs exact reachability, delayed vs eager
```

**Graph:** a 4-ary tree plus an extra earlier parent for every eighth node, 1–10 million nodes. Extra parents either stay within the node's top-level branch (`local-sharing`) or not (`cross-sharing`).

**Batch:** 1–10% of the node count, split evenly between additions and deletions.
- Additions are forward edges (`global` or within one top-level branch for `local`), so they can never create a cycle.
- A deletion removes a node's primary parent edge only when the node has a second parent, at most once per node, so every batch keeps the hierarchy rooted.

**Reported per configuration:**
- the median of five timed applies (merge, Kahn's acyclicity check, rootedness check);
- dominator statistics on the base graph and on the graph plus the additions;
- interval-set metadata with budget k: the time to build labels and sets, one batch repair (nodes visited and changed) and a full rebuild on the updated graph.

## Current limits

- fixed vertex set, edge updates only, rooted DAGs only;
- sequential: no concurrent groups, lock pool or publication protocol yet;
- additions in the generator are forward-only, and dominators assume node order is topological;
- one machine.

The previous multi-file implementation is kept in `../dynamic-hierarchy-deltas.before-simplify-2026-11-05/` for reference.
