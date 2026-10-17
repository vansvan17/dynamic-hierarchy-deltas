#include "delta/model.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::size_t nodes = 256;
  std::size_t batch = 32;
  std::size_t iterations = 20;
  double edge_probability = 0.015;
  std::uint64_t seed = 7;
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto next = [&]() -> std::string {
      if (++index >= argc) {
        throw std::invalid_argument("missing value after " + argument);
      }
      return argv[index];
    };
    if (argument == "--nodes") {
      options.nodes = std::stoul(next());
    } else if (argument == "--batch") {
      options.batch = std::stoul(next());
    } else if (argument == "--iterations") {
      options.iterations = std::stoul(next());
    } else if (argument == "--edge-probability") {
      options.edge_probability = std::stod(next());
    } else if (argument == "--seed") {
      options.seed = std::stoull(next());
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  return options;
}

delta::GraphState make_base(const Options& options, std::mt19937_64& random) {
  std::bernoulli_distribution include_edge(options.edge_probability);
  delta::EdgeSet edges;
  for (delta::Node from = 0; from < options.nodes; ++from) {
    for (delta::Node to = from + 1; to < options.nodes; ++to) {
      if (include_edge(random)) {
        edges.insert(delta::Edge{from, to});
      }
    }
  }
  return delta::GraphState::create(options.nodes, std::move(edges));
}

std::vector<delta::UpdateRequest> make_additions(
    const delta::GraphState& base, std::size_t batch_size,
    std::mt19937_64& random) {
  std::vector<delta::Edge> available;
  for (delta::Node from = 0; from < base.node_count; ++from) {
    for (delta::Node to = from + 1; to < base.node_count; ++to) {
      const delta::Edge edge{from, to};
      if (!base.edges.contains(edge)) {
        available.push_back(edge);
      }
    }
  }
  std::shuffle(available.begin(), available.end(), random);
  batch_size = std::min(batch_size, available.size());
  std::vector<delta::UpdateRequest> requests;
  requests.reserve(batch_size);
  for (std::size_t index = 0; index < batch_size; ++index) {
    requests.push_back(delta::UpdateRequest{
        "add-" + std::to_string(index), delta::EdgeSet{available[index]}, {}});
  }
  return requests;
}

template <typename Function>
double time_milliseconds(Function&& function) {
  const auto start = Clock::now();
  function();
  const auto finish = Clock::now();
  return std::chrono::duration<double, std::milli>(finish - start).count();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    std::mt19937_64 random(options.seed);
    double serial_total = 0.0;
    double batch_total = 0.0;
    std::size_t checked = 0;
    std::size_t minimum_initial_edges = std::numeric_limits<std::size_t>::max();
    std::size_t maximum_initial_edges = 0;
    std::size_t total_initial_edges = 0;

    for (std::size_t iteration = 0; iteration < options.iterations;
         ++iteration) {
      const auto base = make_base(options, random);
      const auto requests = make_additions(base, options.batch, random);
      minimum_initial_edges = std::min(minimum_initial_edges, base.edges.size());
      maximum_initial_edges = std::max(maximum_initial_edges, base.edges.size());
      total_initial_edges += base.edges.size();

      delta::GraphState serial_result = base;
      serial_total += time_milliseconds([&] {
        for (const auto& request : requests) {
          const auto prepared = delta::prepare_delta(serial_result, request);
          serial_result = delta::apply_delta(serial_result, prepared);
        }
      });

      delta::GraphState batch_result = base;
      batch_total += time_milliseconds([&] {
        delta::EdgeSet additions;
        for (const auto& request : requests) {
          additions.insert(request.additions.begin(), request.additions.end());
        }
        const auto combined = delta::prepare_delta(
            base, delta::UpdateRequest{"batch", std::move(additions), {}});
        batch_result = delta::apply_delta(base, combined);
      });

      if (serial_result.edges != batch_result.edges ||
          serial_result.reachability != batch_result.reachability) {
        std::cerr << "serial and batched results differ\n";
        return 1;
      }
      ++checked;
    }

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "nodes=" << options.nodes << '\n';
    std::cout << "batch_size=" << options.batch << '\n';
    std::cout << "iterations=" << checked << '\n';
    std::cout << "initial_edges_min=" << minimum_initial_edges << '\n';
    std::cout << "initial_edges_mean="
              << static_cast<double>(total_initial_edges) /
                     static_cast<double>(checked)
              << '\n';
    std::cout << "initial_edges_max=" << maximum_initial_edges << '\n';
    std::cout << "final_edges_mean="
              << static_cast<double>(total_initial_edges) /
                         static_cast<double>(checked) +
                     static_cast<double>(options.batch)
              << '\n';
    std::cout << "serial_full_recomputations_ms=" << serial_total << '\n';
    std::cout << "one_batched_full_recomputation_ms=" << batch_total << '\n';
    std::cout << "speedup=" << serial_total / batch_total << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
