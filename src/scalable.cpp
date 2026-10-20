#include "delta/scalable.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace delta::scalable {
namespace {

[[nodiscard]] bool endpoint_in_range(Edge edge, std::size_t node_count) {
    return edge.from < node_count && edge.to < node_count;
}

void sort_unique(std::vector<Edge>& edges) {
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
}

[[nodiscard]] bool intersects(const std::vector<Edge>& left, const std::vector<Edge>& right) {
    std::size_t l = 0;
    std::size_t r = 0;
    while (l < left.size() && r < right.size()) {
        if (left[l] == right[r]) {
            return true;
        }
        if (left[l] < right[r]) {
            ++l;
        } else {
            ++r;
        }
    }
    return false;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::optional<Node> extra_parent(Node child, Node primary, std::uint64_t seed,
                                               std::uint32_t branching_factor,
                                               bool stay_in_top_level_branch) {
    const Node child_branch = top_level_branch(child, branching_factor);
    for (std::uint64_t attempt = 0; attempt < 64U; ++attempt) {
        const Node candidate = static_cast<Node>(mix(seed + child + attempt) % child);
        if (candidate != primary &&
            (!stay_in_top_level_branch ||
             top_level_branch(candidate, branching_factor) == child_branch)) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<Edge> mutations(const Batch& batch) {
    std::vector<Edge> result = batch.additions;
    result.insert(result.end(), batch.deletions.begin(), batch.deletions.end());
    return result;
}

} // namespace

CsrGraph::CsrGraph(std::size_t node_count, std::vector<std::uint64_t> offsets,
                   std::vector<Node> targets)
    : node_count_(node_count), offsets_(std::move(offsets)), targets_(std::move(targets)) {
    if (offsets_.size() != node_count_ + 1U || offsets_.front() != 0U ||
        offsets_.back() != targets_.size()) {
        throw std::invalid_argument("invalid CSR layout");
    }
}

CsrGraph CsrGraph::from_edges(std::size_t node_count, std::vector<Edge> edges) {
    if (node_count > std::numeric_limits<Node>::max()) {
        throw std::invalid_argument("node count exceeds 32-bit node identifiers");
    }
    sort_unique(edges);
    std::vector<std::uint64_t> offsets(node_count + 1U, 0U);
    for (const Edge edge : edges) {
        if (!endpoint_in_range(edge, node_count)) {
            throw std::invalid_argument("edge endpoint is out of range");
        }
        ++offsets[static_cast<std::size_t>(edge.from) + 1U];
    }
    std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
    std::vector<Node> targets;
    targets.reserve(edges.size());
    for (const Edge edge : edges) {
        targets.push_back(edge.to);
    }
    return CsrGraph(node_count, std::move(offsets), std::move(targets));
}

std::size_t CsrGraph::node_count() const noexcept {
    return node_count_;
}

std::size_t CsrGraph::edge_count() const noexcept {
    return targets_.size();
}

std::span<const Node> CsrGraph::children(Node node) const {
    if (node >= node_count_) {
        throw std::out_of_range("node is out of range");
    }
    const auto begin = static_cast<std::size_t>(offsets_[node]);
    const auto end = static_cast<std::size_t>(offsets_[node + 1U]);
    if (begin == end) {
        return {};
    }
    return std::span<const Node>(targets_.data() + begin, end - begin);
}

bool CsrGraph::contains(Edge edge) const {
    if (!endpoint_in_range(edge, node_count_)) {
        return false;
    }
    const auto row = children(edge.from);
    return std::binary_search(row.begin(), row.end(), edge.to);
}

bool CsrGraph::is_acyclic() const {
    std::vector<std::uint32_t> indegree(node_count_, 0U);
    for (const Node target : targets_) {
        if (indegree[target] == std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("node indegree exceeds 32-bit counter");
        }
        ++indegree[target];
    }
    std::queue<Node> ready;
    for (std::size_t node = 0; node < node_count_; ++node) {
        if (indegree[node] == 0U) {
            ready.push(static_cast<Node>(node));
        }
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const Node node = ready.front();
        ready.pop();
        ++visited;
        for (const Node child : children(node)) {
            --indegree[child];
            if (indegree[child] == 0U) {
                ready.push(child);
            }
        }
    }
    return visited == node_count_;
}

bool CsrGraph::follows_node_order() const {
    for (std::size_t source = 0; source < node_count_; ++source) {
        for (const Node target : children(static_cast<Node>(source))) {
            if (source >= target) {
                return false;
            }
        }
    }
    return true;
}

std::uint64_t CsrGraph::storage_bytes() const noexcept {
    return static_cast<std::uint64_t>(offsets_.capacity()) * sizeof(std::uint64_t) +
           static_cast<std::uint64_t>(targets_.capacity()) * sizeof(Node);
}

const std::vector<std::uint64_t>& CsrGraph::offsets() const noexcept {
    return offsets_;
}

const std::vector<Node>& CsrGraph::targets() const noexcept {
    return targets_;
}

ApplyResult apply_batch(const CsrGraph& base, Batch batch, ApplyOptions options) {
    sort_unique(batch.additions);
    sort_unique(batch.deletions);
    if (intersects(batch.additions, batch.deletions)) {
        return {false, "an edge cannot be added and deleted in one batch", {}};
    }
    for (const Edge edge : batch.additions) {
        if (!endpoint_in_range(edge, base.node_count())) {
            return {false, "addition endpoint is out of range", {}};
        }
        if (edge.from == edge.to) {
            return {false, "self-loop addition is not allowed", {}};
        }
        if (base.contains(edge)) {
            return {false, "addition already exists", {}};
        }
    }
    for (const Edge edge : batch.deletions) {
        if (!endpoint_in_range(edge, base.node_count())) {
            return {false, "deletion endpoint is out of range", {}};
        }
        if (!base.contains(edge)) {
            return {false, "deletion does not exist", {}};
        }
    }

    std::vector<std::uint64_t> offsets(base.node_count() + 1U, 0U);
    std::size_t add_index = 0;
    std::size_t delete_index = 0;
    for (std::size_t source = 0; source < base.node_count(); ++source) {
        std::size_t additions = 0;
        while (add_index + additions < batch.additions.size() &&
               batch.additions[add_index + additions].from == source) {
            ++additions;
        }
        std::size_t deletions = 0;
        while (delete_index + deletions < batch.deletions.size() &&
               batch.deletions[delete_index + deletions].from == source) {
            ++deletions;
        }
        offsets[source + 1U] = offsets[source] + base.children(static_cast<Node>(source)).size() +
                               additions - deletions;
        add_index += additions;
        delete_index += deletions;
    }

    std::vector<Node> targets;
    targets.reserve(static_cast<std::size_t>(offsets.back()));
    add_index = 0;
    delete_index = 0;
    for (std::size_t source = 0; source < base.node_count(); ++source) {
        const auto existing = base.children(static_cast<Node>(source));
        const std::size_t add_begin = add_index;
        const std::size_t delete_begin = delete_index;
        while (add_index < batch.additions.size() && batch.additions[add_index].from == source) {
            ++add_index;
        }
        while (delete_index < batch.deletions.size() &&
               batch.deletions[delete_index].from == source) {
            ++delete_index;
        }

        std::size_t old = 0;
        std::size_t added = add_begin;
        std::size_t removed = delete_begin;
        while (old < existing.size() || added < add_index) {
            const Node old_target =
                old < existing.size() ? existing[old] : std::numeric_limits<Node>::max();
            const Node new_target =
                added < add_index ? batch.additions[added].to : std::numeric_limits<Node>::max();
            if (new_target < old_target) {
                targets.push_back(new_target);
                ++added;
            } else {
                const bool deleted =
                    removed < delete_index && batch.deletions[removed].to == old_target;
                if (!deleted) {
                    targets.push_back(old_target);
                } else {
                    ++removed;
                }
                ++old;
            }
        }
    }

    CsrGraph candidate(base.node_count(), std::move(offsets), std::move(targets));
    if (options.assume_node_order_is_topological) {
        if (!candidate.follows_node_order()) {
            return {false, "an edge violates the declared topological node order", {}};
        }
    } else if (!candidate.is_acyclic()) {
        return {false, "combined batch creates a cycle", {}};
    }
    return {true, {}, std::move(candidate)};
}

Node primary_parent(Node node, std::uint32_t branching_factor) {
    if (node == 0U) {
        return 0U;
    }
    if (branching_factor < 2U) {
        throw std::invalid_argument("branching factor must be at least two");
    }
    return static_cast<Node>((node - 1U) / branching_factor);
}

CsrGraph make_tree_dag(const TreeSpec& spec) {
    if (spec.node_count == 0U || spec.node_count > std::numeric_limits<Node>::max()) {
        throw std::invalid_argument("node count must fit a non-empty 32-bit graph");
    }
    if (spec.branching_factor < 2U) {
        throw std::invalid_argument("branching factor must be at least two");
    }
    std::vector<std::uint64_t> offsets(spec.node_count + 1U, 0U);
    for (Node child = 1U; child < spec.node_count; ++child) {
        const Node parent = primary_parent(child, spec.branching_factor);
        ++offsets[static_cast<std::size_t>(parent) + 1U];
        if (spec.extra_parent_every != 0U && child > 1U && child % spec.extra_parent_every == 0U) {
            const auto extra = extra_parent(child, parent, spec.seed, spec.branching_factor,
                                            spec.extra_parents_stay_in_top_level_branch);
            if (extra.has_value()) {
                ++offsets[static_cast<std::size_t>(*extra) + 1U];
            }
        }
    }
    std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
    std::vector<Node> targets(static_cast<std::size_t>(offsets.back()));
    auto cursor = offsets;
    for (Node child = 1U; child < spec.node_count; ++child) {
        const Node parent = primary_parent(child, spec.branching_factor);
        targets[static_cast<std::size_t>(cursor[parent]++)] = child;
        if (spec.extra_parent_every != 0U && child > 1U && child % spec.extra_parent_every == 0U) {
            const auto extra = extra_parent(child, parent, spec.seed, spec.branching_factor,
                                            spec.extra_parents_stay_in_top_level_branch);
            if (extra.has_value()) {
                targets[static_cast<std::size_t>(cursor[*extra]++)] = child;
            }
        }
    }
    for (std::size_t node = 0; node < spec.node_count; ++node) {
        const auto begin = targets.begin() + static_cast<std::ptrdiff_t>(offsets[node]);
        const auto end = targets.begin() + static_cast<std::ptrdiff_t>(offsets[node + 1U]);
        std::sort(begin, end);
    }
    return CsrGraph(spec.node_count, std::move(offsets), std::move(targets));
}

Node tree_lca(Node left, Node right, std::uint32_t branching_factor) {
    while (left != right) {
        if (left > right) {
            left = primary_parent(left, branching_factor);
        } else {
            right = primary_parent(right, branching_factor);
        }
    }
    return left;
}

Node top_level_branch(Node node, std::uint32_t branching_factor) {
    while (node != 0U && primary_parent(node, branching_factor) != 0U) {
        node = primary_parent(node, branching_factor);
    }
    return node;
}

Node DominatorTree::lca(Node left, Node right) const {
    if (left >= immediate_dominator.size() || right >= immediate_dominator.size()) {
        throw std::out_of_range("dominator-tree node is out of range");
    }
    while (depth[left] > depth[right]) {
        left = immediate_dominator[left];
    }
    while (depth[right] > depth[left]) {
        right = immediate_dominator[right];
    }
    while (left != right) {
        left = immediate_dominator[left];
        right = immediate_dominator[right];
    }
    return left;
}

std::uint64_t DominatorTree::storage_bytes() const noexcept {
    return static_cast<std::uint64_t>(immediate_dominator.capacity()) * sizeof(Node) +
           static_cast<std::uint64_t>(depth.capacity()) * sizeof(std::uint32_t);
}

DominatorTree compute_dominator_tree_ordered(const CsrGraph& graph, Node root) {
    if (graph.node_count() == 0U || root >= graph.node_count()) {
        throw std::invalid_argument("dominator root is out of range");
    }
    if (root != 0U || !graph.follows_node_order()) {
        throw std::invalid_argument(
            "ordered dominator construction requires root 0 and forward edges");
    }

    std::vector<std::uint64_t> reverse_offsets(graph.node_count() + 1U, 0U);
    for (const Node target : graph.targets()) {
        ++reverse_offsets[static_cast<std::size_t>(target) + 1U];
    }
    std::partial_sum(reverse_offsets.begin(), reverse_offsets.end(), reverse_offsets.begin());
    std::vector<Node> predecessors(graph.edge_count());
    auto cursor = reverse_offsets;
    for (std::size_t source = 0; source < graph.node_count(); ++source) {
        for (const Node target : graph.children(static_cast<Node>(source))) {
            predecessors[static_cast<std::size_t>(cursor[target]++)] = static_cast<Node>(source);
        }
    }

    DominatorTree result;
    result.immediate_dominator.resize(graph.node_count());
    result.depth.resize(graph.node_count(), 0U);
    result.immediate_dominator[root] = root;
    for (std::size_t node = 1U; node < graph.node_count(); ++node) {
        const auto begin = static_cast<std::size_t>(reverse_offsets[node]);
        const auto end = static_cast<std::size_t>(reverse_offsets[node + 1U]);
        if (begin == end) {
            throw std::invalid_argument(
                "ordered dominator construction requires every node to be reachable");
        }
        Node common = predecessors[begin];
        for (std::size_t index = begin + 1U; index < end; ++index) {
            common = result.lca(common, predecessors[index]);
        }
        result.immediate_dominator[node] = common;
        result.depth[node] = result.depth[common] + 1U;
    }
    return result;
}

Batch make_update_batch(const CsrGraph& graph, std::size_t addition_count,
                        std::size_t deletion_count, std::uint32_t branching_factor,
                        Locality locality, std::uint64_t seed) {
    if (graph.node_count() < 3U) {
        throw std::invalid_argument("update generation needs at least three nodes");
    }
    Batch batch;
    batch.additions.reserve(addition_count);
    batch.deletions.reserve(deletion_count);
    std::mt19937_64 random(seed);

    std::unordered_set<std::uint64_t> used;
    used.reserve((addition_count + deletion_count) * 2U + 1U);
    auto key = [](Edge edge) { return (static_cast<std::uint64_t>(edge.from) << 32U) | edge.to; };

    std::size_t attempts = 0;
    while (batch.additions.size() < addition_count) {
        if (++attempts > addition_count * 100U + 10000U) {
            throw std::runtime_error("could not generate enough valid additions");
        }
        Node left = static_cast<Node>(1U + random() % (graph.node_count() - 1U));
        Node right = static_cast<Node>(1U + random() % (graph.node_count() - 1U));
        if (locality == Locality::same_top_level_branch) {
            const Node branch = top_level_branch(left, branching_factor);
            for (int retry = 0; retry < 12 && top_level_branch(right, branching_factor) != branch;
                 ++retry) {
                right = static_cast<Node>(1U + random() % (graph.node_count() - 1U));
            }
            if (top_level_branch(right, branching_factor) != branch) {
                continue;
            }
        }
        Edge edge{std::min(left, right), std::max(left, right)};
        if (edge.from == edge.to || graph.contains(edge) || !used.insert(key(edge)).second) {
            continue;
        }
        batch.additions.push_back(edge);
    }

    attempts = 0;
    while (batch.deletions.size() < deletion_count) {
        if (++attempts > deletion_count * 100U + 10000U) {
            throw std::runtime_error("could not generate enough valid deletions");
        }
        const Node child = static_cast<Node>(1U + random() % (graph.node_count() - 1U));
        Edge edge{primary_parent(child, branching_factor), child};
        if (used.insert(key(edge)).second) {
            batch.deletions.push_back(edge);
        }
    }
    sort_unique(batch.additions);
    sort_unique(batch.deletions);
    return batch;
}

DominatorStats measure_dominator_stats(const Batch& batch, const DominatorTree& dominators,
                                       std::uint32_t branching_factor) {
    const auto edges = mutations(batch);
    DominatorStats stats;
    stats.mutation_count = edges.size();
    if (edges.empty()) {
        return stats;
    }
    std::set<Node> lcas;
    std::set<Node> branches;
    Node whole = dominators.lca(edges.front().from, edges.front().to);
    for (const Edge edge : edges) {
        const Node lca = dominators.lca(edge.from, edge.to);
        lcas.insert(lca);
        if (lca == 0U) {
            ++stats.root_edge_lca_count;
        }
        branches.insert(top_level_branch(lca, branching_factor));
        whole = dominators.lca(whole, edge.from);
        whole = dominators.lca(whole, edge.to);
    }
    stats.whole_batch_lca = whole;
    stats.distinct_edge_lcas = lcas.size();
    stats.distinct_top_level_groups = branches.size();
    return stats;
}

} // namespace delta::scalable
