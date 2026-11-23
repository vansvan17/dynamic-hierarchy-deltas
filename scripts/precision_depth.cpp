// False conflicts by depth. Uniform pairs are nearly all deep nodes, so here each endpoint
// picks a level first, then a node in it. Results are grouped by the shallower endpoint's
// level, as raw counts, so seeds can be pooled.
//
// States: base, repair, relabel. Mixed batches only.
//
// Build: c++ -std=c++20 -O3 -I. scripts/precision_depth.cpp -o build/precision_depth
// Run:   ./build/precision_depth NODES SEEDS PAIRS [global|local] > docs/PRECISION_DEPTH_<n>.csv
//        The last argument says where batch additions go: anywhere, or inside one top-level branch.
#include "scripts/lab.hpp"
#include <chrono>
using namespace lab;

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoul(argv[1]) : 1000000;
    const std::size_t seeds = argc > 2 ? std::stoul(argv[2]) : 3;
    const std::size_t npairs = argc > 3 ? std::stoul(argv[3]) : 20000;
    const bool local_updates = argc > 4 && std::string(argv[4]) == "local";
    const Node b = 4;
    const Levels lv(n, b);
    std::printf("nodes,sharing,updates,update_locality,pairs,k,seed,level,pairs_at_level,disjoint_g,disjoint_new,fc_base,"
                "fc_repair,fc_relabel,false_negatives\n");
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
                    pairs.push_back(level_pairs(lv, b, npairs, local_pairs, 1000 + seed));
                    std::vector<char> tg(npairs), tn(npairs);
                    for (std::size_t i = 0; i < npairs; ++i) {
                        tg[i] = og.shared(pairs.back()[i].u, pairs.back()[i].v);
                        tn[i] = on.shared(pairs.back()[i].u, pairs.back()[i].v);
                    }
                    truth_g.push_back(std::move(tg));
                    truth_n.push_back(std::move(tn));
                }
                for (std::size_t k : {1, 2, 4, 8}) {
                    const auto base = build_sets(g, label, k);
                    auto rep = base;
                    repair(a.g, parents, order, batch, label, rep, k);
                    const auto fresh = build_sets(a.g, relabel, k);
                    for (std::size_t m = 0; m < pairs.size(); ++m) {
                        struct Bucket {
                            std::size_t pairs = 0, dg = 0, dn = 0, fb = 0, fr = 0, fl = 0, fn = 0;
                        };
                        std::vector<Bucket> by(lv.deepest() + 2);
                        for (std::size_t i = 0; i < npairs; ++i) {
                            const Pair p = pairs[m][i];
                            Bucket& x = by[std::min(lv.of(p.u), lv.of(p.v))];
                            ++x.pairs;
                            const bool ob = overlaps(base[p.u], base[p.v]), orp = overlaps(rep[p.u], rep[p.v]),
                                       ol = overlaps(fresh[p.u], fresh[p.v]);
                            if (!truth_g[m][i]) ++x.dg, x.fb += ob;
                            else x.fn += !ob;
                            if (!truth_n[m][i]) ++x.dn, x.fr += orp, x.fl += ol;
                            else x.fn += !orp + !ol;
                        }
                        for (int l = 1; l < static_cast<int>(by.size()); ++l)
                            if (by[l].pairs)
                                std::printf("%zu,%s,%.2f,%s,%s,%zu,%llu,%d,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n", n,
                                            in_branch ? "local" : "cross", frac, local_updates ? "local" : "global",
                                            m ? "local" : "global", k, (unsigned long long)seed, l, by[l].pairs, by[l].dg,
                                            by[l].dn, by[l].fb, by[l].fr, by[l].fl, by[l].fn);
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
