#include "delta/model.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using delta::Edge;
using delta::EdgeSet;
using delta::GraphState;
using delta::PreparedDelta;
using delta::UpdateRequest;

struct Candidate {
  UpdateRequest request;
  PreparedDelta prepared;
};

std::vector<GraphState> ordered_dags(std::size_t node_count) {
  std::vector<Edge> possible;
  for (delta::Node from = 0; from < node_count; ++from) {
    for (delta::Node to = from + 1; to < node_count; ++to) {
      possible.push_back(Edge{from, to});
    }
  }
  if (possible.size() >= 63) {
    throw std::invalid_argument("node count is too large for exhaustive mode");
  }

  std::vector<GraphState> result;
  const std::uint64_t combinations = std::uint64_t{1} << possible.size();
  result.reserve(static_cast<std::size_t>(combinations));
  for (std::uint64_t mask = 0; mask < combinations; ++mask) {
    EdgeSet edges;
    for (std::size_t index = 0; index < possible.size(); ++index) {
      if ((mask & (std::uint64_t{1} << index)) != 0) {
        edges.insert(possible[index]);
      }
    }
    result.push_back(GraphState::create(node_count, std::move(edges)));
  }
  return result;
}

std::vector<Candidate> valid_single_edge_requests(const GraphState& base) {
  std::vector<Candidate> result;
  for (delta::Node from = 0; from < base.node_count; ++from) {
    for (delta::Node to = 0; to < base.node_count; ++to) {
      if (from == to) {
        continue;
      }
      const Edge edge{from, to};
      UpdateRequest request;
      if (base.edges.contains(edge)) {
        request = UpdateRequest{"del-" + delta::edge_to_string(edge), {},
                                EdgeSet{edge}};
      } else {
        request = UpdateRequest{"add-" + delta::edge_to_string(edge),
                                EdgeSet{edge}, {}};
      }
      auto prepared = delta::prepare_delta(base, request);
      if (delta::validate_delta(base, prepared).valid) {
        result.push_back(Candidate{std::move(request), std::move(prepared)});
      }
    }
  }
  return result;
}

std::string pair_kind(const PreparedDelta& left, const PreparedDelta& right) {
  const std::string left_kind = left.additions.empty() ? "delete" : "add";
  const std::string right_kind = right.additions.empty() ? "delete" : "add";
  return left_kind < right_kind ? left_kind + '-' + right_kind
                                : right_kind + '-' + left_kind;
}

void increment(std::map<std::string, std::uint64_t>& counts,
               const std::string& key) {
  ++counts[key];
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t node_count = 5;
  if (argc == 2) {
    node_count = static_cast<std::size_t>(std::stoul(argv[1]));
  }
  if (node_count < 2 || node_count > 5) {
    std::cerr << "usage: delta_exhaustive [nodes: 2..5]\n";
    return 2;
  }

  std::map<std::string, std::uint64_t> counts;
  std::string first_invalid;
  std::string first_naive_failure;

  for (const auto& base : ordered_dags(node_count)) {
    increment(counts, "base_dags");
    const auto candidates = valid_single_edge_requests(base);
    counts["individually_valid_deltas"] += candidates.size();
    for (std::size_t left_index = 0; left_index < candidates.size();
         ++left_index) {
      for (std::size_t right_index = left_index + 1;
           right_index < candidates.size(); ++right_index) {
        const auto& left = candidates[left_index];
        const auto& right = candidates[right_index];
        const auto left_edges = left.prepared.mutated_edges();
        const auto right_edges = right.prepared.mutated_edges();
        EdgeSet shared_edges;
        std::set_intersection(
            left_edges.begin(), left_edges.end(), right_edges.begin(),
            right_edges.end(),
            std::inserter(shared_edges, shared_edges.end()));
        if (!shared_edges.empty()) {
          continue;
        }

        increment(counts, "pairs");
        const auto kind = pair_kind(left.prepared, right.prepared);
        increment(counts, "pairs_" + kind);
        const bool disjoint =
            delta::deltas_are_disjoint(left.prepared, right.prepared);
        increment(counts, disjoint ? "disjoint_pairs" : "overlapping_pairs");

        const auto merged =
            delta::merge_deltas(base, {left.prepared, right.prepared});
        const auto validation = delta::validate_delta(base, merged);
        const auto comparison = delta::compare_two_serial_orders(
            base, left.request, right.request);
        if (comparison.agrees_with_both_orders) {
          increment(counts, "exact_outcome_matches_serial_oracle");
        } else {
          increment(counts, "exact_outcome_serial_mismatches");
        }

        if (!validation.valid) {
          increment(counts, "jointly_invalid");
          increment(counts, "jointly_invalid_" + kind);
          if (first_invalid.empty()) {
            first_invalid = "base=" + delta::edges_to_string(base.edges) +
                            " requests=" + left.request.name + ',' +
                            right.request.name + " reason=" +
                            validation.reasons.front();
          }
          continue;
        }

        increment(counts, "jointly_valid");
        const auto naive = delta::naive_metadata_union(
            base, {left.prepared, right.prepared});
        if (naive == merged.candidate_reachability) {
          increment(counts, "naive_metadata_union_correct");
        } else {
          increment(counts, "naive_metadata_union_wrong");
          increment(counts, "naive_metadata_union_wrong_" + kind);
          increment(counts, std::string("naive_metadata_union_wrong_") +
                                (disjoint ? "disjoint" : "overlapping"));
          if (first_naive_failure.empty()) {
            first_naive_failure =
                "base=" + delta::edges_to_string(base.edges) +
                " requests=" + left.request.name + ',' + right.request.name;
          }
        }
      }
    }
  }

  std::cout << "nodes=" << node_count << '\n';
  for (const auto& [key, value] : counts) {
    std::cout << key << '=' << value << '\n';
  }
  std::cout << "first_jointly_invalid=" << first_invalid << '\n';
  std::cout << "first_naive_union_failure=" << first_naive_failure << '\n';
  return counts["exact_outcome_serial_mismatches"] == 0 ? 0 : 1;
}
