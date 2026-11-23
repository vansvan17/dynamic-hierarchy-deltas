// How many separate label ranges does a node's set of descendants really take? That is the k
// it would need for zero false conflicts. We sample up to 20 nodes per level and count, on G
// and on G' after one mixed batch (with G's labels, and with fresh ones).
//
// Build: c++ -std=c++20 -O3 -I. scripts/range_count.cpp -o build/range_count
// Run:   ./build/range_count NODES PER_LEVEL > docs/RANGE_COUNT_<n>.csv
#include "scripts/lab.hpp"
using namespace lab;

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoul(argv[1]) : 1000000;
    const std::size_t per = argc > 2 ? std::stoul(argv[2]) : 20;
    const Node b = 4;
    const Levels lv(n, b);
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
                std::mt19937_64 rng(7);
                std::vector<Node> below;
                for (int l = 1; l <= lv.deepest(); ++l) {
                    const std::size_t width = lv.width(l);
                    for (std::size_t i = 0; i < std::min(per, width); ++i) {
                        const Node u = static_cast<Node>(lv.start[l] + (width <= per ? i : rng() % width));
                        const std::size_t before = exact_ranges(g, label, u);
                        const std::size_t kept = exact_ranges(a.g, label, u, &below);
                        const std::size_t relabelled = exact_ranges(a.g, fresh, u);
                        std::printf("%zu,%s,%s,%.2f,%d,%u,%zu,%zu,%zu,%zu\n", n, in_branch ? "local" : "cross",
                                    local_updates ? "local" : "global", frac, l, u, below.size(), before, kept, relabelled);
                    }
                }
                std::fflush(stdout);
            }
        }
    }
}
