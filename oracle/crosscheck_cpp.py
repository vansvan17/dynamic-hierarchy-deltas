"""Cross-check random C++ delta outcomes against the independent Python oracle."""

from __future__ import annotations

import argparse
import random
import subprocess
from pathlib import Path

from reference_model import (
    Request,
    atomic_serial,
    merged_result,
    naive_metadata_matches,
    valid_single_edge_requests,
)


def edge_text(edges: frozenset[tuple[int, int]]) -> str:
    if not edges:
        return "-"
    return ",".join(f"{source}>{destination}" for source, destination in sorted(edges))


def parse_edges(text: str) -> frozenset[tuple[int, int]]:
    if not text:
        return frozenset()
    return frozenset(
        tuple(int(part) for part in token.split("->", maxsplit=1))
        for token in text.split(",")
    )


def run_cpp(binary: Path, node_count: int, base, left: Request, right: Request):
    command = [
        str(binary),
        str(node_count),
        edge_text(base),
        edge_text(left.additions),
        edge_text(left.deletions),
        edge_text(right.additions),
        edge_text(right.deletions),
    ]
    completed = subprocess.run(command, check=True, text=True, capture_output=True)
    result = {}
    for line in completed.stdout.splitlines():
        key, value = line.split("=", maxsplit=1)
        result[key] = value
    return result


def random_base(node_count: int, probability: float, rng: random.Random):
    return frozenset(
        (source, destination)
        for source in range(node_count)
        for destination in range(source + 1, node_count)
        if rng.random() < probability
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/delta_cli"))
    parser.add_argument("--cases", type=int, default=300)
    parser.add_argument("--nodes", type=int, default=6)
    parser.add_argument("--seed", type=int, default=17)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    checked = 0
    naive_failures = 0
    jointly_invalid = 0

    while checked < args.cases:
        base = random_base(args.nodes, 0.25, rng)
        requests = valid_single_edge_requests(args.nodes, base)
        if len(requests) < 2:
            continue
        left, right = rng.sample(requests, 2)
        if left.mutation == right.mutation:
            continue

        cpp = run_cpp(args.binary, args.nodes, base, left, right)
        if cpp.get("prepared") != "1":
            raise AssertionError(f"C++ rejected an individually valid request: {cpp}")

        valid, expected_edges, expected_reachability = merged_result(
            args.nodes, base, left, right
        )
        if int(cpp["valid"]) != int(valid):
            raise AssertionError((base, left, right, cpp, valid))
        if parse_edges(cpp["edges"]) != expected_edges:
            raise AssertionError("candidate edge mismatch")
        if parse_edges(cpp["reachability"]) != expected_reachability:
            raise AssertionError("reachability mismatch")

        expected_naive_match = naive_metadata_matches(args.nodes, base, left, right)
        if int(cpp["naive_match"]) != int(expected_naive_match):
            raise AssertionError("naive metadata classification mismatch")
        naive_failures += int(not expected_naive_match and valid)

        serial_left = atomic_serial(args.nodes, base, (left, right))
        serial_right = atomic_serial(args.nodes, base, (right, left))
        expected_serial_match = (
            serial_left is not None
            and serial_right is not None
            and serial_left == serial_right
            and valid
            and serial_left == (expected_edges, expected_reachability)
        ) or (serial_left is None and serial_right is None and not valid)
        if int(cpp["serial_match"]) != int(expected_serial_match):
            raise AssertionError("serial-oracle classification mismatch")

        jointly_invalid += int(not valid)
        checked += 1

    print(f"cases={checked}")
    print(f"nodes={args.nodes}")
    print(f"seed={args.seed}")
    print(f"jointly_invalid={jointly_invalid}")
    print(f"valid_cases_where_naive_union_failed={naive_failures}")
    print("cpp_python_mismatches=0")


if __name__ == "__main__":
    main()
