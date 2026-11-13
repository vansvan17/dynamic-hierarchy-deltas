// How many disjoint label ranges would represent a node's descendants exactly, by primary-tree
// level: the k a node would need for zero false conflicts. For a sample of nodes per level it
// collects the post-order labels of all descendants, sorts them and counts maximal runs, on G
// and on G' after one mixed batch (labels of G kept, and fresh labels on G').
//
// Build: c++ -std=c++20 -O3 -I. scripts/range_count.cpp -o build/range_count
// Run:   ./build/range_count NODES PER_LEVEL > docs/RANGE_COUNT_<n>.csv
#include "dh.hpp"
#include <cstdio>
#include <string>
using namespace dh;

static std::size_t runs(const Csr& g, const std::vector<Node>& label, Node u, std::vector<char>& seen,
                        std::vector<Node>& got) {
    got.clear();
    std::vector<Node> stack{u};
    seen[u] = 1;
    while (!stack.empty()) {
        const Node x = stack.back();
        stack.pop_back();
        got.push_back(x);
        for (Node c : g.kids(x))
            if (!seen[c]) seen[c] = 1, stack.push_back(c);
    }
    for (Node x : got) seen[x] = 0;
    std::vector<Node> ls;
    ls.reserve(got.size());
    for (Node x : got) ls.push_back(label[x]);
    std::sort(ls.begin(), ls.end());
    std::size_t r = 0;
    for (std::size_t i = 0; i < ls.size(); ++i) r += i == 0 || ls[i] != ls[i - 1] + 1;
    return r;
}

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoul(argv[1]) : 1000000;
    const std::size_t per = argc > 2 ? std::stoul(argv[2]) : 20;
    const Node b = 4;
    std::vector<std::uint64_t> s{0};
    for (std::uint64_t w = 1; s.back() + w < n; w *= b) s.push_back(s.back() + w);
    s.push_back(n);
    std::printf("nodes,sharing,update_locality,updates,level,node,descendants_new,ranges_base,ranges_repair_labels,"
                "ranges_fresh_labels\n");
    for (bool in_branch : {true, false}) {
        const Csr g = make_tree_dag(n, b, 8, 17, in_branch);
        const auto label = postorder_labels(g);
        for (bool local_updates : {false, true}) {
            for (double frac : {0.01, 0.10}) {
                const auto total = static_cast<std::size_t>(frac * static_cast<double>(n));
                const Batch batch = make_update_batch(g, total / 2, total - total / 2, b, local_updates, 101);
                const Applied a = apply_batch(g, batch);
                if (a.error) throw std::runtime_error(a.error);
                const auto fresh = postorder_labels(a.g);
                std::vector<char> seen(n, 0);
                std::vector<Node> got;
                std::mt19937_64 rng(7);
                for (std::size_t l = 1; l + 1 < s.size(); ++l) {
                    const std::size_t width = s[l + 1] - s[l];
                    for (std::size_t i = 0; i < std::min<std::size_t>(per, width); ++i) {
                        const Node u = static_cast<Node>(s[l] + (width <= per ? i : rng() % width));
                        const std::size_t rb = runs(g, label, u, seen, got);
                        const std::size_t rr = runs(a.g, label, u, seen, got);
                        const std::size_t d = got.size();
                        const std::size_t rf = runs(a.g, fresh, u, seen, got);
                        std::printf("%zu,%s,%s,%.2f,%zu,%u,%zu,%zu,%zu,%zu\n", n, in_branch ? "local" : "cross",
                                    local_updates ? "local" : "global", frac, l, u, d, rb, rr, rf);
                    }
                }
                std::fflush(stdout);
            }
        }
    }
}
