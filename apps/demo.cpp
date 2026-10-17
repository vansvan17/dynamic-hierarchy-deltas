#include "delta/model.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using delta::Edge;
using delta::EdgeSet;
using delta::GraphState;
using delta::PreparedDelta;
using delta::UpdateRequest;

void heading(const std::string& text) {
  std::cout << "\n" << text << "\n" << std::string(text.size(), '-') << "\n";
}

void disjoint_additions() {
  heading("1. Disjoint additions");
  const auto base = GraphState::create(6);
  const auto left = delta::prepare_delta(
      base, UpdateRequest{"left", EdgeSet{{0, 1}}, {}});
  const auto right = delta::prepare_delta(
      base, UpdateRequest{"right", EdgeSet{{3, 4}}, {}});
  const auto merged = delta::merge_deltas(base, {left, right});
  std::cout << "disjoint: " << std::boolalpha
            << delta::deltas_are_disjoint(left, right) << "\n";
  std::cout << "merged edges: " << delta::edges_to_string(merged.candidate_edges)
            << "\n";
}

void overlapping_additions() {
  heading("2. Overlapping but commuting additions");
  const auto base = GraphState::create(
      6, EdgeSet{{0, 1}, {0, 2}, {1, 3}, {2, 4}});
  const UpdateRequest left_request{"3-to-5", EdgeSet{{3, 5}}, {}};
  const UpdateRequest right_request{"4-to-5", EdgeSet{{4, 5}}, {}};
  const auto left = delta::prepare_delta(base, left_request);
  const auto right = delta::prepare_delta(base, right_request);
  const auto merged = delta::merge_deltas(base, {left, right});
  const auto comparison =
      delta::compare_two_serial_orders(base, left_request, right_request);
  std::cout << "disjoint: " << delta::deltas_are_disjoint(left, right) << "\n";
  std::cout << "combined reachability additions: "
            << delta::edges_to_string(merged.reachability_added) << "\n";
  std::cout << "serial oracle agrees: " << comparison.agrees_with_both_orders
            << "\n";
}

void joint_cycle() {
  heading("3. Individually valid additions that jointly create a cycle");
  const auto base = GraphState::create(2);
  const UpdateRequest left_request{"0-to-1", EdgeSet{{0, 1}}, {}};
  const UpdateRequest right_request{"1-to-0", EdgeSet{{1, 0}}, {}};
  const auto left = delta::prepare_delta(base, left_request);
  const auto right = delta::prepare_delta(base, right_request);
  const auto merged = delta::merge_deltas(base, {left, right});
  const auto validation = delta::validate_delta(base, merged);
  const auto comparison =
      delta::compare_two_serial_orders(base, left_request, right_request);
  std::cout << "individual deltas valid: "
            << (delta::validate_delta(base, left).valid &&
                delta::validate_delta(base, right).valid)
            << "\n";
  std::cout << "merged delta valid: " << validation.valid << "\n";
  std::cout << "reason: " << validation.reasons.front() << "\n";
  std::cout << "both atomic serial orders also reject: "
            << comparison.agrees_with_both_orders << "\n";
}

void deletion_witness() {
  heading("4. Deletions with a shared reachability witness");
  const auto base = GraphState::create(
      4, EdgeSet{{0, 1}, {1, 3}, {0, 2}, {2, 3}});
  const auto left = delta::prepare_delta(
      base, UpdateRequest{"remove-left", {}, EdgeSet{{1, 3}}});
  const auto right = delta::prepare_delta(
      base, UpdateRequest{"remove-right", {}, EdgeSet{{2, 3}}});
  const auto merged = delta::merge_deltas(base, {left, right});
  const auto naive = delta::naive_metadata_union(base, {left, right});
  std::cout << "each delta removes 0~>3: "
            << left.reachability_removed.contains(Edge{0, 3}) << ", "
            << right.reachability_removed.contains(Edge{0, 3}) << "\n";
  std::cout << "combined delta removes 0~>3: "
            << merged.reachability_removed.contains(Edge{0, 3}) << "\n";
  std::cout << "naive union is correct: "
            << (naive == merged.candidate_reachability) << "\n";
}

void insertion_chain() {
  heading("5. Insertions that create an emergent path");
  const auto base = GraphState::create(3);
  const auto left = delta::prepare_delta(
      base, UpdateRequest{"0-to-1", EdgeSet{{0, 1}}, {}});
  const auto right = delta::prepare_delta(
      base, UpdateRequest{"1-to-2", EdgeSet{{1, 2}}, {}});
  const auto merged = delta::merge_deltas(base, {left, right});
  const auto naive = delta::naive_metadata_union(base, {left, right});
  std::cout << "naive union contains 0~>2: " << naive.contains(Edge{0, 2})
            << "\n";
  std::cout << "combined recomputation contains 0~>2: "
            << merged.candidate_reachability.contains(Edge{0, 2}) << "\n";
  std::cout << "dependency regions overlap: "
            << !delta::deltas_are_disjoint(left, right) << "\n";
}

}  // namespace

int main() {
  std::cout << std::boolalpha << "Dynamic hierarchy delta model\n";
  disjoint_additions();
  overlapping_additions();
  joint_cycle();
  deletion_witness();
  insertion_chain();
}
