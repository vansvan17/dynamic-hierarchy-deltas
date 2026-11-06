// Repair propagation split by update kind: 10M nodes, 100k additions only, then 100k deletions only.
// Build: c++ -std=c++20 -O3 -I. scripts/k_split.cpp -o build/k_split
#include "dh.hpp"
#include <iostream>
using namespace dh;
int main() {
    const Csr g = make_tree_dag(10000000, 4, 8, 17, true);
    const auto label = postorder_labels(g);
    for (int mode = 0; mode < 2; ++mode) {  // 0: 100k additions only, 1: 100k deletions only
        const Batch b = mode == 0 ? make_update_batch(g, 100000, 0, 4, false, 18) : make_update_batch(g, 0, 100000, 4, false, 18);
        const Applied a = apply_batch(g, b);
        const Csr parents = reverse(a.g);
        const auto order = topo_order(a.g);
        for (std::size_t k : {1, 2, 4, 8}) {
            auto sets = build_sets(g, label, k);
            const RepairStats st = repair(a.g, parents, order, b, label, sets, k);
            std::cout << (mode == 0 ? "additions" : "deletions") << " k=" << k << " visited=" << st.visited << " changed=" << st.changed << '\n';
        }
    }
}
