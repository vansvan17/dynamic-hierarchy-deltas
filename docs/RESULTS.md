# Current Results (5 Nov 2026)

All numbers come from the single-binary rewrite (`dh.hpp`, `main.cpp`). Every timing was taken on one Apple-silicon macOS machine with Apple clang in a Release build. These are machine-local measurements, not comparative performance claims.

## Rewrite check

The rewrite reproduces the old implementation's outputs exactly:
- every counter of `delta_exhaustive 5` (pairs, validity, serial agreement, naive-union failures by kind and by disjointness);
- the full output of the 500-case Python cross-check.

The old code is in `../../dynamic-hierarchy-deltas.before-simplify-2026-11-05/`.

## Exhaustive five-node check

`./build/dh exhaustive 5` enumerates the 1,024 DAGs consistent with one fixed five-node topological order, every valid single-edge addition and deletion, and every pair of them.

| Result | Count |
| --- | ---: |
| Individually valid deltas | 14,167 |
| Delta pairs examined | 93,283 |
| Pair outcomes matching both atomic serial orders | 93,283 |
| Jointly valid / jointly invalid pairs | 86,118 / 7,165 |
| Conservatively disjoint / overlapping pairs | 4,030 / 89,253 |
| Valid pairs where naive union of reachability deltas was wrong | 21,294 |
| ... two additions / addition and deletion / two deletions | 5,806 / 13,152 / 2,336 |
| ... among conservatively disjoint pairs | 0 |

All jointly invalid pairs are two-addition cycles (the smallest is `0 -> 1` with `1 -> 0`). The smallest metadata interaction is the chain `0 -> 1` plus `1 -> 2`, which creates `0 -> 2` in neither private delta. The deletion counterpart is the diamond.

### Interval metadata on the same cases

For each of the 100,285 valid batches (14,167 single edges and 86,118 jointly valid pairs), and for k = 1, 2, 3, the check does four things:
1. labels the base graph by DFS post-order;
2. builds the sets;
3. applies the batch and repairs the sets in one pass;
4. compares the result against exact reachability and against a full rebuild on the updated graph.

| k | False negatives | Invariant failures | False overlaps, repaired | False overlaps, rebuilt | Batches where repair was less precise |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0 | 0 | 38,145 | 38,145 | 0 |
| 2 | 0 | 0 | 1,060 | 1,050 | 10 |
| 3 | 0 | 0 | 0 | 0 | 0 |

The soundness result is what this check is for: no shared descendant was ever missed, and local subsumption held after every repair. Five-node graphs are far too small to measure precision; with every node labelled, three ranges almost always represent a five-node set exactly. The 10 batches at k = 2 where repair was less precise than a rebuild are the online-versus-offline effect: the repair keeps an old merge that a fresh compression would not make. The unit test with points 0, 5, 20, 9, 13, 17 shows the same effect (12 versus 11 falsely covered labels).

## Independent C++ and Python check

`python3 oracle/crosscheck_cpp.py --binary build/dh --cases 500 --nodes 6 --seed 17` gives:

```text
cases=500
nodes=6
seed=17
jointly_invalid=27
valid_cases_where_naive_union_failed=119
cpp_python_mismatches=0
```

## Million-scale matrix

`./scripts/run_scale_matrix.sh ./build/dh 17 4` produces `SCALE_RESULTS.csv`, with k = 4. The hierarchy is a 4-ary tree with one extra, earlier parent for every eighth node. The extra parent stays inside the node's top-level branch (`local-sharing`) or not (`cross-sharing`).

**Generator change (5 Nov 2026).** Additions are forward edges, uniform (`global`) or within one top-level branch (`local`). The earlier generator deleted primary-parent edges of uniformly chosen nodes, and about 7 in 8 of those nodes had no other parent, so most "valid" batches cut subtrees off from the root. Deletions now remove the primary parent edge only of a node that has a second parent, at most once per node, and apply rejects any batch that leaves a node unreachable. **Every number below replaces the earlier scale results.**

### Structural apply

Apply means: merge the batch into one CSR, run Kahn's acyclicity check, and check rootedness by DFS from the root. Each value is the median of five runs.

| Nodes | Edges | Updates | Global (ms) | Local (ms) | CSR bytes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000,000 | 1,124,992 | 10,000 (1%) | 16.7 | 27.9 | 12,499,976 |
| 1,000,000 | 1,124,992 | 100,000 (10%) | 22.6 | 18.2 | 12,499,976 |
| 5,000,000 | 5,624,916 | 50,000 (1%) | 108.8 | 108.2 | 62,499,672 |
| 5,000,000 | 5,624,916 | 500,000 (10%) | 125.1 | 116.4 | 62,499,672 |
| 10,000,000 | 11,249,915 | 100,000 (1%) | 215.0 | 211.5 | 124,999,668 |
| 10,000,000 | 11,249,915 | 1,000,000 (10%) | 336.9 | 267.9 | 124,999,668 |

These are higher than the old 146–190 ms at 10M nodes. The old figures had no rootedness check and used different (unrooted) batches. Run-to-run spread on this machine is large (the 1M local 1% row is above its 10% row), so treat differences under about 1.5x as noise.

### Dominator-based partitioning

| Sharing | Locality | Root share, all mutations (base dominators) | Root share, base plus additions | Root share, additions only |
| --- | --- | ---: | ---: | ---: |
| local | global | 0.31–0.37 | 0.32–0.42 | 0.62–0.74 |
| local | local | 0 | 0 | 0 |
| cross | global | 0.96–0.97 | 0.96–0.97 | 0.96–0.98 |
| cross | local | 0.91–0.95 | 0.91–0.95 | 0.88–0.93 |

Five observations:
- **One dominator for the whole batch is the root in all 22 configurations.**
- **Local workload with local sharing: 0% by construction.** Both endpoints of every update lie in one top-level branch, and that branch's root dominates it.
- **Global workload with local sharing:** 62–74% of additions have the root as their dominator-tree LCA. The all-mutation figure is lower only because deletions of a primary parent edge almost never select the root.
- **Cross sharing makes nearly every update a root update,** additions included (88–98%), even when the update stays within one primary-tree branch. The earlier report of 49–55% averaged roughly 96% for additions with a low rate for the old deletions. Primary-tree locality is not a usable partition key once sharing crosses branches.
- **Base-graph dominators understate root selection.** With the dominators of the base graph plus the additions, which dominate in both the old and the new graph, the global root share rises from 0.37 to 0.42 at 5M nodes and 10% updates. The partition analysis has to use the union graph.

### Interval repair versus full rebuild (k = 4)

Columns:
- `repair_ms`: one pass over the batch's ancestors in G′.
- `repair_prep_ms`: topological order and parent index of G′; a real system would maintain these.
- `rebuild_ms`: recompute every set from the labels on G′, with labels kept fixed, so this is cheaper than a true DomLock renumbering.
- Repair and rebuild are single runs.

| Nodes | Updates | Repair (ms) | Prep (ms) | Rebuild (ms) | Nodes changed |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1M | 1% | 16–18 | ~12 | 101–110 | ~30,000 |
| 1M | 10% | 59–65 | ~12 | 101–110 | ~158,000 |
| 10M | 1% | 167–173 | ~180 | 1,028–1,089 | ~300,000 |
| 10M | 10% | 670–677 | ~205 | 1,203–1,248 | ~1,580,000 |

At 1% updates, repair plus prep is about 3x cheaper than a rebuild at 10M nodes. At 10% it is about 1.4x cheaper. So E5 in `../paper/REVIEW_2026-11-02.md` (rebuild beats repair at ≥ 1%) is **not** supported on this graph at k = 4, but the margin at 10% is small and comes from single runs. Each node holds 1.26–1.35 ranges on average.

### Budget sweep

`K_SWEEP.csv` covers 10M nodes, global workload, local sharing, mixed batches (half additions, half deletions), single runs. The k = 4 rows are the matching rows of `SCALE_RESULTS.csv`.

| k | Updates | Nodes changed by repair | Repair (ms) | Mean ranges per node |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1% | 231,153 | 119 | 1.00 |
| 2 | 1% | 278,086 | 222 | 1.15 |
| 4 | 1% | 304,912 | 167 | 1.26 |
| 8 | 1% | 318,635 | 231 | 1.35 |
| 1 | 10% | 1,322,283 | 665 | 1.00 |
| 2 | 10% | 1,537,070 | 641 | 1.20 |
| 4 | 10% | 1,585,402 | 670 | 1.35 |
| 8 | 10% | 1,591,577 | 742 | 1.47 |

Per mixed batch, 38% more nodes change at k = 8 than at k = 1 (1% updates). Splitting by update kind (`scripts/k_split.cpp`: same graph, 100,000 updates of one kind) shows where that comes from:

| k | Nodes changed, additions only | Nodes changed, deletions only |
| ---: | ---: | ---: |
| 1 | 345,458 | 69,174 |
| 2 | 360,919 | 157,306 |
| 4 | 370,067 | 214,166 |
| 8 | 374,332 | 248,937 |

- **Insertion propagation grows only 8% from k = 1 to k = 8.** The review predicted this direction (F3: fragment sets propagate what a hull absorbs), but the effect is small on this hierarchy.
- **Most of the growth is deletion repair (3.6x).** With more ranges there is more precision to win back, so more ancestors shrink. That is extra work, but it buys precision.
- **Neither result supports "delayed merging reduces propagation".**

## False conflicts against exact reachability (7 Nov 2026)

`scripts/precision.cpp` writes `PRECISION.csv`. It keeps a descendant bitset per node, so it runs on 4,096 and 16,384 nodes only, using the same generator as the scale runs.
- **Conflict:** two nodes truly conflict when they share a descendant (each node counts as its own descendant).
- **False conflict:** the intervals overlap but the nodes share no descendant. The rate is false conflicts divided by truly non-conflicting pairs.
- **Pairs:** 200,000 per run, either uniform or within one top-level branch. More than 99% of sampled pairs truly do not conflict.
- **Runs:** five batch seeds for each configuration, batches of 1% or 10% updates (mixed, additions only or deletions only), k = 1, 2, 4 and 8.

Each batch is scored in four states on the same pairs:
- **base:** sets built on G, before the batch;
- **repair:** the base sets after one-pass repair, keeping the labels of G;
- **rebuild:** sets rebuilt on G′ from the labels of G;
- **relabel:** fresh post-order labels on G′, with sets built from them (closest to renumbering from scratch).

**No false negatives in any of the 960 runs (about 768 million pair checks).**

At 16,384 nodes, with mixed batches and uniform pairs (mean of five seeds):

| Sharing | Updates | k | Base | Repair | Rebuild | Relabel | Nodes changed by repair |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| local | 1% | 1 | 3.20% | 4.57% | 4.57% | 4.29% | 383 |
| local | 1% | 2 | 0.95% | 1.25% | 1.25% | 1.21% | 455 |
| local | 1% | 4 | 0.33% | 0.43% | 0.43% | 0.41% | 489 |
| local | 1% | 8 | 0.14% | 0.17% | 0.17% | 0.16% | 505 |
| local | 10% | 1 | 3.20% | 11.62% | 11.62% | 9.10% | 2,263 |
| local | 10% | 2 | 0.95% | 3.46% | 3.46% | 2.68% | 2,547 |
| local | 10% | 4 | 0.33% | 1.25% | 1.25% | 0.93% | 2,603 |
| local | 10% | 8 | 0.14% | 0.52% | 0.52% | 0.39% | 2,609 |
| cross | 1% | 1 | 11.38% | 12.22% | 12.22% | 11.45% | 303 |
| cross | 1% | 2 | 3.53% | 3.79% | 3.78% | 3.55% | 395 |
| cross | 1% | 4 | 1.29% | 1.38% | 1.38% | 1.30% | 459 |
| cross | 1% | 8 | 0.58% | 0.62% | 0.62% | 0.58% | 491 |
| cross | 10% | 1 | 11.38% | 16.52% | 16.52% | 12.27% | 2,091 |
| cross | 10% | 2 | 3.53% | 5.44% | 5.43% | 3.86% | 2,501 |
| cross | 10% | 4 | 1.29% | 2.03% | 2.03% | 1.39% | 2,594 |
| cross | 10% | 8 | 0.58% | 0.89% | 0.89% | 0.60% | 2,603 |

What this shows, on these graphs only:
- **The budget is what matters.** Each doubling of k cuts false conflicts by roughly 2–3.5x. From k = 1 to k = 8, repaired false conflicts fall 19–27x, while the nodes changed by repair rise 15–62%.
- **More propagation went with fewer false conflicts, not more.** Across budgets the two move in opposite directions. Within one budget, the batches that propagate more are the larger ones, which also leave more false conflicts, so the two are confounded there. A within-batch comparison of eager versus delayed merging would need an eager-widening variant that does not exist yet.
- **One-pass repair loses almost nothing against a rebuild with the same labels.** The largest difference in any configuration is 0.05 percentage points.
- **The remaining loss is label drift.** Keeping the labels of G costs precision after additions (16,384 nodes, 10% additions, k = 4, cross sharing: 3.5% repaired versus 2.7% relabelled). Deletions lower the rate, and fresh labels lower it further. How often to relabel is a real design question.
- **Rates barely change between 4,096 and 16,384 nodes** (see the CSV). That is a hint, not evidence, about 10 million nodes.

Caveats:
- These are pairwise overlaps. Lock-pool conflicts, real access patterns (STMBench7) and DomLock's leaf-only labels are not modelled.
- Uniform pairs under a 4-ary tree are rarely related, which is why more than 99% of pairs are non-conflicting.

## Not measured yet

- Precision at million scale (needs sampled ground truth, for example per-pair BFS), and false conflicts under a lock-pool or STMBench7 access pattern.
- An eager-widening baseline inside one batch, to test directly whether delaying merges reduces false conflicts.
- The MID baselines, a DomLock adapter, lock-pool simulation, concurrent groups, and STMBench7.
- Timing spread on repair and rebuild, and a second machine.
