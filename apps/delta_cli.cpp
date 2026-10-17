#include "delta/model.hpp"

#include <charconv>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

delta::Node parse_node(std::string_view text) {
  delta::Node result{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::invalid_argument("invalid node: " + std::string(text));
  }
  return result;
}

delta::EdgeSet parse_edges(std::string_view text) {
  delta::EdgeSet result;
  if (text == "-" || text.empty()) {
    return result;
  }
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    const auto token = text.substr(start, end - start);
    const std::size_t arrow = token.find('>');
    if (arrow == std::string_view::npos) {
      throw std::invalid_argument("invalid edge: " + std::string(token));
    }
    result.insert(delta::Edge{parse_node(token.substr(0, arrow)),
                              parse_node(token.substr(arrow + 1))});
    start = end + 1;
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr << "usage: delta_cli N BASE D1_ADD D1_DELETE D2_ADD D2_DELETE\n"
                 "edge lists use 0>1,1>2 and '-' means empty\n";
    return 2;
  }
  try {
    const auto node_count = static_cast<std::size_t>(std::stoul(argv[1]));
    const auto base = delta::GraphState::create(node_count, parse_edges(argv[2]));
    const delta::UpdateRequest left{"left", parse_edges(argv[3]),
                                    parse_edges(argv[4])};
    const delta::UpdateRequest right{"right", parse_edges(argv[5]),
                                     parse_edges(argv[6])};
    const auto left_delta = delta::prepare_delta(base, left);
    const auto right_delta = delta::prepare_delta(base, right);

    const auto left_validation = delta::validate_delta(base, left_delta);
    const auto right_validation = delta::validate_delta(base, right_delta);
    if (!left_validation.valid || !right_validation.valid) {
      std::cout << "prepared=0\n";
      std::cout << "reason="
                << delta::join_reasons(!left_validation.valid
                                           ? left_validation.reasons
                                           : right_validation.reasons)
                << "\n";
      return 0;
    }

    const auto merged =
        delta::merge_deltas(base, {left_delta, right_delta});
    const auto validation = delta::validate_delta(base, merged);
    const auto comparison =
        delta::compare_two_serial_orders(base, left, right);
    std::cout << "prepared=1\n";
    std::cout << "valid=" << (validation.valid ? 1 : 0) << "\n";
    std::cout << "reason=" << delta::join_reasons(validation.reasons) << "\n";
    std::cout << "edges=" << delta::edges_to_string(merged.candidate_edges)
              << "\n";
    std::cout << "reachability="
              << delta::edges_to_string(merged.candidate_reachability) << "\n";
    std::cout << "naive_match="
              << (delta::naive_metadata_union(base, {left_delta, right_delta}) ==
                          merged.candidate_reachability
                      ? 1
                      : 0)
              << "\n";
    std::cout << "serial_match="
              << (comparison.agrees_with_both_orders ? 1 : 0) << "\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
