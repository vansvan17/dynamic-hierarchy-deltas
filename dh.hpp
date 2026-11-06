// Batched edge updates on a rooted DAG, plus k-bounded interval-set metadata.
// Exact model (bitmask closure, n <= 32) is the oracle for the sparse CSR engine.
#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dh {

using Node = std::uint32_t;
struct Edge {
    Node from{}, to{};
    auto operator<=>(const Edge&) const = default;
};
using Edges = std::vector<Edge>;  // sorted and unique wherever it matters
struct Batch {
    Edges add, del;
};

inline void sort_unique(Edges& e) {
    std::sort(e.begin(), e.end());
    e.erase(std::unique(e.begin(), e.end()), e.end());
}
inline bool has(const Edges& e, Edge x) { return std::binary_search(e.begin(), e.end(), x); }

// ---- Exact model: reachability as one bitmask row per node (paths of length >= 1).
using Rows = std::vector<std::uint32_t>;

inline Rows closure(std::size_t n, const Edges& edges) {
    Rows r(n, 0);
    for (Edge e : edges) r[e.from] |= 1U << e.to;
    for (std::size_t k = 0; k < n; ++k)
        for (std::size_t i = 0; i < n; ++i)
            if (r[i] >> k & 1U) r[i] |= r[k];
    return r;
}
inline bool acyclic(const Rows& r) {
    for (std::size_t i = 0; i < r.size(); ++i)
        if (r[i] >> i & 1U) return false;
    return true;
}
inline Edges candidate(const Edges& base, Batch b) {
    sort_unique(b.del);
    Edges out;
    std::set_difference(base.begin(), base.end(), b.del.begin(), b.del.end(), std::back_inserter(out));
    out.insert(out.end(), b.add.begin(), b.add.end());
    sort_unique(out);
    return out;
}
// Additions are new, in range and not self-loops; deletions exist; no edge is both.
inline bool preconditions(std::size_t n, const Edges& base, Batch b) {
    sort_unique(b.add);
    sort_unique(b.del);
    for (Edge e : b.add)
        if (e.from >= n || e.to >= n || e.from == e.to || has(base, e) || has(b.del, e)) return false;
    for (Edge e : b.del)
        if (!has(base, e)) return false;
    return true;
}
inline std::optional<Edges> apply_exact(std::size_t n, const Edges& base, const Batch& b) {
    if (!preconditions(n, base, b)) return std::nullopt;
    Edges out = candidate(base, b);
    if (!acyclic(closure(n, out))) return std::nullopt;
    return out;
}
// Combines two independently prepared batches; fails if they mutate a common edge.
inline std::optional<Batch> merge(const Batch& l, const Batch& r) {
    for (const Edges* x : {&l.add, &l.del})
        for (Edge e : *x)
            if (std::count(r.add.begin(), r.add.end(), e) || std::count(r.del.begin(), r.del.end(), e))
                return std::nullopt;
    Batch m = l;
    m.add.insert(m.add.end(), r.add.begin(), r.add.end());
    m.del.insert(m.del.end(), r.del.begin(), r.del.end());
    return m;
}
// Base closure, minus what either private delta removed, plus what either added.
inline Rows naive_union(std::size_t n, const Edges& base, const Batch& l, const Batch& r) {
    const Rows old = closure(n, base);
    Rows added(n, 0), removed(n, 0), out(n);
    for (const Batch* b : {&l, &r}) {
        const Rows c = closure(n, candidate(base, *b));
        for (std::size_t i = 0; i < n; ++i) added[i] |= c[i] & ~old[i], removed[i] |= old[i] & ~c[i];
    }
    for (std::size_t i = 0; i < n; ++i) out[i] = (old[i] & ~removed[i]) | added[i];
    return out;
}
// Ancestors of each mutated edge's source and descendants of its target, before and after.
inline std::uint32_t affected(std::size_t n, const Edges& base, const Batch& b) {
    std::uint32_t mask = 0;
    for (const Edges& g : {base, candidate(base, b)}) {
        const Rows r = closure(n, g);
        for (const Edges* x : {&b.add, &b.del})
            for (Edge e : *x) {
                mask |= 1U << e.from | 1U << e.to | r[e.to];
                for (std::size_t i = 0; i < n; ++i)
                    if (r[i] >> e.from & 1U) mask |= 1U << i;
            }
    }
    return mask;
}

// ---- Sparse engine: compressed sparse rows, children sorted per node.
struct Csr {
    std::vector<std::uint64_t> off{0};
    std::vector<Node> dst;
    std::size_t n() const { return off.size() - 1; }
    std::span<const Node> kids(Node v) const { return {dst.data() + off[v], dst.data() + off[v + 1]}; }
    bool has(Edge e) const {
        auto k = kids(e.from);
        return std::binary_search(k.begin(), k.end(), e.to);
    }
    std::uint64_t bytes() const { return off.size() * sizeof(off[0]) + dst.size() * sizeof(Node); }
};

inline Csr from_edges(std::size_t n, Edges edges) {
    sort_unique(edges);
    Csr g;
    g.off.assign(n + 1, 0);
    for (Edge e : edges) {
        if (e.from >= n || e.to >= n) throw std::invalid_argument("edge endpoint out of range");
        ++g.off[e.from + 1];
        g.dst.push_back(e.to);
    }
    std::partial_sum(g.off.begin(), g.off.end(), g.off.begin());
    return g;
}
inline Csr reverse(const Csr& g) {
    Csr r;
    r.off.assign(g.n() + 1, 0);
    for (Node t : g.dst) ++r.off[t + 1];
    std::partial_sum(r.off.begin(), r.off.end(), r.off.begin());
    r.dst.resize(g.dst.size());
    auto cursor = r.off;
    for (Node v = 0; v < g.n(); ++v)
        for (Node c : g.kids(v)) r.dst[cursor[c]++] = v;
    return r;
}
// Kahn's algorithm; the order is shorter than n exactly when the graph has a cycle.
inline std::vector<Node> topo_order(const Csr& g) {
    std::vector<std::uint32_t> indegree(g.n(), 0);
    for (Node t : g.dst) ++indegree[t];
    std::vector<Node> order;
    order.reserve(g.n());
    for (Node v = 0; v < g.n(); ++v)
        if (indegree[v] == 0) order.push_back(v);
    for (std::size_t i = 0; i < order.size(); ++i)
        for (Node c : g.kids(order[i]))
            if (--indegree[c] == 0) order.push_back(c);
    return order;
}
inline bool rooted(const Csr& g, Node root = 0) {
    std::vector<char> seen(g.n(), 0);
    std::vector<Node> stack{root};
    seen[root] = 1;
    std::size_t count = 1;
    while (!stack.empty()) {
        Node v = stack.back();
        stack.pop_back();
        for (Node c : g.kids(v))
            if (!seen[c]) seen[c] = 1, ++count, stack.push_back(c);
    }
    return count == g.n();
}

struct Applied {
    Csr g;
    const char* error = nullptr;
};
// Applies the whole batch in one merge, then checks acyclicity and (optionally) rootedness.
inline Applied apply_batch(const Csr& base, Batch b, bool require_rooted = true) {
    sort_unique(b.add);
    sort_unique(b.del);
    for (Edge e : b.add)
        if (e.from >= base.n() || e.to >= base.n() || e.from == e.to || base.has(e) || has(b.del, e))
            return {{}, "invalid addition"};
    for (Edge e : b.del)
        if (e.from >= base.n() || e.to >= base.n() || !base.has(e)) return {{}, "invalid deletion"};
    Csr g;
    g.off.reserve(base.n() + 1);
    g.dst.reserve(base.dst.size() + b.add.size());
    auto a = b.add.begin();
    auto d = b.del.begin();
    for (Node v = 0; v < base.n(); ++v) {
        for (Node c : base.kids(v)) {
            for (; a != b.add.end() && a->from == v && a->to < c; ++a) g.dst.push_back(a->to);
            if (d != b.del.end() && *d == Edge{v, c}) ++d;
            else g.dst.push_back(c);
        }
        for (; a != b.add.end() && a->from == v; ++a) g.dst.push_back(a->to);
        g.off.push_back(g.dst.size());
    }
    if (topo_order(g).size() != g.n()) return {{}, "batch creates a cycle"};
    if (require_rooted && !rooted(g)) return {{}, "batch leaves a node unreachable from the root"};
    return {std::move(g)};
}

// ---- Dominator tree of a DAG whose node order is topological, rooted at 0.
struct Dominators {
    std::vector<Node> idom;
    std::vector<std::uint32_t> depth;
    Node lca(Node a, Node b) const {
        while (depth[a] > depth[b]) a = idom[a];
        while (depth[b] > depth[a]) b = idom[b];
        while (a != b) a = idom[a], b = idom[b];
        return a;
    }
};
// The immediate dominator of a node is the dominator-tree LCA of its predecessors.
inline Dominators dominators(const Csr& g) {
    const Csr preds = reverse(g);
    Dominators t{std::vector<Node>(g.n(), 0), std::vector<std::uint32_t>(g.n(), 0)};
    for (Node v = 1; v < g.n(); ++v) {
        auto p = preds.kids(v);
        if (p.empty() || p.back() >= v) throw std::invalid_argument("needs root 0, forward edges, all reachable");
        Node common = p[0];
        for (Node u : p) common = t.lca(common, u);
        t.idom[v] = common;
        t.depth[v] = t.depth[common] + 1;
    }
    return t;
}

// ---- Synthetic hierarchy: b-ary tree plus an extra (earlier) parent for every eighth node.
inline Node primary_parent(Node v, Node b) { return v == 0 ? 0 : (v - 1) / b; }
inline Node top_branch(Node v, Node b) {
    while (v != 0 && primary_parent(v, b) != 0) v = primary_parent(v, b);
    return v;
}
inline std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}
inline Csr make_tree_dag(std::size_t n, Node b, Node extra_every, std::uint64_t seed, bool sharing_in_branch) {
    Edges edges;
    edges.reserve(n + (extra_every ? n / extra_every : 0));
    for (Node v = 1; v < n; ++v) {
        const Node p = primary_parent(v, b);
        edges.push_back({p, v});
        if (extra_every == 0 || v < 2 || v % extra_every != 0) continue;
        for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
            const auto u = static_cast<Node>(mix(seed + v + attempt) % v);
            if (u != p && (!sharing_in_branch || top_branch(u, b) == top_branch(v, b))) {
                edges.push_back({u, v});
                break;
            }
        }
    }
    return from_edges(n, std::move(edges));
}
// Additions are forward (never a cycle). A deletion removes the primary parent edge only of
// a node with a second parent, at most once per node, so every node stays reachable.
inline Batch make_update_batch(const Csr& g, std::size_t adds, std::size_t dels, Node b, bool local,
                               std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto any = [&] { return static_cast<Node>(1 + rng() % (g.n() - 1)); };
    std::unordered_set<std::uint64_t> used;
    auto fresh = [&](Edge e) { return used.insert(std::uint64_t{e.from} << 32 | e.to).second; };
    Batch batch;
    for (std::size_t tries = 0; batch.add.size() < adds; ++tries) {
        if (tries > adds * 100 + 10000) throw std::runtime_error("could not generate additions");
        Node x = any(), y = any();
        for (int retry = 0; local && retry < 12 && top_branch(y, b) != top_branch(x, b); ++retry) y = any();
        const Edge e{std::min(x, y), std::max(x, y)};
        if ((!local || top_branch(x, b) == top_branch(y, b)) && x != y && !g.has(e) && fresh(e))
            batch.add.push_back(e);
    }
    const Csr parents = reverse(g);
    std::vector<char> touched(g.n(), 0);
    for (std::size_t tries = 0; batch.del.size() < dels; ++tries) {
        if (tries > dels * 100 + 10000) throw std::runtime_error("could not generate deletions");
        const Node v = any();
        const Edge e{primary_parent(v, b), v};
        if (parents.kids(v).size() >= 2 && !touched[v] && fresh(e)) touched[v] = 1, batch.del.push_back(e);
    }
    sort_unique(batch.add);
    sort_unique(batch.del);
    return batch;
}

// ---- Interval sets: at most k disjoint, non-adjacent ranges of point labels per node.
using Range = std::pair<Node, Node>;
using Set = std::vector<Range>;

// Joins overlapping or adjacent ranges (no label is added), then merges the pair with the
// smallest gap until at most k ranges remain.
inline Set compress(Set s, std::size_t k) {
    std::sort(s.begin(), s.end());
    Set out;
    for (Range r : s) {
        if (!out.empty() && r.first <= out.back().second + 1) out.back().second = std::max(out.back().second, r.second);
        else out.push_back(r);
    }
    while (out.size() > k) {
        std::size_t best = 0;
        for (std::size_t i = 1; i + 1 < out.size(); ++i)
            if (out[i + 1].first - out[i].second < out[best + 1].first - out[best].second) best = i;
        out[best].second = out[best + 1].second;
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(best) + 1);
    }
    return out;
}
inline bool overlaps(const Set& a, const Set& b) {
    for (std::size_t i = 0, j = 0; i < a.size() && j < b.size();) {
        if (a[i].second < b[j].first) ++i;
        else if (b[j].second < a[i].first) ++j;
        else return true;
    }
    return false;
}
inline bool covers(const Set& a, const Set& b) {
    std::size_t i = 0;
    for (Range r : b) {
        while (i < a.size() && a[i].second < r.first) ++i;
        if (i == a.size() || a[i].first > r.first || a[i].second < r.second) return false;
    }
    return true;
}

// Every node owns one immutable point label: its DFS post-order number.
inline std::vector<Node> postorder_labels(const Csr& g) {
    std::vector<Node> label(g.n(), 0);
    std::vector<char> seen(g.n(), 0);
    std::vector<std::pair<Node, std::uint64_t>> stack;
    Node next = 0;
    for (Node s = 0; s < g.n(); ++s) {
        if (seen[s]) continue;
        seen[s] = 1;
        stack.push_back({s, g.off[s]});
        while (!stack.empty()) {
            const Node v = stack.back().first;
            if (stack.back().second < g.off[v + 1]) {
                const Node c = g.dst[stack.back().second++];
                if (!seen[c]) seen[c] = 1, stack.push_back({c, g.off[c]});
            } else {
                label[v] = next++;
                stack.pop_back();
            }
        }
    }
    return label;
}
// Local subsumption: own label covered, every child's set covered, at most k ranges.
inline bool invariant_holds(const Csr& g, const std::vector<Node>& label, const std::vector<Set>& sets,
                            std::size_t k) {
    for (Node v = 0; v < g.n(); ++v) {
        if (sets[v].size() > k || !covers(sets[v], {{label[v], label[v]}})) return false;
        for (Node c : g.kids(v))
            if (!covers(sets[v], sets[c])) return false;
    }
    return true;
}
inline Set gather(const Csr& g, const std::vector<Set>& sets, Node v, Set start, std::size_t k) {
    for (Node c : g.kids(v)) start.insert(start.end(), sets[c].begin(), sets[c].end());
    return compress(std::move(start), k);
}
// Full rebuild from the labels, children before parents.
inline std::vector<Set> build_sets(const Csr& g, const std::vector<Node>& label, std::size_t k) {
    const auto order = topo_order(g);
    std::vector<Set> sets(g.n());
    for (auto v = order.rbegin(); v != order.rend(); ++v) sets[*v] = gather(g, sets, *v, {{label[*v], label[*v]}}, k);
    return sets;
}

struct RepairStats {
    std::size_t visited = 0, changed = 0;
};
// One pass over the batch's ancestors in G', children before parents. Sources of deleted
// edges, and parents of a node that lost coverage, recompute from their label and children;
// every other visited node keeps its set and absorbs its children's. A node whose set is
// unchanged stops propagation: its parents already cover it.
inline RepairStats repair(const Csr& g, const Csr& parents, const std::vector<Node>& order, const Batch& b,
                          const std::vector<Node>& label, std::vector<Set>& sets, std::size_t k) {
    std::vector<std::uint32_t> rank(g.n());
    for (std::size_t i = 0; i < order.size(); ++i) rank[order[i]] = static_cast<std::uint32_t>(i);
    std::vector<char> full(g.n(), 0), queued(g.n(), 0);
    std::priority_queue<std::pair<std::uint32_t, Node>> queue;
    auto push = [&](Node v) {
        if (!queued[v]) queued[v] = 1, queue.push({rank[v], v});
    };
    for (Edge e : b.del) full[e.from] = 1, push(e.from);
    for (Edge e : b.add) push(e.from);
    RepairStats stats;
    while (!queue.empty()) {
        const Node v = queue.top().second;
        queue.pop();
        ++stats.visited;
        Set s = gather(g, sets, v, full[v] ? Set{{label[v], label[v]}} : sets[v], k);
        if (s == sets[v]) continue;
        const bool lost = !covers(s, sets[v]);
        sets[v] = std::move(s);
        ++stats.changed;
        for (Node p : parents.kids(v)) {
            if (lost) full[p] = 1;
            push(p);
        }
    }
    return stats;
}

}  // namespace dh
