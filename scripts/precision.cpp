// False conflicts of k-bounded interval sets against exact reachability, on graphs small
// enough for a descendant bitset per node. A pair (u, v) truly conflicts when some node is a
// descendant of both (u and v count as their own descendants). A false conflict is a pair the
// intervals report as overlapping that truly does not conflict; a false negative would be the
// reverse and must never happen.
//
// For each batch it measures four states on the same sampled pairs:
//   base     sets built on G, before the batch;
//   repair   base sets repaired in one pass for the batch (labels of G kept);
//   rebuild  sets rebuilt on G' from the labels of G;
//   relabel  fresh post-order labels on G' and sets built from them;
//   eager    the base sets repaired after every single update, in a random order (labels of G
//            kept), i.e. no delaying across the batch.
// It also reports propagation (nodes changed and visited) for the batch repair and, summed
// over all single-update repairs, for the eager variant.
//
// Build: c++ -std=c++20 -O3 -I. scripts/precision.cpp -o build/precision
// Run:   ./build/precision > docs/PRECISION.csv
#include "dh.hpp"
#include <cstdio>
#include <iostream>
using namespace dh;

namespace {
using Bits = std::vector<std::uint64_t>;

std::vector<Bits> descendants(const Csr& g) {
    const std::size_t w = (g.n() + 63) / 64;
    std::vector<Bits> d(g.n(), Bits(w, 0));
    const auto order = topo_order(g);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const Node v = *it;
        d[v][v / 64] |= 1ULL << (v % 64);
        for (Node c : g.kids(v))
            for (std::size_t i = 0; i < w; ++i) d[v][i] |= d[c][i];
    }
    return d;
}
bool intersect(const Bits& a, const Bits& b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] & b[i]) return true;
    return false;
}

struct Pair {
    Node u, v;
};
// Half the pairs are uniform, half stay inside one top-level branch (the accesses a
// partitioned workload would issue together).
std::vector<Pair> sample_pairs(std::size_t n, Node b, std::size_t count, bool local, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Pair> out;
    while (out.size() < count) {
        const auto u = static_cast<Node>(1 + rng() % (n - 1));
        auto v = static_cast<Node>(1 + rng() % (n - 1));
        for (int r = 0; local && r < 64 && top_branch(v, b) != top_branch(u, b); ++r) v = static_cast<Node>(1 + rng() % (n - 1));
        if (u != v && (!local || top_branch(u, b) == top_branch(v, b))) out.push_back({u, v});
    }
    return out;
}

struct Count {
    std::size_t disjoint = 0, false_conflicts = 0, false_negatives = 0;
};
Count score(const std::vector<Pair>& pairs, const std::vector<Bits>& desc, const std::vector<Set>& sets) {
    Count c;
    for (Pair p : pairs) {
        const bool truth = intersect(desc[p.u], desc[p.v]);
        const bool said = overlaps(sets[p.u], sets[p.v]);
        if (!truth) ++c.disjoint, c.false_conflicts += said;
        else c.false_negatives += !said;
    }
    return c;
}
}  // namespace

int main(int argc, char** argv) {
    const std::size_t seeds = argc > 1 ? std::stoul(argv[1]) : 5;
    const std::size_t npairs = 200000;
    const Node b = 4;
    const std::vector<std::size_t> ks{1, 2, 4, 8};
    std::printf("nodes,sharing,updates,kind,pairs,k,seed,disjoint_fraction,fc_base,fc_repair,fc_rebuild,fc_relabel,"
                "fc_eager,false_negatives,repair_changed,repair_visited,eager_changed,eager_visited\n");
    for (std::size_t n : {4096, 16384}) {
        for (bool in_branch : {true, false}) {
            const Csr g = make_tree_dag(n, b, 8, 17, in_branch);
            const auto label = postorder_labels(g);
            const auto desc_g = descendants(g);
            for (double frac : {0.01, 0.10}) {
                for (int kind = 0; kind < 3; ++kind) {  // 0 mixed, 1 additions only, 2 deletions only
                    const auto total = static_cast<std::size_t>(frac * static_cast<double>(n));
                    const std::size_t adds = kind == 0 ? total / 2 : kind == 1 ? total : 0;
                    const std::size_t dels = kind == 0 ? total - total / 2 : kind == 2 ? total : 0;
                    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
                        const Batch batch = make_update_batch(g, adds, dels, b, false, 100 + seed);
                        const Applied a = apply_batch(g, batch);
                        if (a.error) throw std::runtime_error(a.error);
                        const Csr parents = reverse(a.g);
                        const auto order = topo_order(a.g);
                        const auto desc = descendants(a.g);
                        const auto relabel = postorder_labels(a.g);

                        struct PerK {
                            std::vector<Set> base, rep, rebuild, relabel, eager;
                            RepairStats st, eager_st;
                        };
                        std::vector<PerK> per(ks.size());
                        for (std::size_t i = 0; i < ks.size(); ++i) {
                            const std::size_t k = ks[i];
                            per[i].base = build_sets(g, label, k);
                            per[i].rep = per[i].base;
                            per[i].st = repair(a.g, parents, order, batch, label, per[i].rep, k);
                            if (!invariant_holds(a.g, label, per[i].rep, k)) throw std::runtime_error("invariant broken");
                            per[i].rebuild = build_sets(a.g, label, k);
                            per[i].relabel = build_sets(a.g, relabel, k);
                            per[i].eager = per[i].base;
                        }
                        // Eager: one update at a time in a random order. Intermediate graphs need
                        // not be rooted (an addition may come after the deletion it compensates).
                        std::vector<Batch> steps;
                        for (Edge e : batch.add) steps.push_back({{e}, {}});
                        for (Edge e : batch.del) steps.push_back({{}, {e}});
                        std::shuffle(steps.begin(), steps.end(), std::mt19937_64(500 + seed));
                        Csr cur = g;
                        for (const Batch& one : steps) {
                            Applied s = apply_batch(cur, one, false);
                            if (s.error) throw std::runtime_error(s.error);
                            cur = std::move(s.g);
                            const Csr cp = reverse(cur);
                            const auto co = topo_order(cur);
                            for (std::size_t i = 0; i < ks.size(); ++i) {
                                const RepairStats r = repair(cur, cp, co, one, label, per[i].eager, ks[i]);
                                per[i].eager_st.changed += r.changed;
                                per[i].eager_st.visited += r.visited;
                            }
                        }
                        for (std::size_t i = 0; i < ks.size(); ++i)
                            if (!invariant_holds(a.g, label, per[i].eager, ks[i])) throw std::runtime_error("eager invariant broken");

                        for (bool local_pairs : {false, true}) {
                            const auto pairs = sample_pairs(n, b, npairs, local_pairs, 1000 + seed);
                            for (std::size_t i = 0; i < ks.size(); ++i) {
                                const PerK& p = per[i];
                                const Count cb = score(pairs, desc_g, p.base), cr = score(pairs, desc, p.rep),
                                            cn = score(pairs, desc, p.rebuild), cl = score(pairs, desc, p.relabel),
                                            ce = score(pairs, desc, p.eager);
                                auto rate = [](const Count& c) { return c.disjoint ? double(c.false_conflicts) / double(c.disjoint) : 0.0; };
                                std::printf("%zu,%s,%.2f,%s,%s,%zu,%llu,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%zu,%zu,%zu,%zu,%zu\n", n,
                                            in_branch ? "local" : "cross", frac,
                                            kind == 0 ? "mixed" : kind == 1 ? "additions" : "deletions",
                                            local_pairs ? "local" : "global", ks[i], (unsigned long long)seed,
                                            double(cr.disjoint) / double(npairs), rate(cb), rate(cr), rate(cn), rate(cl), rate(ce),
                                            cb.false_negatives + cr.false_negatives + cn.false_negatives + cl.false_negatives +
                                                ce.false_negatives,
                                            p.st.changed, p.st.visited, p.eager_st.changed, p.eager_st.visited);
                            }
                        }
                    }
                }
            }
        }
    }
}
