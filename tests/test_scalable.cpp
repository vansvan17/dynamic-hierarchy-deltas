#include "delta/model.hpp"
#include "delta/scalable.hpp"

#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using delta::Edge;
using delta::EdgeSet;
using delta::scalable::Batch;
using delta::scalable::CsrGraph;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] EdgeSet edge_set(const CsrGraph& graph) {
    EdgeSet result;
    for (std::size_t source = 0; source < graph.node_count(); ++source) {
        for (const auto target : graph.children(static_cast<delta::Node>(source))) {
            result.insert(Edge{static_cast<delta::Node>(source), target});
        }
    }
    return result;
}

void csr_round_trip() {
    const std::vector<Edge> edges{{2, 3}, {0, 2}, {0, 1}, {0, 1}};
    const auto graph = CsrGraph::from_edges(4, edges);
    require(graph.edge_count() == 3U, "duplicate edge should be removed");
    require(graph.contains({0, 1}) && graph.contains({2, 3}), "round-tripped graph lost edges");
    require(graph.is_acyclic(), "test graph should be acyclic");
}

void cycle_is_rejected() {
    const auto base = CsrGraph::from_edges(3, {{0, 1}, {1, 2}});
    const auto result = delta::scalable::apply_batch(base, Batch{{{2, 0}}, {}});
    require(!result.accepted, "cycle-creating batch must be rejected");
}

void batch_matches_exact_oracle() {
    const EdgeSet base_edges{{0, 1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}};
    const Batch batch{{{4, 5}, {2, 3}}, {{1, 3}}};
    const auto scalable_base =
        CsrGraph::from_edges(6, std::vector<Edge>(base_edges.begin(), base_edges.end()));
    const auto scalable_result = delta::scalable::apply_batch(scalable_base, batch);
    require(scalable_result.accepted, "scalable batch should validate");

    const auto exact_base = delta::GraphState::create(6, base_edges);
    const delta::UpdateRequest request{"batch",
                                       EdgeSet(batch.additions.begin(), batch.additions.end()),
                                       EdgeSet(batch.deletions.begin(), batch.deletions.end())};
    const auto prepared = delta::prepare_delta(exact_base, request);
    require(delta::validate_delta(exact_base, prepared).valid, "exact batch should validate");
    const auto exact_result = delta::apply_delta(exact_base, prepared);
    require(edge_set(scalable_result.graph) == exact_result.edges,
            "sparse result differs from exact oracle");
}

void ordered_fast_path() {
    const auto base = delta::scalable::make_tree_dag({1000, 4, 8, 9});
    require(base.follows_node_order(), "generated graph must follow node order");
    const auto batch =
        delta::scalable::make_update_batch(base, 50, 50, 4, delta::scalable::Locality::global, 10);
    const auto result =
        delta::scalable::apply_batch(base, batch, {.assume_node_order_is_topological = true});
    require(result.accepted, "generated forward-edge batch should validate");
    require(result.graph.edge_count() == base.edge_count(),
            "equal additions and deletions should preserve edge count");
}

void locality_changes_root_pressure() {
    const auto graph = delta::scalable::make_tree_dag({10000, 4, 0, 11});
    const auto global =
        delta::scalable::make_update_batch(graph, 500, 0, 4, delta::scalable::Locality::global, 12);
    const auto local = delta::scalable::make_update_batch(
        graph, 500, 0, 4, delta::scalable::Locality::same_top_level_branch, 12);
    const auto dominators = delta::scalable::compute_dominator_tree_ordered(graph);
    const auto global_stats = delta::scalable::measure_dominator_stats(global, dominators, 4);
    const auto local_stats = delta::scalable::measure_dominator_stats(local, dominators, 4);
    require(global_stats.root_edge_lca_count > local_stats.root_edge_lca_count,
            "local batching should reduce root-selected edge groups");
}

void ordered_dominator_tree() {
    const auto graph = CsrGraph::from_edges(6, {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 4}, {1, 5}});
    const auto dominators = delta::scalable::compute_dominator_tree_ordered(graph);
    require(dominators.immediate_dominator[3] == 0U,
            "shared child should be immediately dominated by root");
    require(dominators.immediate_dominator[4] == 3U,
            "single-predecessor child should be dominated by its predecessor");
    require(dominators.lca(4, 5) == 0U, "dominator-tree LCA is wrong");
}

void invalid_preconditions_are_rejected() {
    const auto base = CsrGraph::from_edges(3, {{0, 1}});
    require(!delta::scalable::apply_batch(base, Batch{{{0, 1}}, {}}).accepted,
            "existing addition should fail");
    require(!delta::scalable::apply_batch(base, Batch{{}, {{1, 2}}}).accepted,
            "missing deletion should fail");
    require(!delta::scalable::apply_batch(base, Batch{{{1, 2}}, {{1, 2}}}).accepted,
            "same-edge add/delete should fail");
}

void randomized_sparse_exact_crosscheck() {
    std::mt19937_64 random(91);
    for (std::size_t case_number = 0; case_number < 200U; ++case_number) {
        constexpr std::size_t nodes = 8U;
        EdgeSet base_edges;
        for (delta::Node from = 0; from < nodes; ++from) {
            for (delta::Node to = from + 1U; to < nodes; ++to) {
                if (random() % 5U == 0U) {
                    base_edges.insert({from, to});
                }
            }
        }

        Batch batch;
        for (const Edge edge : base_edges) {
            if (random() % 7U == 0U) {
                batch.deletions.push_back(edge);
            }
        }
        for (delta::Node from = 0; from < nodes; ++from) {
            for (delta::Node to = from + 1U; to < nodes; ++to) {
                const Edge edge{from, to};
                if (!base_edges.contains(edge) && random() % 11U == 0U) {
                    batch.additions.push_back(edge);
                }
            }
        }

        const auto sparse_base =
            CsrGraph::from_edges(nodes, std::vector<Edge>(base_edges.begin(), base_edges.end()));
        const auto sparse_result = delta::scalable::apply_batch(sparse_base, batch);
        require(sparse_result.accepted, "forward-only random batch was rejected");

        const auto exact_base = delta::GraphState::create(nodes, base_edges);
        const delta::UpdateRequest request{"random",
                                           EdgeSet(batch.additions.begin(), batch.additions.end()),
                                           EdgeSet(batch.deletions.begin(), batch.deletions.end())};
        const auto prepared = delta::prepare_delta(exact_base, request);
        require(delta::validate_delta(exact_base, prepared).valid,
                "exact oracle rejected forward-only random batch");
        const auto exact_result = delta::apply_delta(exact_base, prepared);
        require(edge_set(sparse_result.graph) == exact_result.edges,
                "random sparse result differs from exact oracle");
    }
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"csr_round_trip", csr_round_trip},
        {"cycle_is_rejected", cycle_is_rejected},
        {"batch_matches_exact_oracle", batch_matches_exact_oracle},
        {"ordered_fast_path", ordered_fast_path},
        {"locality_changes_root_pressure", locality_changes_root_pressure},
        {"ordered_dominator_tree", ordered_dominator_tree},
        {"invalid_preconditions_are_rejected", invalid_preconditions_are_rejected},
        {"randomized_sparse_exact_crosscheck", randomized_sparse_exact_crosscheck},
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
    std::cout << tests.size() - failures << '/' << tests.size() << " scalable tests passed\n";
    return failures == 0U ? 0 : 1;
}
