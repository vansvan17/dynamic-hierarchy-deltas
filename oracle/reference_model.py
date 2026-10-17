"""Small Python oracle used to cross-check the C++ implementation."""

from __future__ import annotations

from dataclasses import dataclass

Edge = tuple[int, int]


@dataclass(frozen=True)
class Request:
    additions: frozenset[Edge] = frozenset()
    deletions: frozenset[Edge] = frozenset()

    @property
    def mutation(self) -> Edge:
        return next(iter(self.additions or self.deletions))


def closure(node_count: int, edges: frozenset[Edge]) -> frozenset[Edge]:
    adjacency = [[] for _ in range(node_count)]
    for source, destination in edges:
        adjacency[source].append(destination)

    result: set[Edge] = set()
    for source in range(node_count):
        stack = list(adjacency[source])
        visited: set[int] = set()
        while stack:
            current = stack.pop()
            if current in visited:
                continue
            visited.add(current)
            result.add((source, current))
            stack.extend(adjacency[current])
    return frozenset(result)


def cyclic(node_count: int, edges: frozenset[Edge]) -> bool:
    return any((node, node) in closure(node_count, edges) for node in range(node_count))


def apply_request(
    node_count: int, edges: frozenset[Edge], request: Request
) -> frozenset[Edge] | None:
    if request.additions & edges:
        return None
    if not request.deletions <= edges:
        return None
    candidate = (edges - request.deletions) | request.additions
    if cyclic(node_count, candidate):
        return None
    return candidate


def atomic_serial(
    node_count: int, base: frozenset[Edge], requests: tuple[Request, Request]
) -> tuple[frozenset[Edge], frozenset[Edge]] | None:
    scratch = base
    for request in requests:
        updated = apply_request(node_count, scratch, request)
        if updated is None:
            return None
        scratch = updated
    return scratch, closure(node_count, scratch)


def valid_single_edge_requests(
    node_count: int, base: frozenset[Edge]
) -> list[Request]:
    result: list[Request] = []
    for source in range(node_count):
        for destination in range(node_count):
            if source == destination:
                continue
            edge = (source, destination)
            if edge in base:
                request = Request(deletions=frozenset({edge}))
            else:
                request = Request(additions=frozenset({edge}))
            if apply_request(node_count, base, request) is not None:
                result.append(request)
    return result


def merged_result(
    node_count: int, base: frozenset[Edge], left: Request, right: Request
) -> tuple[bool, frozenset[Edge], frozenset[Edge]]:
    additions = left.additions | right.additions
    deletions = left.deletions | right.deletions
    candidate = (base - deletions) | additions
    valid = not cyclic(node_count, candidate)
    return valid, candidate, closure(node_count, candidate)


def naive_metadata_matches(
    node_count: int, base: frozenset[Edge], left: Request, right: Request
) -> bool:
    base_closure = closure(node_count, base)
    individual = []
    for request in (left, right):
        candidate = apply_request(node_count, base, request)
        assert candidate is not None
        candidate_closure = closure(node_count, candidate)
        individual.append(
            (candidate_closure - base_closure, base_closure - candidate_closure)
        )
    added = individual[0][0] | individual[1][0]
    removed = individual[0][1] | individual[1][1]
    naive = (base_closure - removed) | added
    return naive == merged_result(node_count, base, left, right)[2]
