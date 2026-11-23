// Shared pieces for the precision experiments in this folder.
#pragma once
#include "dh.hpp"
#include <cstdio>
#include <string>

namespace lab {
using namespace dh;

struct Pair {
    Node u, v;
};

// Exact answer to "do u and v share a descendant?". Each node counts as its own descendant.
// We mark everything below u, then search below v until we hit a marked node.
struct Oracle {
    const Csr& g;
    std::vector<std::uint32_t> mark;
    std::uint32_t stamp = 0;
    std::vector<Node> stack;
    std::uint64_t work = 0;  // nodes popped, summed over all calls
    explicit Oracle(const Csr& graph) : g(graph), mark(graph.n(), 0) {}

    bool shared(Node u, Node v) {
        const std::uint32_t below_u = ++stamp, below_v = ++stamp;
        walk(u, below_u, 0);
        if (mark[v] == below_u) return true;
        return walk(v, below_v, below_u);
    }
    // Marks everything reachable from s with `tag`. Returns true early if it meets `stop`.
    bool walk(Node s, std::uint32_t tag, std::uint32_t stop) {
        stack.assign(1, s);
        mark[s] = tag;
        while (!stack.empty()) {
            const Node x = stack.back();
            stack.pop_back();
            ++work;
            for (Node c : g.kids(x)) {
                if (stop && mark[c] == stop) return true;
                if (mark[c] != tag) mark[c] = tag, stack.push_back(c);
            }
        }
        return false;
    }
};

// Uniform pairs. With `in_branch`, v is redrawn (up to 64 times) until it shares u's
// top-level branch, and pairs that still don't are dropped.
inline std::vector<Pair> uniform_pairs(std::size_t n, Node b, std::size_t count, bool in_branch, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto any = [&] { return static_cast<Node>(1 + rng() % (n - 1)); };
    std::vector<Pair> out;
    while (out.size() < count) {
        const Node u = any();
        Node v = any();
        for (int r = 0; in_branch && r < 64 && top_branch(v, b) != top_branch(u, b); ++r) v = any();
        if (u != v && (!in_branch || top_branch(u, b) == top_branch(v, b))) out.push_back({u, v});
    }
    return out;
}

// Levels of the generator's b-ary primary tree. start[l] is the first node of level l (the
// root is level 0) and the last entry is n, so level l is [start[l], start[l + 1]).
struct Levels {
    std::vector<std::uint64_t> start{0};
    Levels(std::size_t n, Node b) {
        for (std::uint64_t width = 1; start.back() + width < n; width *= b) start.push_back(start.back() + width);
        start.push_back(n);
    }
    int deepest() const { return static_cast<int>(start.size()) - 2; }
    int of(Node v) const {
        return static_cast<int>(std::upper_bound(start.begin(), start.end(), std::uint64_t{v}) - start.begin()) - 1;
    }
    std::uint64_t width(int l) const { return start[l + 1] - start[l]; }
};

// Pairs whose endpoints each pick a level (1 to deepest) uniformly, then a node within it,
// so shallow nodes get sampled as often as deep ones. `in_branch` works as above (256 tries).
inline std::vector<Pair> level_pairs(const Levels& lv, Node b, std::size_t count, bool in_branch, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto pick = [&] {
        const int l = 1 + static_cast<int>(rng() % static_cast<std::uint64_t>(lv.deepest()));
        return static_cast<Node>(lv.start[l] + rng() % lv.width(l));
    };
    std::vector<Pair> out;
    while (out.size() < count) {
        const Node u = pick();
        Node v = pick();
        for (int r = 0; in_branch && r < 256 && top_branch(v, b) != top_branch(u, b); ++r) v = pick();
        if (u != v && (!in_branch || top_branch(u, b) == top_branch(v, b))) out.push_back({u, v});
    }
    return out;
}

// How many disjoint label ranges it takes to describe u's descendants exactly.
// `got` returns the descendants themselves, in case the caller wants their count.
inline std::size_t exact_ranges(const Csr& g, const std::vector<Node>& label, Node u, std::vector<Node>* got = nullptr) {
    std::vector<char> seen(g.n(), 0);
    std::vector<Node> stack{u}, below;
    seen[u] = 1;
    while (!stack.empty()) {
        const Node x = stack.back();
        stack.pop_back();
        below.push_back(x);
        for (Node c : g.kids(x))
            if (!seen[c]) seen[c] = 1, stack.push_back(c);
    }
    std::vector<Node> labels;
    for (Node x : below) labels.push_back(label[x]);
    std::sort(labels.begin(), labels.end());
    std::size_t runs = 0;
    for (std::size_t i = 0; i < labels.size(); ++i) runs += i == 0 || labels[i] != labels[i - 1] + 1;
    if (got) *got = std::move(below);
    return runs;
}

inline double rate(std::size_t hits, std::size_t out_of) { return out_of ? double(hits) / double(out_of) : 0.0; }

// ---- Label orders. Any permutation of labels is sound; only precision changes.

// Post-order over a spanning tree given as child lists.
inline std::vector<Node> tree_postorder(const Csr& t) {
    std::vector<Node> label(t.n(), 0);
    std::vector<std::pair<Node, std::uint64_t>> stack{{0, t.off[0]}};
    Node next = 0;
    while (!stack.empty()) {
        const Node v = stack.back().first;
        if (stack.back().second < t.off[v + 1]) {
            const Node c = t.dst[stack.back().second++];
            stack.push_back({c, t.off[c]});
        } else {
            label[v] = next++;
            stack.pop_back();
        }
    }
    if (next != t.n()) throw std::runtime_error("tree does not reach every node");
    return label;
}

// Post-order DFS that enters a child only through its last incoming edge, so a shared
// child ends up labelled under the parent we reach last.
inline std::vector<Node> last_parent_labels(const Csr& g) {
    std::vector<std::uint32_t> left(g.n(), 0);
    for (Node c : g.dst) ++left[c];
    std::vector<Node> label(g.n(), 0);
    std::vector<std::pair<Node, std::uint64_t>> stack{{0, g.off[0]}};
    Node next = 0;
    while (!stack.empty()) {
        const Node v = stack.back().first;
        if (stack.back().second < g.off[v + 1]) {
            const Node c = g.dst[stack.back().second++];
            if (--left[c] == 0) stack.push_back({c, g.off[c]});
        } else {
            label[v] = next++;
            stack.pop_back();
        }
    }
    if (next != g.n()) throw std::runtime_error("not every node was reached");
    return label;
}

// "dfs" is the default everywhere else: a shared child goes under the first parent reached.
// "primary" labels each node under its primary-tree parent (needs the generator's tree).
// "dominator" labels each node under its immediate dominator.
inline std::vector<Node> labels_for(const std::string& order, const Csr& g, Node b) {
    if (order == "dfs") return postorder_labels(g);
    if (order == "last") return last_parent_labels(g);
    Edges tree;
    if (order == "primary") {
        for (Node v = 1; v < g.n(); ++v) tree.push_back({primary_parent(v, b), v});
    } else if (order == "dominator") {
        const Dominators d = dominators(g);
        for (Node v = 1; v < g.n(); ++v) tree.push_back({d.idom[v], v});
    } else {
        throw std::invalid_argument("unknown label order: " + order);
    }
    return tree_postorder(from_edges(g.n(), std::move(tree)));
}
}  // namespace lab
