// How often do the interval sets report a conflict that isn't real? Small graphs (4,096 and
// 16,384 nodes), every pair answered exactly.
//
// Two nodes truly conflict if they share a descendant. A false conflict is a pair whose sets
// overlap anyway. A false negative (sets disjoint, nodes conflict) must never happen.
//
// Each batch is scored five ways on the same pairs:
//   base     sets on G, before the batch
//   repair   base sets after one repair pass for the whole batch (labels of G kept)
//   rebuild  sets rebuilt on G' from the labels of G
//   relabel  fresh labels on G', sets built from those
//   eager    base sets repaired after every single update, in random order
// and we record how many nodes the batch repair and the eager repairs touched.
//
// Build: c++ -std=c++20 -O3 -I. scripts/precision.cpp -o build/precision
// Run:   ./build/precision 5 > docs/PRECISION.csv
#include "scripts/lab.hpp"
using namespace lab;

struct Count {
    std::size_t disjoint = 0, false_conflicts = 0, false_negatives = 0;
};
static Count score(const std::vector<Pair>& pairs, const std::vector<char>& truth, const std::vector<Set>& sets) {
    Count c;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const bool said = overlaps(sets[pairs[i].u], sets[pairs[i].v]);
        if (!truth[i]) ++c.disjoint, c.false_conflicts += said;
        else c.false_negatives += !said;
    }
    return c;
}
static double fc(const Count& c) { return rate(c.false_conflicts, c.disjoint); }

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
            for (double frac : {0.01, 0.10}) {
                for (int kind = 0; kind < 3; ++kind) {  // mixed, additions only, deletions only
                    const auto total = static_cast<std::size_t>(frac * static_cast<double>(n));
                    const std::size_t adds = kind == 0 ? total / 2 : kind == 1 ? total : 0;
                    const std::size_t dels = kind == 0 ? total - total / 2 : kind == 2 ? total : 0;
                    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
                        const Batch batch = make_update_batch(g, adds, dels, b, false, 100 + seed);
                        const Applied a = apply_batch(g, batch);
                        if (a.error) throw std::runtime_error(a.error);
                        const Csr parents = reverse(a.g);
                        const auto order = topo_order(a.g);
                        const auto relabel = postorder_labels(a.g);

                        struct PerK {
                            std::vector<Set> base, rep, rebuild, relabel, eager;
                            RepairStats st, eager_st;
                        };
                        std::vector<PerK> per(ks.size());
                        for (std::size_t i = 0; i < ks.size(); ++i) {
                            PerK& p = per[i];
                            p.base = build_sets(g, label, ks[i]);
                            p.rep = p.eager = p.base;
                            p.st = repair(a.g, parents, order, batch, label, p.rep, ks[i]);
                            if (!invariant_holds(a.g, label, p.rep, ks[i])) throw std::runtime_error("invariant broken");
                            p.rebuild = build_sets(a.g, label, ks[i]);
                            p.relabel = build_sets(a.g, relabel, ks[i]);
                        }

                        // Eager: one update at a time. The graph in between may not be rooted
                        // (an addition can come after the deletion it makes up for), which is fine.
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

                        Oracle before(g), after(a.g);
                        for (bool local_pairs : {false, true}) {
                            const auto pairs = uniform_pairs(n, b, npairs, local_pairs, 1000 + seed);
                            std::vector<char> tg(npairs), tn(npairs);
                            for (std::size_t i = 0; i < npairs; ++i)
                                tg[i] = before.shared(pairs[i].u, pairs[i].v), tn[i] = after.shared(pairs[i].u, pairs[i].v);
                            for (std::size_t i = 0; i < ks.size(); ++i) {
                                const PerK& p = per[i];
                                const Count cb = score(pairs, tg, p.base), cr = score(pairs, tn, p.rep),
                                            cn = score(pairs, tn, p.rebuild), cl = score(pairs, tn, p.relabel),
                                            ce = score(pairs, tn, p.eager);
                                std::printf("%zu,%s,%.2f,%s,%s,%zu,%llu,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%zu,%zu,%zu,%zu,%zu\n", n,
                                            in_branch ? "local" : "cross", frac,
                                            kind == 0 ? "mixed" : kind == 1 ? "additions" : "deletions",
                                            local_pairs ? "local" : "global", ks[i], (unsigned long long)seed,
                                            double(cr.disjoint) / double(npairs), fc(cb), fc(cr), fc(cn), fc(cl), fc(ce),
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
