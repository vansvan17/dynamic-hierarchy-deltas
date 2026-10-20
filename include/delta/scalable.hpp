#pragma once

#include "delta/model.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace delta::scalable {

class CsrGraph {
  public:
    CsrGraph() = default;
    CsrGraph(std::size_t node_count, std::vector<std::uint64_t> offsets, std::vector<Node> targets);

    [[nodiscard]] static CsrGraph from_edges(std::size_t node_count, std::vector<Edge> edges);
    [[nodiscard]] std::size_t node_count() const noexcept;
    [[nodiscard]] std::size_t edge_count() const noexcept;
    [[nodiscard]] std::span<const Node> children(Node node) const;
    [[nodiscard]] bool contains(Edge edge) const;
    [[nodiscard]] bool is_acyclic() const;
    [[nodiscard]] bool follows_node_order() const;
    [[nodiscard]] std::uint64_t storage_bytes() const noexcept;

    [[nodiscard]] const std::vector<std::uint64_t>& offsets() const noexcept;
    [[nodiscard]] const std::vector<Node>& targets() const noexcept;

  private:
    std::size_t node_count_{};
    std::vector<std::uint64_t> offsets_;
    std::vector<Node> targets_;
};

struct Batch {
    std::vector<Edge> additions;
    std::vector<Edge> deletions;
};

struct ApplyOptions {
    bool assume_node_order_is_topological{false};
};

struct ApplyResult {
    bool accepted{};
    std::string reason;
    CsrGraph graph;
};

[[nodiscard]] ApplyResult apply_batch(const CsrGraph& base, Batch batch, ApplyOptions options = {});

struct TreeSpec {
    std::size_t node_count{};
    std::uint32_t branching_factor{4};
    std::uint32_t extra_parent_every{0};
    std::uint64_t seed{1};
    bool extra_parents_stay_in_top_level_branch{true};
};

[[nodiscard]] CsrGraph make_tree_dag(const TreeSpec& spec);
[[nodiscard]] Node primary_parent(Node node, std::uint32_t branching_factor);
[[nodiscard]] Node tree_lca(Node left, Node right, std::uint32_t branching_factor);
[[nodiscard]] Node top_level_branch(Node node, std::uint32_t branching_factor);

struct DominatorTree {
    std::vector<Node> immediate_dominator;
    std::vector<std::uint32_t> depth;

    [[nodiscard]] Node lca(Node left, Node right) const;
    [[nodiscard]] std::uint64_t storage_bytes() const noexcept;
};

[[nodiscard]] DominatorTree compute_dominator_tree_ordered(const CsrGraph& graph, Node root = 0U);

enum class Locality { global, same_top_level_branch };

[[nodiscard]] Batch make_update_batch(const CsrGraph& graph, std::size_t addition_count,
                                      std::size_t deletion_count, std::uint32_t branching_factor,
                                      Locality locality, std::uint64_t seed);

struct DominatorStats {
    Node whole_batch_lca{};
    std::size_t mutation_count{};
    std::size_t root_edge_lca_count{};
    std::size_t distinct_edge_lcas{};
    std::size_t distinct_top_level_groups{};
};

[[nodiscard]] DominatorStats measure_dominator_stats(const Batch& batch,
                                                     const DominatorTree& dominators,
                                                     std::uint32_t branching_factor);

} // namespace delta::scalable
