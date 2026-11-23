// Same question as precision.cpp, at 1 or 10 million nodes. Too big to store every node's
// descendants, so we only answer the sampled pairs, each one exactly.
//
// States: base, repair, rebuild, relabel (see precision.cpp). No eager variant here: one
// repair per update would mean up to a million full passes.
//
// Build: c++ -std=c++20 -O3 -I. scripts/precision_large.cpp -o build/precision_large
// Run:   ./build/precision_large NODES SEEDS PAIRS > docs/PRECISION_LARGE_<n>.csv
#include "scripts/lab.hpp"
#include <chrono>
using namespace lab;

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoul(argv[1]) : 1000000;
    const std::size_t seeds = argc > 2 ? std::stoul(argv[2]) : 2;
    const std::size_t npairs = argc > 3 ? std::stoul(argv[3]) : 100000;
    const Node b = 4;
    std::printf("nodes,sharing,updates,kind,pairs,k,seed,sampled,disjoint_fraction,fc_base,fc_repair,fc_rebuild,"
                "fc_relabel,false_negatives,repair_changed,oracle_work_per_pair\n");
    for (bool in_branch : {true, false}) {
        const Csr g = make_tree_dag(n, b, 8, 17, in_branch);
        const auto label = postorder_labels(g);
        for (double frac : {0.01, 0.10}) {
            for (int kind = 0; kind < 2; ++kind) {  // mixed, additions only
                const auto total = static_cast<std::size_t>(frac * static_cast<double>(n));
                const std::size_t adds = kind == 0 ? total / 2 : total;
                const std::size_t dels = kind == 0 ? total - total / 2 : 0;
                for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
                    const auto t0 = std::chrono::steady_clock::now();
                    const Batch batch = make_update_batch(g, adds, dels, b, false, 100 + seed);
                    const Applied a = apply_batch(g, batch);
                    if (a.error) throw std::runtime_error(a.error);
                    const Csr parents = reverse(a.g);
                    const auto order = topo_order(a.g);
                    const auto relabel = postorder_labels(a.g);

                    // The true answers don't depend on k, so work them out once.
                    std::vector<std::vector<Pair>> pairs;
                    std::vector<std::vector<char>> truth_g, truth_n;
                    Oracle og(g), on(a.g);
                    for (bool local_pairs : {false, true}) {
                        pairs.push_back(uniform_pairs(n, b, npairs, local_pairs, 1000 + seed));
                        std::vector<char> tg(npairs), tn(npairs);
                        for (std::size_t i = 0; i < npairs; ++i) {
                            tg[i] = og.shared(pairs.back()[i].u, pairs.back()[i].v);
                            tn[i] = on.shared(pairs.back()[i].u, pairs.back()[i].v);
                        }
                        truth_g.push_back(std::move(tg));
                        truth_n.push_back(std::move(tn));
                    }
                    const double work = double(og.work + on.work) / double(4 * npairs);

                    for (std::size_t k : {1, 2, 4, 8}) {
                        const auto base = build_sets(g, label, k);
                        auto rep = base;
                        const RepairStats st = repair(a.g, parents, order, batch, label, rep, k);
                        const auto rebuild = build_sets(a.g, label, k);
                        const auto fresh = build_sets(a.g, relabel, k);
                        for (std::size_t m = 0; m < pairs.size(); ++m) {
                            std::size_t dis_g = 0, dis_n = 0, fcb = 0, fcr = 0, fcn = 0, fcl = 0, fn = 0;
                            for (std::size_t i = 0; i < npairs; ++i) {
                                const Pair p = pairs[m][i];
                                const bool ob = overlaps(base[p.u], base[p.v]), orp = overlaps(rep[p.u], rep[p.v]),
                                           onb = overlaps(rebuild[p.u], rebuild[p.v]), ol = overlaps(fresh[p.u], fresh[p.v]);
                                if (!truth_g[m][i]) ++dis_g, fcb += ob;
                                else fn += !ob;
                                if (!truth_n[m][i]) ++dis_n, fcr += orp, fcn += onb, fcl += ol;
                                else fn += !orp + !onb + !ol;
                            }
                            std::printf("%zu,%s,%.2f,%s,%s,%zu,%llu,%zu,%.4f,%.5f,%.5f,%.5f,%.5f,%zu,%zu,%.1f\n", n,
                                        in_branch ? "local" : "cross", frac, kind == 0 ? "mixed" : "additions",
                                        m ? "local" : "global", k, (unsigned long long)seed, npairs,
                                        double(dis_n) / double(npairs), rate(fcb, dis_g), rate(fcr, dis_n),
                                        rate(fcn, dis_n), rate(fcl, dis_n), fn, st.changed, work);
                        }
                        std::fflush(stdout);
                    }
                    std::fprintf(stderr, "%s %.2f %s seed %llu: %.1f s\n", in_branch ? "local" : "cross", frac,
                                 kind == 0 ? "mixed" : "additions", (unsigned long long)seed,
                                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                }
            }
        }
    }
}
