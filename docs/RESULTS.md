# Current Results

## Five node exhaustive check

Command:

```sh
./build/delta_exhaustive 5
```

The checker enumerates 1,024 base DAGs consistent with a fixed topological order and prepares every valid single-edge addition and deletion. It then examines every pair that mutates distinct edges.

| Result | Count |
| --- | ---: |
| Individually valid deltas across all base graphs | 14,167 |
| Delta pairs examined | 93,283 |
| Pair outcomes matching both atomic serial orders | 93,283 |
| Jointly valid pairs | 86,118 |
| Jointly invalid pairs | 7,165 |
| Conservatively disjoint pairs | 4,030 |
| Overlapping pairs | 89,253 |
| Valid pairs where naïve metadata union was wrong | 21,294 |
| Wrong unions for two additions | 5,806 |
| Wrong unions for an addition and deletion | 13,152 |
| Wrong unions for two deletions | 2,336 |
| Wrong unions among conservatively disjoint pairs | 0 |

All jointly invalid pairs in this search were two-addition cases. The smallest example is adding `0 -> 1` and `1 -> 0` to an empty graph. Each addition is valid alone, but the combined graph contains a cycle.

The smallest metadata interaction is also an insertion case. Adding `0 -> 1` and `1 -> 2` creates `0 -> 2`, but that reachability pair occurs in neither private delta. The corresponding deletion example is a diamond: neither branch deletion removes source-to-target reachability alone, while both deletions together do.

## Independent C++ and Python check

Command:

```sh
python3 oracle/crosscheck_cpp.py \
  --binary build/delta_cli \
  --cases 500 \
  --nodes 6 \
  --seed 17
```

Result:

```text
cases=500
nodes=6
seed=17
jointly_invalid=27
valid_cases_where_naive_union_failed=119
cpp_python_mismatches=0
```

## Exploratory repeated work benchmark

Command:

```sh
./build/delta_benchmark \
  --nodes 256 \
  --batch 32 \
  --iterations 20 \
  --edge-probability 0.015 \
  --seed 7
```

One local release build produced:

```text
nodes=256
batch_size=32
iterations=20
initial_edges_min=422
initial_edges_mean=491.850
initial_edges_max=532
final_edges_mean=523.850
serial_full_recomputations_ms=390.097
one_batched_full_recomputation_ms=15.081
speedup=25.866
```

This is only a sanity check of repeated work, not a performance claim. It compares 32 complete reachability recomputations with one complete recomputation. It does not include a parallel scheduler, incremental metadata, or DomLock.

## What the results currently support

- Conservative disjointness is useful as a cheap sufficient case.
- Overlap by itself is not a reason to serialize.
- Independently computed metadata changes cannot generally be unioned.
- A merge has to account for paths that appear or disappear only because several updates are present together.
- Exact recomputation gives a working oracle for the next incremental version.

## Million-scale sparse batch experiment

The scalable engine uses CSR adjacency, applies the complete edge batch in one merge, and performs a full DAG check with Kahn's algorithm. The generated hierarchy is a rooted 4-ary tree with one extra forward parent for every eighth node. The main matrix keeps shared parents inside a top-level branch; four additional rows use cross-branch shared parents. It is a synthetic stress hierarchy, not an STMBench result.

The experiment also constructs the actual dominator tree of the ordered DAG. This is separate from the primary-tree LCA heuristic: for a reachable DAG, the immediate dominator of a node is the deepest common dominator of all its predecessors. Root-selection counts below use this dominator tree, not the primary-parent tree.

The experiment covers the sizes and update ratios requested for the final study. An update count is a percentage of the node count and is split evenly between additions and deletions. Every apply result below is the median of five runs from one local Apple clang 21 release build on arm64 macOS. These timings are preliminary machine-local measurements, not a comparative performance claim.

| Nodes | Edges | Updates | Locality | Full validation + apply (ms) | CSR bytes per state |
| ---: | ---: | ---: | --- | ---: | ---: |
| 1,000,000 | 1,124,992 | 10,000 (1%) | global | 10.095 | 12,499,976 |
| 1,000,000 | 1,124,992 | 100,000 (10%) | global | 12.305 | 12,499,976 |
| 5,000,000 | 5,624,916 | 50,000 (1%) | global | 64.226 | 62,499,672 |
| 5,000,000 | 5,624,916 | 500,000 (10%) | global | 85.172 | 62,499,672 |
| 10,000,000 | 11,249,915 | 100,000 (1%) | global | 146.276 | 124,999,668 |
| 10,000,000 | 11,249,915 | 1,000,000 (10%) | global | 190.445 | 124,999,668 |

The complete 18-row main matrix, plus four cross-branch-sharing checks, is in [SCALE_RESULTS.csv](SCALE_RESULTS.csv). It includes graph and dominator construction, batch generation, peak process RSS, local workloads, and intermediate ratios.

### Root-selection observation

Treating the entire batch as one request selected the root in all 22 configurations. This happened even for the local request workload because one batch contains work from several top-level branches.

Partitioning tells a more precise story. With shared parents restricted to their top-level branch, 31.1%--37.2% of globally paired updates had the root as their true dominator-tree LCA. Constraining each added edge to one top-level subtree reduced this to 0%; the batch separated into four non-root groups. But with cross-branch shared parents, locality in the primary tree was not enough: 49.2%--52.6% of local updates still selected the root, and global updates reached 54.3%--54.9%.

This suggests a concrete next mechanism:

1. partition a batch by a cached **actual dominator**, using primary-tree locality only as a cheap first filter;
2. prepare disjoint top-level groups concurrently;
3. merge their structural and metadata deltas; and
4. fall back to a larger lock only for cross-group updates or failed validation.

The result does **not** yet show parallel speedup. It shows why taking one dominator for the entire batch is too coarse, and it also shows a failure mode for naïve locality: cross-branch sharing can collapse apparently local work back to the root.

### Correctness checks added with the scalable path

- strict edge preconditions and duplicate normalization;
- rejection of a cycle created only by the combined batch;
- exact sparse-vs-transitive-closure comparison on hand-written cases;
- 200 deterministic randomized sparse-vs-exact comparisons;
- hand-checked immediate-dominator and dominator-LCA cases;
- global and local batch-generation checks; and
- the existing independent Python/C++ and exhaustive five-node checks.
