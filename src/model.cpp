#include "delta/model.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>

namespace delta {
namespace {

using Adjacency = std::vector<std::vector<Node>>;

[[nodiscard]] bool endpoint_in_range(const Edge& edge,
                                     std::size_t node_count) {
  return edge.from < node_count && edge.to < node_count;
}

[[nodiscard]] Adjacency adjacency(std::size_t node_count,
                                  const EdgeSet& edges, bool reverse = false) {
  Adjacency result(node_count);
  for (const auto& edge : edges) {
    if (!endpoint_in_range(edge, node_count)) {
      continue;
    }
    if (reverse) {
      result[edge.to].push_back(edge.from);
    } else {
      result[edge.from].push_back(edge.to);
    }
  }
  for (auto& neighbours : result) {
    std::sort(neighbours.begin(), neighbours.end());
  }
  return result;
}

[[nodiscard]] std::set<Node> walk_from(Node start, const Adjacency& graph) {
  std::set<Node> visited{start};
  std::vector<Node> stack{start};
  while (!stack.empty()) {
    const Node current = stack.back();
    stack.pop_back();
    for (const Node next : graph[current]) {
      if (visited.insert(next).second) {
        stack.push_back(next);
      }
    }
  }
  return visited;
}

template <typename T>
[[nodiscard]] std::set<T> set_union_copy(const std::set<T>& left,
                                         const std::set<T>& right) {
  std::set<T> result;
  std::set_union(left.begin(), left.end(), right.begin(), right.end(),
                 std::inserter(result, result.end()));
  return result;
}

template <typename T>
[[nodiscard]] std::set<T> set_difference_copy(const std::set<T>& left,
                                              const std::set<T>& right) {
  std::set<T> result;
  std::set_difference(left.begin(), left.end(), right.begin(), right.end(),
                      std::inserter(result, result.end()));
  return result;
}

template <typename T>
[[nodiscard]] std::set<T> set_intersection_copy(const std::set<T>& left,
                                                const std::set<T>& right) {
  std::set<T> result;
  std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                        std::inserter(result, result.end()));
  return result;
}

[[nodiscard]] bool intersects(const EdgeSet& left, const EdgeSet& right) {
  auto l = left.begin();
  auto r = right.begin();
  while (l != left.end() && r != right.end()) {
    if (*l == *r) {
      return true;
    }
    if (*l < *r) {
      ++l;
    } else {
      ++r;
    }
  }
  return false;
}

[[nodiscard]] bool intersects(const std::set<Node>& left,
                              const std::set<Node>& right) {
  auto l = left.begin();
  auto r = right.begin();
  while (l != left.end() && r != right.end()) {
    if (*l == *r) {
      return true;
    }
    if (*l < *r) {
      ++l;
    } else {
      ++r;
    }
  }
  return false;
}

void append_unique(std::vector<std::string>& reasons, std::string reason) {
  if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
    reasons.push_back(std::move(reason));
  }
}

[[nodiscard]] std::vector<std::string> request_errors(
    const GraphState& state, const UpdateRequest& request) {
  std::vector<std::string> reasons;
  const auto overlap = set_intersection_copy(request.additions, request.deletions);
  if (!overlap.empty()) {
    reasons.push_back("same edge is added and deleted: " +
                      edges_to_string(overlap));
  }

  EdgeSet unknown;
  EdgeSet self_loops;
  for (const auto& edge : set_union_copy(request.additions, request.deletions)) {
    if (!endpoint_in_range(edge, state.node_count)) {
      unknown.insert(edge);
    }
  }
  for (const auto& edge : request.additions) {
    if (edge.from == edge.to) {
      self_loops.insert(edge);
    }
  }
  if (!unknown.empty()) {
    reasons.push_back("edges reference unknown nodes: " +
                      edges_to_string(unknown));
  }
  if (!self_loops.empty()) {
    reasons.push_back("self-loop additions are not allowed: " +
                      edges_to_string(self_loops));
  }

  const auto already_present =
      set_intersection_copy(request.additions, state.edges);
  if (!already_present.empty()) {
    reasons.push_back("edges to add already exist: " +
                      edges_to_string(already_present));
  }
  const auto missing = set_difference_copy(request.deletions, state.edges);
  if (!missing.empty()) {
    reasons.push_back("edges to delete do not exist: " +
                      edges_to_string(missing));
  }
  return reasons;
}

}  // namespace

GraphState GraphState::create(std::size_t node_count, EdgeSet edges,
                              std::uint64_t version) {
  for (const auto& edge : edges) {
    if (!endpoint_in_range(edge, node_count)) {
      throw std::invalid_argument("edge references an unknown node: " +
                                  edge_to_string(edge));
    }
  }
  if (const auto cycle = find_cycle(node_count, edges); cycle.has_value()) {
    std::ostringstream message;
    message << "committed state must be a DAG; cycle:";
    for (const Node node : *cycle) {
      message << ' ' << node;
    }
    throw std::invalid_argument(message.str());
  }
  auto reachability = transitive_closure(node_count, edges);
  return GraphState{node_count, std::move(edges), std::move(reachability),
                    version};
}

EdgeSet PreparedDelta::mutated_edges() const {
  return set_union_copy(additions, deletions);
}

EdgeSet transitive_closure(std::size_t node_count, const EdgeSet& edges) {
  const auto graph = adjacency(node_count, edges);
  EdgeSet result;
  for (Node source = 0; source < node_count; ++source) {
    std::vector<bool> visited(node_count, false);
    std::vector<Node> stack(graph[source].begin(), graph[source].end());
    while (!stack.empty()) {
      const Node current = stack.back();
      stack.pop_back();
      if (visited[current]) {
        continue;
      }
      visited[current] = true;
      result.insert(Edge{source, current});
      for (const Node next : graph[current]) {
        if (!visited[next]) {
          stack.push_back(next);
        }
      }
    }
  }
  return result;
}

std::optional<std::vector<Node>> find_cycle(std::size_t node_count,
                                            const EdgeSet& edges) {
  const auto graph = adjacency(node_count, edges);
  std::vector<int> colour(node_count, 0);
  std::vector<Node> parent(node_count, 0);

  std::function<std::optional<std::vector<Node>>(Node)> visit =
      [&](Node node) -> std::optional<std::vector<Node>> {
    colour[node] = 1;
    for (const Node child : graph[node]) {
      if (colour[child] == 0) {
        parent[child] = node;
        if (auto cycle = visit(child); cycle.has_value()) {
          return cycle;
        }
      } else if (colour[child] == 1) {
        std::vector<Node> path{child};
        Node current = node;
        while (current != child) {
          path.push_back(current);
          current = parent[current];
        }
        path.push_back(child);
        std::reverse(path.begin(), path.end());
        return path;
      }
    }
    colour[node] = 2;
    return std::nullopt;
  };

  for (Node node = 0; node < node_count; ++node) {
    if (colour[node] == 0) {
      if (auto cycle = visit(node); cycle.has_value()) {
        return cycle;
      }
    }
  }
  return std::nullopt;
}

PreparedDelta prepare_delta(const GraphState& state,
                            const UpdateRequest& request) {
  EdgeSet known_additions;
  EdgeSet known_deletions;
  for (const auto& edge : request.additions) {
    if (endpoint_in_range(edge, state.node_count)) {
      known_additions.insert(edge);
    }
  }
  for (const auto& edge : request.deletions) {
    if (endpoint_in_range(edge, state.node_count)) {
      known_deletions.insert(edge);
    }
  }

  auto candidate_edges = set_difference_copy(state.edges, known_deletions);
  candidate_edges = set_union_copy(candidate_edges, known_additions);
  const auto candidate_reachability =
      transitive_closure(state.node_count, candidate_edges);

  const auto old_forward = adjacency(state.node_count, state.edges);
  const auto old_reverse = adjacency(state.node_count, state.edges, true);
  const auto new_forward = adjacency(state.node_count, candidate_edges);
  const auto new_reverse = adjacency(state.node_count, candidate_edges, true);
  std::set<Node> affected_nodes;
  for (const auto& edge : set_union_copy(known_additions, known_deletions)) {
    for (const auto& nodes : {walk_from(edge.from, old_reverse),
                              walk_from(edge.to, old_forward),
                              walk_from(edge.from, new_reverse),
                              walk_from(edge.to, new_forward)}) {
      affected_nodes.insert(nodes.begin(), nodes.end());
    }
  }

  return PreparedDelta{
      request.name,
      state.version,
      request.additions,
      request.deletions,
      request.additions,
      request.deletions,
      candidate_edges,
      candidate_reachability,
      set_difference_copy(candidate_reachability, state.reachability),
      set_difference_copy(state.reachability, candidate_reachability),
      affected_nodes,
      find_cycle(state.node_count, candidate_edges),
  };
}

ValidationResult validate_delta(const GraphState& state,
                                const PreparedDelta& prepared) {
  std::vector<std::string> reasons;
  if (prepared.base_version != state.version) {
    reasons.push_back("stale base version " +
                      std::to_string(prepared.base_version) +
                      "; current version is " + std::to_string(state.version));
  }

  const UpdateRequest request{prepared.name, prepared.additions,
                              prepared.deletions};
  for (auto reason : request_errors(state, request)) {
    append_unique(reasons, std::move(reason));
  }

  const auto changed_absence =
      set_intersection_copy(prepared.expected_absent, state.edges);
  if (!changed_absence.empty()) {
    append_unique(reasons, "edges expected absent are now present: " +
                               edges_to_string(changed_absence));
  }
  const auto changed_presence =
      set_difference_copy(prepared.expected_present, state.edges);
  if (!changed_presence.empty()) {
    append_unique(reasons, "edges expected present are now absent: " +
                               edges_to_string(changed_presence));
  }
  if (prepared.cycle.has_value()) {
    std::ostringstream message;
    message << "update creates a cycle:";
    for (const Node node : *prepared.cycle) {
      message << ' ' << node;
    }
    append_unique(reasons, message.str());
  }

  if (transitive_closure(state.node_count, prepared.candidate_edges) !=
      prepared.candidate_reachability) {
    append_unique(reasons,
                  "prepared reachability metadata does not match candidate edges");
  }
  return ValidationResult{reasons.empty(), std::move(reasons)};
}

GraphState apply_delta(const GraphState& state,
                       const PreparedDelta& prepared) {
  const auto validation = validate_delta(state, prepared);
  if (!validation.valid) {
    throw DeltaConflict(join_reasons(validation.reasons));
  }
  return GraphState{state.node_count, prepared.candidate_edges,
                    prepared.candidate_reachability, state.version + 1};
}

bool deltas_are_disjoint(const PreparedDelta& left,
                         const PreparedDelta& right) {
  return !intersects(left.mutated_edges(), right.mutated_edges()) &&
         !intersects(left.affected_nodes, right.affected_nodes);
}

PreparedDelta merge_deltas(const GraphState& base,
                           const std::vector<PreparedDelta>& deltas,
                           std::string name) {
  if (deltas.empty()) {
    return prepare_delta(base, UpdateRequest{std::move(name), {}, {}});
  }

  std::map<Edge, std::string> edge_owner;
  EdgeSet additions;
  EdgeSet deletions;
  for (const auto& prepared : deltas) {
    if (prepared.base_version != base.version) {
      throw DeltaConflict("delta " + prepared.name +
                          " was prepared on a different base version");
    }
    const auto validation = validate_delta(base, prepared);
    if (!validation.valid) {
      throw DeltaConflict("delta " + prepared.name + " is invalid: " +
                          join_reasons(validation.reasons));
    }
    for (const auto& edge : prepared.mutated_edges()) {
      if (const auto existing = edge_owner.find(edge);
          existing != edge_owner.end()) {
        throw DeltaConflict("edge " + edge_to_string(edge) +
                            " is mutated by both " + existing->second +
                            " and " + prepared.name);
      }
      edge_owner.emplace(edge, prepared.name);
    }
    additions.insert(prepared.additions.begin(), prepared.additions.end());
    deletions.insert(prepared.deletions.begin(), prepared.deletions.end());
  }

  const auto overlap = set_intersection_copy(additions, deletions);
  if (!overlap.empty()) {
    throw DeltaConflict("merged delta adds and deletes the same edges: " +
                        edges_to_string(overlap));
  }
  return prepare_delta(
      base, UpdateRequest{std::move(name), std::move(additions),
                          std::move(deletions)});
}

EdgeSet naive_metadata_union(const GraphState& base,
                             const std::vector<PreparedDelta>& deltas) {
  EdgeSet added;
  EdgeSet removed;
  for (const auto& prepared : deltas) {
    added.insert(prepared.reachability_added.begin(),
                 prepared.reachability_added.end());
    removed.insert(prepared.reachability_removed.begin(),
                   prepared.reachability_removed.end());
  }
  auto result = set_difference_copy(base.reachability, removed);
  return set_union_copy(result, added);
}

SerialOutcome run_atomic_serial(const GraphState& base,
                                const std::vector<UpdateRequest>& requests) {
  GraphState scratch = base;
  for (const auto& request : requests) {
    const auto prepared = prepare_delta(scratch, request);
    const auto validation = validate_delta(scratch, prepared);
    if (!validation.valid) {
      return SerialOutcome{false, base, request.name,
                           join_reasons(validation.reasons)};
    }
    scratch = apply_delta(scratch, prepared);
  }
  scratch.version = base.version + 1;
  return SerialOutcome{true, std::move(scratch), std::nullopt, std::nullopt};
}

SerialComparison compare_two_serial_orders(const GraphState& base,
                                           const UpdateRequest& left,
                                           const UpdateRequest& right) {
  const auto left_delta = prepare_delta(base, left);
  const auto right_delta = prepare_delta(base, right);
  ValidationResult merged_validation;
  std::optional<GraphState> merged_state;
  try {
    const auto merged = merge_deltas(base, {left_delta, right_delta});
    merged_validation = validate_delta(base, merged);
    if (merged_validation.valid) {
      merged_state = apply_delta(base, merged);
    }
  } catch (const DeltaConflict& conflict) {
    merged_validation = ValidationResult{false, {conflict.what()}};
  }

  const auto left_then_right = run_atomic_serial(base, {left, right});
  const auto right_then_left = run_atomic_serial(base, {right, left});
  bool agrees = false;
  if (merged_state.has_value()) {
    agrees = left_then_right.accepted && right_then_left.accepted &&
             left_then_right.state.edges == merged_state->edges &&
             right_then_left.state.edges == merged_state->edges &&
             left_then_right.state.reachability == merged_state->reachability &&
             right_then_left.state.reachability == merged_state->reachability;
  } else {
    agrees = !left_then_right.accepted && !right_then_left.accepted;
  }
  return SerialComparison{std::move(merged_validation), std::move(merged_state),
                          left_then_right, right_then_left, agrees};
}

std::string edge_to_string(const Edge& edge) {
  return std::to_string(edge.from) + "->" + std::to_string(edge.to);
}

std::string edges_to_string(const EdgeSet& edges) {
  std::ostringstream output;
  bool first = true;
  for (const auto& edge : edges) {
    if (!first) {
      output << ',';
    }
    output << edge_to_string(edge);
    first = false;
  }
  return output.str();
}

std::string join_reasons(const std::vector<std::string>& reasons) {
  std::ostringstream output;
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    if (index > 0) {
      output << "; ";
    }
    output << reasons[index];
  }
  return output.str();
}

}  // namespace delta
