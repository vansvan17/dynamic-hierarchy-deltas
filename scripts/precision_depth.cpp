// False conflicts by depth. Uniform pairs are almost all deep nodes with small subtrees, so
// here each endpoint first picks a primary-tree level uniformly (1 .. deepest), then a node
// uniformly within that level. Every pair is answered exactly (mark the descendants of u,
// search from v), and results are bucketed by the level of the shallower endpoint.
//
// States: base (sets on G), repair (one batch pass, labels of G), relabel (fresh labels on G').
// Mixed batches only; precision_large.cpp covers additions-only and the rebuild state.
//
// Build: c++ -std=c++20 -O3 -I. scripts/precision_depth.cpp -o build/precision_depth
// Run:   ./build/precision_depth NODES SEEDS PAIRS [global|local] > docs/PRECISION_DEPTH_<n>.csv
//        (the last argument sets where batch additions go: anywhere, or inside one top-level branch)
// Output: one row per configuration, seed, k and level of the shallower endpoint, with
// counts, so rates can be pooled over seeds.
#include "dh.hpp"
#include <chrono>
#include <cstdio>
#include <string>
using namespace dh;

namespace {
struct Oracle {
    const Csr& g;
    std::vector<std::uint32_t> mark;
    std::uint32_t stamp = 0;
    std::vector<Node> stack;
    explicit Oracle(const Csr& graph) : g(graph), mark(graph.n(), 0) {}
    bool shared(Node u, Node v) {
        const std::uint32_t mu = ++stamp, mv = ++stamp;
        stack.assign(1, u);
        mark[u] = mu;
        while (!stack.empty()) {
            const Node x = stack.back();
            stack.pop_back();
            for (Node c : g.kids(x))
                if (mark[c] != mu) mark[c] = mu, stack.push_back(c);
        }
        if (mark[v] == mu) return true;
        stack.assign(1, v);
        mark[v] = mv;
        while (!stack.empty()) {
            const Node x = stack.back();
            stack.pop_back();
            for (Node c : g.kids(x)) {
                if (mark[c] == mu) return true;
                if (mark[c] != mv) mark[c] = mv, stack.push_back(c);
            }
        }
        return false;
    }
};

// Level of v in the b-ary primary tree (root is level 0) and the first node of each level.
std::vector<std::uint64_t> level_starts(std::size_t n, Node b) {
    std::vector<std::uint64_t> s{0};
    for (std::uint64_t width = 1; s.back() + width < n; width *= b) s.push_back(s.back() + width);
    s.push_back(n);  // sentinel: end of the deepest (possibly partial) level
    return s;
}
int level_of(Node v, const std::vector<std::uint64_t>& s) {
    return static_cast<int>(std::upper_bound(s.begin(), s.end(), std::uint64_t{v}) - s.begin()) - 1;
}

struct Pair {
    Node u, v;
};
std::vector<Pair> sample_pairs(std::size_t count, const std::vector<std::uint64_t>& s, Node b, bool local,
                               std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    const int levels = static_cast<int>(s.size()) - 2;  // levels 1 .. levels (root excluded)
    auto pick = [&] {
        const int l = 1 + static_cast<int>(rng() % static_cast<std::uint64_t>(levels));
        return static_cast<Node>(s[l] + rng() % (s[l + 1] - s[l]));
    };
    std::vector<Pair> out;
    while (out.size() < count) {
        const Node u = pick();
        Node v = pick();
        for (int r = 0; local && r < 256 && top_branch(v, b) != top_branch(u, b); ++r) v = pick();
        if (u != v && (!local || top_branch(u, b) == top_branch(v, b))) out.push_back({u, v});
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoul(argv[1]) : 1000000;
    const std::size_t seeds = argc > 2 ? std::stoul(argv[2]) : 3;
    const std::size_t npairs = argc > 3 ? std::stoul(argv[3]) : 20000;
    const bool local_updates = argc > 4 && std::string(argv[4]) == "local";
    const Node b = 4;
    const std::vector<std::size_t> ks{1, 2, 4, 8};
    const auto starts = level_starts(n, b);
    const int levels = static_cast<int>(starts.size()) - 1;
    std::printf("nodes,sharing,updates,update_locality,pairs,k,seed,level,pairs_at_level,disjoint_g,disjoint_new,fc_base,fc_repair,"
                "fc_relabel,false_negatives\n");
    for (bool in_branch : {true, false}) {
        const Csr g = make_tree_dag(n, b, 8, 17, in_branch);
        const auto label = postorder_labels(g);
        for (double frac : {0.01, 0.10}) {
            const auto total = static_cast<std::size_t>(frac * static_cast<double>(n));
            for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
                const auto t0 = std::chrono::steady_clock::now();
                const Batch batch = make_update_batch(g, total / 2, total - total / 2, b, local_updates, 100 + seed);
                const Applied a = apply_batch(g, batch);
                if (a.error) throw std::runtime_error(a.error);
                const Csr parents = reverse(a.g);
                const auto order = topo_order(a.g);
                const auto relabel = postorder_labels(a.g);

                std::vector<std::vector<Pair>> pairs;
                std::vector<std::vector<char>> truth_g, truth_n;
                Oracle og(g), on(a.g);
                for (bool local_pairs : {false, true}) {
                    pairs.push_back(sample_pairs(npairs, starts, b, local_pairs, 1000 + seed));
                    std::vector<char> tg(npairs), tn(npairs);
                    for (std::size_t i = 0; i < npairs; ++i) {
                        tg[i] = og.shared(pairs.back()[i].u, pairs.back()[i].v);
                        tn[i] = on.shared(pairs.back()[i].u, pairs.back()[i].v);
                    }
                    truth_g.push_back(std::move(tg));
                    truth_n.push_back(std::move(tn));
                }
                for (std::size_t k : ks) {
                    const auto base = build_sets(g, label, k);
                    auto rep = base;
                    repair(a.g, parents, order, batch, label, rep, k);
                    const auto fresh = build_sets(a.g, relabel, k);
                    for (std::size_t m = 0; m < pairs.size(); ++m) {
                        struct Bucket {
                            std::size_t pairs = 0, dg = 0, dn = 0, fb = 0, fr = 0, fl = 0, fn = 0;
                        };
                        std::vector<Bucket> by(levels + 1);
                        for (std::size_t i = 0; i < npairs; ++i) {
                            const Pair p = pairs[m][i];
                            Bucket& x = by[std::min(level_of(p.u, starts), level_of(p.v, starts))];
                            ++x.pairs;
                            const bool ob = overlaps(base[p.u], base[p.v]), orp = overlaps(rep[p.u], rep[p.v]),
                                       ol = overlaps(fresh[p.u], fresh[p.v]);
                            if (!truth_g[m][i]) ++x.dg, x.fb += ob;
                            else x.fn += !ob;
                            if (!truth_n[m][i]) ++x.dn, x.fr += orp, x.fl += ol;
                            else x.fn += !orp + !ol;
                        }
                        for (int l = 1; l <= levels; ++l)
                            if (by[l].pairs)
                                std::printf("%zu,%s,%.2f,%s,%s,%zu,%llu,%d,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n", n,
                                            in_branch ? "local" : "cross", frac, local_updates ? "local" : "global", m ? "local" : "global", k,
                                            (unsigned long long)seed, l, by[l].pairs, by[l].dg, by[l].dn, by[l].fb,
                                            by[l].fr, by[l].fl, by[l].fn);
                    }
                    std::fflush(stdout);
                }
                std::fprintf(stderr, "%s %.2f seed %llu: %.1f s\n", in_branch ? "local" : "cross", frac,
                             (unsigned long long)seed,
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
        }
    }
}
