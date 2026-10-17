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
serial_full_recomputations_ms=400.085
one_batched_full_recomputation_ms=14.952
speedup=26.759
```

This is only a sanity check of repeated work, not a performance claim. It compares 32 complete reachability recomputations with one complete recomputation. It does not include a parallel scheduler, incremental metadata, or DomLock.

## What the results currently support

- Conservative disjointness is useful as a cheap sufficient case.
- Overlap by itself is not a reason to serialize.
- Independently computed metadata changes cannot generally be unioned.
- A merge has to account for paths that appear or disappear only because several updates are present together.
- Exact recomputation gives a working oracle for the next incremental version.
