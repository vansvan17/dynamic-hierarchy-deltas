#include "delta/scalable.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

[[nodiscard]] std::uint64_t peak_rss_bytes() {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0U;
    }
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#else
    return 0U;
#endif
}

[[nodiscard]] delta::scalable::Locality parse_locality(const std::string& value) {
    if (value == "global") {
        return delta::scalable::Locality::global;
    }
    if (value == "local") {
        return delta::scalable::Locality::same_top_level_branch;
    }
    throw std::invalid_argument("locality must be 'global' or 'local'");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::size_t node_count = argc > 1 ? std::stoull(argv[1]) : 1'000'000U;
        const double update_ratio = argc > 2 ? std::stod(argv[2]) : 0.01;
        const std::string locality_name = argc > 3 ? argv[3] : "global";
        const std::uint64_t seed = argc > 4 ? std::stoull(argv[4]) : 17U;
        const std::string validation_name = argc > 5 ? argv[5] : "full";
        const std::size_t repeats = argc > 6 ? std::stoull(argv[6]) : 5U;
        const std::string sharing_name = argc > 7 ? argv[7] : "local-sharing";
        if (update_ratio <= 0.0 || update_ratio > 0.10) {
            throw std::invalid_argument("update ratio must be in (0, 0.10]");
        }
        const auto locality = parse_locality(locality_name);
        if (validation_name != "full" && validation_name != "ordered") {
            throw std::invalid_argument("validation must be 'full' or 'ordered'");
        }
        if (repeats == 0U) {
            throw std::invalid_argument("repeats must be positive");
        }
        if (sharing_name != "local-sharing" && sharing_name != "cross-sharing") {
            throw std::invalid_argument("sharing must be 'local-sharing' or 'cross-sharing'");
        }
        constexpr std::uint32_t branching_factor = 4U;
        const std::size_t update_count =
            static_cast<std::size_t>(std::ceil(static_cast<double>(node_count) * update_ratio));
        const std::size_t additions = update_count / 2U;
        const std::size_t deletions = update_count - additions;

        const auto build_begin = Clock::now();
        const auto base = delta::scalable::make_tree_dag(
            {node_count, branching_factor, 8U, seed, sharing_name == "local-sharing"});
        const auto build_end = Clock::now();

        const auto dominator_begin = Clock::now();
        const auto dominators = delta::scalable::compute_dominator_tree_ordered(base);
        const auto dominator_end = Clock::now();

        const auto batch_begin = Clock::now();
        const auto batch = delta::scalable::make_update_batch(
            base, additions, deletions, branching_factor, locality, seed + 1U);
        const auto batch_end = Clock::now();
        const auto stats =
            delta::scalable::measure_dominator_stats(batch, dominators, branching_factor);

        std::vector<double> apply_samples;
        apply_samples.reserve(repeats);
        delta::scalable::ApplyResult result;
        for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
            const auto apply_begin = Clock::now();
            result = delta::scalable::apply_batch(
                base, batch, {.assume_node_order_is_topological = validation_name == "ordered"});
            const auto apply_end = Clock::now();
            if (!result.accepted) {
                std::cerr << "batch rejected: " << result.reason << '\n';
                return 2;
            }
            apply_samples.push_back(milliseconds(apply_begin, apply_end));
        }
        std::sort(apply_samples.begin(), apply_samples.end());
        const double apply_ms = apply_samples[apply_samples.size() / 2U];

        const std::uint64_t batch_bytes =
            static_cast<std::uint64_t>(batch.additions.capacity() + batch.deletions.capacity()) *
            sizeof(delta::Edge);
        const double updates_per_second =
            apply_ms == 0.0 ? 0.0 : 1000.0 * static_cast<double>(update_count) / apply_ms;
        const double root_edge_fraction = stats.mutation_count == 0U
                                              ? 0.0
                                              : static_cast<double>(stats.root_edge_lca_count) /
                                                    static_cast<double>(stats.mutation_count);

        std::cout << std::fixed << std::setprecision(3);
        std::cout << "nodes,base_edges,updates,update_ratio,additions,deletions,locality,"
                     "sharing,validation,repeats,"
                     "build_ms,dominator_build_ms,batch_generation_ms,apply_ms,"
                     "updates_per_second,base_storage_bytes,result_storage_bytes,"
                     "dominator_storage_bytes,batch_storage_bytes,"
                     "peak_rss_bytes,whole_batch_lca,whole_batch_selects_root,"
                     "root_edge_lca_fraction,distinct_edge_lcas,distinct_top_groups\n";
        std::cout << node_count << ',' << base.edge_count() << ',' << update_count << ','
                  << update_ratio << ',' << additions << ',' << deletions << ',' << locality_name
                  << ',' << sharing_name << ',' << validation_name << ',' << repeats << ','
                  << milliseconds(build_begin, build_end) << ','
                  << milliseconds(dominator_begin, dominator_end) << ','
                  << milliseconds(batch_begin, batch_end) << ',' << apply_ms << ','
                  << updates_per_second << ',' << base.storage_bytes() << ','
                  << result.graph.storage_bytes() << ',' << dominators.storage_bytes() << ','
                  << batch_bytes << ',' << peak_rss_bytes() << ',' << stats.whole_batch_lca << ','
                  << (stats.whole_batch_lca == 0U ? 1 : 0) << ',' << root_edge_fraction << ','
                  << stats.distinct_edge_lcas << ',' << stats.distinct_top_level_groups << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        std::cerr << "usage: delta_scale_experiment [nodes] [ratio<=0.10] "
                     "[global|local] [seed] [full|ordered] [repeats] "
                     "[local-sharing|cross-sharing]\n";
        return 1;
    }
}
