// False conflicts at million scale, with exact answers for sampled pairs only. Storing every
// node's descendants does not fit here, so each sampled pair (u, v) is answered by marking the
// descendants of u and then searching from v until it meets a marked node. That is exact for
// the pair, and costs the size of the two descendant sets.
//
// States, as in precision.cpp: base (sets on G), repair (one batch pass, labels of G),
// rebuild (sets on G' from the labels of G), relabel (fresh labels on G'). The eager variant
// is left out: one repair per update means 10^4..10^6 full passes at this size.
//
// Build: c++ -std=c++20 -O3 -I. scripts/precision_large.cpp -o build/precision_large
// Run:   ./build/precision_large NODES SEEDS PAIRS > docs/PRECISION_LARGE.csv
#include "dh.hpp"
#include <chrono>
#include <cstdio>
using namespace dh;

namespace {
// Exact "do u and v share a descendant?" using a stamped visited array.
struct Oracle {
    const Csr& g;
    std::vector<std::uint32_t> mark;
    std::uint32_t stamp = 0;
    std::vector<Node> stack;
    std::uint64_t work = 0;
    explicit Oracle(const Csr& graph) : g(graph), mark(graph.n(), 0) {}
    bool shared(Node u, Node v) {
        ++stamp;
        stack.assign(1, u);
        mark[u] = stamp;
        while (!stack.empty()) {
            const Node x = stack.back();
            stack.pop_back();
            ++work;
            for (Node c : g.kids(x))
                if (mark[c] != stamp) mark[c] = stamp, stack.push_back(c);
        }
        // Second search from v uses stamp+1 for its own visited set; stop at a node marked by u.
        const std::uint32_t own = ++stamp;
        if (mark[v] == own - 1) return true;
        stack.assign(1, v);
        mark[v] = own;
        while (!stack.empty()) {
            const Node x = stack.back();
            stack.pop_back();
            ++work;
            for (Node c : g.kids(x)) {
                if (mark[c] == own - 1) return true;
                if (mark[c] != own) mark[c] = own, stack.push_back(c);
            }
        }
        return false;
    }
};

struct Pair {
    Node u, v;
};
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
double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}
}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoul(argv[1]) : 1000000;
    const std::size_t seeds = argc > 2 ? std::stoul(argv[2]) : 2;
    const std::size_t npairs = argc > 3 ? std::stoul(argv[3]) : 100000;
    const Node b = 4;
    const std::vector<std::size_t> ks{1, 2, 4, 8};
    std::printf("nodes,sharing,updates,kind,pairs,k,seed,sampled,disjoint_fraction,fc_base,fc_repair,fc_rebuild,"
                "fc_relabel,false_negatives,repair_changed,oracle_work_per_pair\n");
    for (bool in_branch : {true, false}) {
        const Csr g = make_tree_dag(n, b, 8, 17, in_branch);
        const auto label = postorder_labels(g);
        for (double frac : {0.01, 0.10}) {
            for (int kind = 0; kind < 2; ++kind) {  // 0 mixed, 1 additions only
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

                    // Ground truth for every pair on G and on G', computed once for all k.
                    std::vector<std::vector<Pair>> pairs;
                    std::vector<std::vector<char>> truth_g, truth_n;
                    Oracle og(g), on(a.g);
                    for (bool local_pairs : {false, true}) {
                        pairs.push_back(sample_pairs(n, b, npairs, local_pairs, 1000 + seed));
                        std::vector<char> tg(npairs), tn(npairs);
                        for (std::size_t i = 0; i < npairs; ++i) {
                            tg[i] = og.shared(pairs.back()[i].u, pairs.back()[i].v);
                            tn[i] = on.shared(pairs.back()[i].u, pairs.back()[i].v);
                        }
                        truth_g.push_back(std::move(tg));
                        truth_n.push_back(std::move(tn));
                    }
                    const double work = double(og.work + on.work) / double(4 * npairs);

                    for (std::size_t k : ks) {
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
                            auto r = [](std::size_t a, std::size_t d) { return d ? double(a) / double(d) : 0.0; };
                            std::printf("%zu,%s,%.2f,%s,%s,%zu,%llu,%zu,%.4f,%.5f,%.5f,%.5f,%.5f,%zu,%zu,%.1f\n", n,
                                        in_branch ? "local" : "cross", frac, kind == 0 ? "mixed" : "additions",
                                        m ? "local" : "global", k, (unsigned long long)seed, npairs,
                                        double(dis_n) / double(npairs), r(fcb, dis_g), r(fcr, dis_n), r(fcn, dis_n),
                                        r(fcl, dis_n), fn, st.changed, work);
                        }
                        std::fflush(stdout);
                    }
                    std::fprintf(stderr, "%s %.2f %s seed %llu: %.1f s\n", in_branch ? "local" : "cross", frac,
                                 kind == 0 ? "mixed" : "additions", (unsigned long long)seed, seconds_since(t0));
                }
            }
        }
    }
}
