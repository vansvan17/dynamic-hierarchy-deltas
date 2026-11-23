// Can a better label order fix precision near the root? We try four orders (see labels_for
// in lab.hpp: dfs, last, primary, dominator). They differ only in where a shared child gets
// its label.
//
// For each order: false conflicts by level on level-sampled pairs (base graph, after a mixed
// batch with labels kept, and relabelled), plus the median exact range count per level.
//
// Build: c++ -std=c++20 -O3 -I. scripts/label_order.cpp -o build/label_order
// Run:   ./build/label_order NODES SEEDS PAIRS > docs/LABEL_ORDER_1M.csv
#include "scripts/lab.hpp"
#include <chrono>
using namespace lab;

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoul(argv[1]) : 1000000;
    const std::size_t seeds = argc > 2 ? std::stoul(argv[2]) : 2;
    const std::size_t npairs = argc > 3 ? std::stoul(argv[3]) : 10000;
    const Node b = 4;
    const Levels lv(n, b);
    const int levels = lv.deepest();
    std::printf("nodes,sharing,updates,seed,order,state,k,level,disjoint,false_conflicts,false_negatives,median_ranges\n");
    for (bool in_branch : {true, false}) {
        const Csr g = make_tree_dag(n, b, 8, 17, in_branch);
        for (double frac : {0.01, 0.10}) {
            for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
                const auto t0 = std::chrono::steady_clock::now();
                const auto total = static_cast<std::size_t>(frac * static_cast<double>(n));
                const Batch batch = make_update_batch(g, total / 2, total - total / 2, b, false, 100 + seed);
                const Applied a = apply_batch(g, batch);
                if (a.error) throw std::runtime_error(a.error);
                const Csr parents = reverse(a.g);
                const auto topo = topo_order(a.g);

                const auto pairs = level_pairs(lv, b, npairs, false, 1000 + seed);
                std::vector<char> tg(npairs), tn(npairs);
                Oracle og(g), on(a.g);
                for (std::size_t i = 0; i < npairs; ++i) tg[i] = og.shared(pairs[i].u, pairs[i].v), tn[i] = on.shared(pairs[i].u, pairs[i].v);

                // Nodes to count ranges for: up to 20 per level.
                std::vector<std::vector<Node>> sample(levels + 1);
                std::mt19937_64 rs(7);
                for (int l = 1; l <= levels; ++l) {
                    const std::size_t width = lv.width(l);
                    for (std::size_t i = 0; i < std::min<std::size_t>(20, width); ++i)
                        sample[l].push_back(static_cast<Node>(lv.start[l] + (width <= 20 ? i : rs() % width)));
                }

                for (const std::string order : {"dfs", "last", "primary", "dominator"}) {
                    const auto lab_g = labels_for(order, g, b);
                    const auto lab_n = labels_for(order, a.g, b);
                    // Range counts take a while, so only for the first seed.
                    std::vector<std::size_t> med_g(levels + 1), med_n(levels + 1);
                    if (seed == 1)
                        for (int l = 1; l <= levels; ++l) {
                            std::vector<std::size_t> rg, rn;
                            for (Node u : sample[l]) rg.push_back(exact_ranges(g, lab_g, u)), rn.push_back(exact_ranges(a.g, lab_n, u));
                            std::sort(rg.begin(), rg.end()), std::sort(rn.begin(), rn.end());
                            med_g[l] = rg[rg.size() / 2], med_n[l] = rn[rn.size() / 2];
                        }
                    for (std::size_t k : {1, 8}) {
                        const auto base = build_sets(g, lab_g, k);
                        auto rep = base;
                        repair(a.g, parents, topo, batch, lab_g, rep, k);
                        const auto fresh = build_sets(a.g, lab_n, k);
                        struct State {
                            const char* name;
                            const std::vector<Set>& sets;
                            const std::vector<char>& truth;
                            const std::vector<std::size_t>& med;
                        };
                        for (const State& st : {State{"base", base, tg, med_g}, State{"repair", rep, tn, med_g},
                                                State{"relabel", fresh, tn, med_n}}) {
                            std::vector<std::size_t> dis(levels + 1), fc(levels + 1), fn(levels + 1);
                            for (std::size_t i = 0; i < npairs; ++i) {
                                const int l = std::min(lv.of(pairs[i].u), lv.of(pairs[i].v));
                                const bool said = overlaps(st.sets[pairs[i].u], st.sets[pairs[i].v]);
                                if (!st.truth[i]) ++dis[l], fc[l] += said;
                                else fn[l] += !said;
                            }
                            for (int l = 1; l <= levels; ++l)
                                std::printf("%zu,%s,%.2f,%llu,%s,%s,%zu,%d,%zu,%zu,%zu,%zu\n", n, in_branch ? "local" : "cross",
                                            frac, (unsigned long long)seed, order.c_str(), st.name, k, l, dis[l], fc[l], fn[l],
                                            seed == 1 ? st.med[l] : 0);
                        }
                    }
                    std::fflush(stdout);
                }
                std::fprintf(stderr, "%s %.2f seed %llu: %.1f s\n", in_branch ? "local" : "cross", frac,
                             (unsigned long long)seed, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
        }
    }
}
