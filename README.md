# Dynamic Hierarchy Deltas

This is an early correctness model for batched additions and deletions in a dynamic directed acyclic graph. I am keeping it independent of DomLock for now so that the update semantics can be tested before adding hierarchical lock behavior.

The repository has two implementations:

- a C++20 model that will be extended toward the actual implementation; and
- a small Python oracle used to check the C++ results independently.

This is exploratory work. It is not a novelty claim or a production implementation.

## Current model

A committed state contains a fixed vertex set, a DAG edge set, exact transitive closure, and a version number.

Each update has four explicit stages:

1. `prepare_delta` creates a private candidate state and records reachability additions, removals, and a conservative dependency region.
2. `validate_delta` checks the base version, edge preconditions, self-loops, cycles, and metadata consistency.
3. `merge_deltas` combines deltas prepared on the same version and recomputes exact metadata on the combined graph.
4. `apply_delta` publishes a validated delta as the next version.

The complete transitive-closure recomputation is deliberately slow. It gives a reference answer for testing a later affected-region algorithm.

## Build and test

Requirements:

- a C++20 compiler;
- CMake 3.20 or newer; and
- Python 3 for the independent cross-check.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`ctest` runs the C++ unit tests and 150 deterministic C++/Python cross-check cases.

## Run the examples

```sh
./build/delta_demo
```

The examples cover disjoint additions, overlapping additions, a jointly created cycle, deletion of the last two reachability witnesses, and an emergent path created by two insertions.

## Run the exhaustive checker

```sh
./build/delta_exhaustive 5
```

This enumerates every base DAG consistent with one fixed five-node topological order and examines every pair of distinct, individually valid single-edge deltas.

The current five-node run checks 93,283 pair outcomes. The exact merged outcome agrees with the atomic serial oracle in every case. A naïve union of the private metadata changes is wrong in 21,294 jointly valid cases. Details are in [docs/RESULTS.md](docs/RESULTS.md).

## Run the independent cross-check

```sh
python3 oracle/crosscheck_cpp.py \
  --binary build/delta_cli \
  --cases 500 \
  --nodes 6 \
  --seed 17
```

The Python code does not call into the C++ library. It computes the expected edge set, reachability, validity, naïve-union result, and two atomic serial orders separately, then compares them with the C++ command-line output.

## Run the exploratory benchmark

```sh
./build/delta_benchmark \
  --nodes 256 \
  --batch 32 \
  --iterations 20 \
  --edge-probability 0.015 \
  --seed 7
```

The benchmark compares full reachability recomputation after every insertion with one full recomputation after applying the complete batch. It measures the amount of repeated work batching can avoid; it does not yet measure concurrent delta preparation or DomLock integration.

## Repository layout

```text
include/delta/model.hpp   public C++ model API
src/model.cpp             C++ implementation
apps/demo.cpp             hand-checked examples
apps/exhaustive.cpp       exhaustive small-DAG checker
apps/benchmark.cpp        repeated-work benchmark
apps/delta_cli.cpp        interface used by the Python cross-check
oracle/                   independent Python reference model
tests/                    C++ unit tests
docs/RESULTS.md           current observations and counts
```

## Current limits

- fixed vertex set;
- strict add and delete preconditions;
- atomic publication of the whole batch;
- exact transitive closure rather than DomLock or MID intervals;
- no active-lock semantics yet; and
- no parallel execution yet.

The next step is to update only the affected region after a merge and compare each result with the exact oracle. DomLock integration comes after the dynamic update behavior is clear.
