#include "delta/model.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using delta::Edge;
using delta::EdgeSet;
using delta::GraphState;
using delta::UpdateRequest;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void prepare_and_apply_addition() {
  const auto base = GraphState::create(3, EdgeSet{{0, 1}});
  const auto prepared = delta::prepare_delta(
      base, UpdateRequest{"add", EdgeSet{{1, 2}}, {}});
  require(delta::validate_delta(base, prepared).valid,
          "addition should validate");
  require(prepared.reachability_added.contains(Edge{0, 2}),
          "addition should create transitive reachability");
  const auto result = delta::apply_delta(base, prepared);
  require(result.version == 1, "apply should increment version");
}

void disjoint_updates() {
  const auto base = GraphState::create(4);
  const auto left = delta::prepare_delta(
      base, UpdateRequest{"left", EdgeSet{{0, 1}}, {}});
  const auto right = delta::prepare_delta(
      base, UpdateRequest{"right", EdgeSet{{2, 3}}, {}});
  require(delta::deltas_are_disjoint(left, right),
          "separate additions should be disjoint");
}

void overlapping_updates_merge() {
  const auto base = GraphState::create(
      6, EdgeSet{{0, 1}, {0, 2}, {1, 3}, {2, 4}});
  const UpdateRequest left_request{"left", EdgeSet{{3, 5}}, {}};
  const UpdateRequest right_request{"right", EdgeSet{{4, 5}}, {}};
  const auto left = delta::prepare_delta(base, left_request);
  const auto right = delta::prepare_delta(base, right_request);
  require(!delta::deltas_are_disjoint(left, right),
          "updates should overlap through nodes 0 and 5");
  const auto merged = delta::merge_deltas(base, {left, right});
  require(delta::validate_delta(base, merged).valid,
          "overlapping additions should merge");
  require(delta::compare_two_serial_orders(base, left_request, right_request)
              .agrees_with_both_orders,
          "merge should match both serial orders");
}

void joint_cycle_rejected() {
  const auto base = GraphState::create(2);
  const UpdateRequest left_request{"left", EdgeSet{{0, 1}}, {}};
  const UpdateRequest right_request{"right", EdgeSet{{1, 0}}, {}};
  const auto left = delta::prepare_delta(base, left_request);
  const auto right = delta::prepare_delta(base, right_request);
  require(delta::validate_delta(base, left).valid &&
              delta::validate_delta(base, right).valid,
          "individual additions should validate");
  const auto merged = delta::merge_deltas(base, {left, right});
  require(!delta::validate_delta(base, merged).valid,
          "combined cycle should be rejected");
  require(delta::compare_two_serial_orders(base, left_request, right_request)
              .agrees_with_both_orders,
          "atomic serial orders should also reject");
}

void deletion_witness_breaks_naive_union() {
  const auto base = GraphState::create(
      4, EdgeSet{{0, 1}, {1, 3}, {0, 2}, {2, 3}});
  const auto left = delta::prepare_delta(
      base, UpdateRequest{"left", {}, EdgeSet{{1, 3}}});
  const auto right = delta::prepare_delta(
      base, UpdateRequest{"right", {}, EdgeSet{{2, 3}}});
  require(!left.reachability_removed.contains(Edge{0, 3}) &&
              !right.reachability_removed.contains(Edge{0, 3}),
          "each branch should retain the alternate witness");
  const auto merged = delta::merge_deltas(base, {left, right});
  require(merged.reachability_removed.contains(Edge{0, 3}),
          "combined deletion should remove the last witness");
  require(delta::naive_metadata_union(base, {left, right}) !=
              merged.candidate_reachability,
          "naive union should be wrong");
}

void insertion_chain_breaks_naive_union() {
  const auto base = GraphState::create(3);
  const auto left = delta::prepare_delta(
      base, UpdateRequest{"left", EdgeSet{{0, 1}}, {}});
  const auto right = delta::prepare_delta(
      base, UpdateRequest{"right", EdgeSet{{1, 2}}, {}});
  const auto merged = delta::merge_deltas(base, {left, right});
  require(!delta::naive_metadata_union(base, {left, right})
               .contains(Edge{0, 2}),
          "private deltas should miss the emergent path");
  require(merged.candidate_reachability.contains(Edge{0, 2}),
          "combined recomputation should find the emergent path");
}

void stale_delta_rejected() {
  const auto base = GraphState::create(3);
  const auto old = delta::prepare_delta(
      base, UpdateRequest{"old", EdgeSet{{0, 1}}, {}});
  const auto other = delta::prepare_delta(
      base, UpdateRequest{"other", EdgeSet{{1, 2}}, {}});
  const auto current = delta::apply_delta(base, other);
  const auto validation = delta::validate_delta(current, old);
  require(!validation.valid, "stale delta should fail validation");
  require(delta::join_reasons(validation.reasons).find("stale") !=
              std::string::npos,
          "stale reason should be reported");
}

void duplicate_edge_owner_rejected() {
  const auto base = GraphState::create(2);
  const auto left = delta::prepare_delta(
      base, UpdateRequest{"left", EdgeSet{{0, 1}}, {}});
  const auto right = delta::prepare_delta(
      base, UpdateRequest{"right", EdgeSet{{0, 1}}, {}});
  bool threw = false;
  try {
    static_cast<void>(delta::merge_deltas(base, {left, right}));
  } catch (const delta::DeltaConflict&) {
    threw = true;
  }
  require(threw, "duplicate edge ownership should be rejected");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"prepare_and_apply_addition", prepare_and_apply_addition},
      {"disjoint_updates", disjoint_updates},
      {"overlapping_updates_merge", overlapping_updates_merge},
      {"joint_cycle_rejected", joint_cycle_rejected},
      {"deletion_witness_breaks_naive_union",
       deletion_witness_breaks_naive_union},
      {"insertion_chain_breaks_naive_union",
       insertion_chain_breaks_naive_union},
      {"stale_delta_rejected", stale_delta_rejected},
      {"duplicate_edge_owner_rejected", duplicate_edge_owner_rejected},
  };

  std::size_t failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size()
            << " tests passed\n";
  return failures == 0 ? 0 : 1;
}
