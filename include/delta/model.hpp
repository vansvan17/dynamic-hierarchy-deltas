#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace delta {

using Node = std::uint32_t;

struct Edge {
  Node from{};
  Node to{};

  auto operator<=>(const Edge&) const = default;
};

using EdgeSet = std::set<Edge>;

struct GraphState {
  std::size_t node_count{};
  EdgeSet edges;
  EdgeSet reachability;
  std::uint64_t version{};

  static GraphState create(std::size_t node_count, EdgeSet edges = {},
                           std::uint64_t version = 0);
};

struct UpdateRequest {
  std::string name;
  EdgeSet additions;
  EdgeSet deletions;
};

struct PreparedDelta {
  std::string name;
  std::uint64_t base_version{};
  EdgeSet additions;
  EdgeSet deletions;
  EdgeSet expected_absent;
  EdgeSet expected_present;
  EdgeSet candidate_edges;
  EdgeSet candidate_reachability;
  EdgeSet reachability_added;
  EdgeSet reachability_removed;
  std::set<Node> affected_nodes;
  std::optional<std::vector<Node>> cycle;

  [[nodiscard]] EdgeSet mutated_edges() const;
};

struct ValidationResult {
  bool valid{};
  std::vector<std::string> reasons;
};

struct SerialOutcome {
  bool accepted{};
  GraphState state;
  std::optional<std::string> rejected_request;
  std::optional<std::string> reason;
};

struct SerialComparison {
  ValidationResult merged_validation;
  std::optional<GraphState> merged_state;
  SerialOutcome left_then_right;
  SerialOutcome right_then_left;
  bool agrees_with_both_orders{};
};

class DeltaConflict : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] EdgeSet transitive_closure(std::size_t node_count,
                                         const EdgeSet& edges);
[[nodiscard]] std::optional<std::vector<Node>> find_cycle(
    std::size_t node_count, const EdgeSet& edges);
[[nodiscard]] PreparedDelta prepare_delta(const GraphState& state,
                                          const UpdateRequest& request);
[[nodiscard]] ValidationResult validate_delta(const GraphState& state,
                                              const PreparedDelta& prepared);
[[nodiscard]] GraphState apply_delta(const GraphState& state,
                                     const PreparedDelta& prepared);
[[nodiscard]] bool deltas_are_disjoint(const PreparedDelta& left,
                                       const PreparedDelta& right);
[[nodiscard]] PreparedDelta merge_deltas(
    const GraphState& base, const std::vector<PreparedDelta>& deltas,
    std::string name = "merged");
[[nodiscard]] EdgeSet naive_metadata_union(
    const GraphState& base, const std::vector<PreparedDelta>& deltas);
[[nodiscard]] SerialOutcome run_atomic_serial(
    const GraphState& base, const std::vector<UpdateRequest>& requests);
[[nodiscard]] SerialComparison compare_two_serial_orders(
    const GraphState& base, const UpdateRequest& left,
    const UpdateRequest& right);

[[nodiscard]] std::string edge_to_string(const Edge& edge);
[[nodiscard]] std::string edges_to_string(const EdgeSet& edges);
[[nodiscard]] std::string join_reasons(const std::vector<std::string>& reasons);

}  // namespace delta
