// dh test | exhaustive [N] | cli N BASE L_ADD L_DEL R_ADD R_DEL | scale NODES RATIO global|local SEED REPEATS
// local-sharing|cross-sharing K
#include "dh.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include <sys/resource.h>

using namespace dh;

namespace {

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

std::string text(std::size_t n, const Rows& rows) {
    std::string out;
    for (Node i = 0; i < n; ++i)
        for (Node j = 0; j < n; ++j)
            if (rows[i] >> j & 1U) out += (out.empty() ? "" : ",") + std::to_string(i) + "->" + std::to_string(j);
    return out;
}
Edges parse(std::string s) {  // "0>1,1>2" or "-"
    Edges out;
    for (std::size_t at = 0; s != "-" && at < s.size();) {
        const std::size_t end = std::min(s.find(',', at), s.size()), arrow = s.find('>', at);
        out.push_back({static_cast<Node>(std::stoul(s.substr(at, arrow - at))),
                       static_cast<Node>(std::stoul(s.substr(arrow + 1, end - arrow - 1)))});
        at = end + 1;
    }
    sort_unique(out);
    return out;
}
Rows rows(std::size_t n, const Edges& edges) { return closure(n, edges); }

// Every valid single-edge request on a base graph: delete an existing edge, or add one that
// keeps the graph acyclic.
std::vector<Batch> single_requests(std::size_t n, const Edges& base) {
    std::vector<Batch> out;
    for (Node a = 0; a < n; ++a)
        for (Node b = 0; b < n; ++b) {
            const Edge e{a, b};
            const Batch r = has(base, e) ? Batch{{}, {e}} : Batch{{e}, {}};
            if (a != b && apply_exact(n, base, r)) out.push_back(r);
        }
    return out;
}

// Interval metadata after repair: invariant, false negatives, false overlaps vs a full rebuild.
void check_intervals(std::size_t n, const Edges& base, const Edges& after, const Batch& b, std::size_t k,
                     std::map<std::string, std::uint64_t>& count) {
    const Csr g = from_edges(n, base), g2 = from_edges(n, after);
    const auto label = postorder_labels(g);
    auto sets = build_sets(g, label, k);
    repair(g2, reverse(g2), topo_order(g2), b, label, sets, k);
    const auto rebuilt = build_sets(g2, label, k);
    const Rows r = closure(n, after);
    std::uint64_t repaired_fp = 0, rebuilt_fp = 0;
    for (Node x = 0; x < n; ++x)
        for (Node y = x + 1; y < n; ++y) {
            const bool truth = ((r[x] | 1U << x) & (r[y] | 1U << y)) != 0;
            count["k" + std::to_string(k) + "_false_negatives"] += truth && !overlaps(sets[x], sets[y]);
            repaired_fp += !truth && overlaps(sets[x], sets[y]);
            rebuilt_fp += !truth && overlaps(rebuilt[x], rebuilt[y]);
        }
    const std::string key = "k" + std::to_string(k) + "_";
    count[key + "batches"]++;
    count[key + "invariant_failures"] += !invariant_holds(g2, label, sets, k);
    count[key + "false_overlaps_repaired"] += repaired_fp;
    count[key + "false_overlaps_rebuilt"] += rebuilt_fp;
    count[key + "repaired_less_precise_than_rebuilt"] += repaired_fp > rebuilt_fp;
    count[key + "repaired_more_precise_than_rebuilt"] += repaired_fp < rebuilt_fp;
}

int exhaustive(std::size_t n) {
    if (n < 2 || n > 5) throw std::invalid_argument("exhaustive needs 2..5 nodes");
    Edges forward;
    for (Node a = 0; a < n; ++a)
        for (Node b = a + 1; b < n; ++b) forward.push_back({a, b});
    std::map<std::string, std::uint64_t> count;
    for (std::uint64_t mask = 0; mask < (std::uint64_t{1} << forward.size()); ++mask) {
        Edges base;
        for (std::size_t i = 0; i < forward.size(); ++i)
            if (mask >> i & 1U) base.push_back(forward[i]);
        count["base_dags"]++;
        const auto requests = single_requests(n, base);
        count["individually_valid_deltas"] += requests.size();
        for (const Batch& r : requests)
            for (std::size_t k = 1; k <= 3; ++k) check_intervals(n, base, *apply_exact(n, base, r), r, k, count);
        for (std::size_t i = 0; i < requests.size(); ++i)
            for (std::size_t j = i + 1; j < requests.size(); ++j) {
                const Batch &l = requests[i], &r = requests[j];
                const auto m = merge(l, r);
                if (!m) continue;
                const std::string kind = l.add.empty() && r.add.empty() ? "delete-delete"
                                         : l.add.empty() || r.add.empty() ? "add-delete"
                                                                          : "add-add";
                const bool disjoint = (affected(n, base, l) & affected(n, base, r)) == 0;
                count["pairs"]++;
                count["pairs_" + kind]++;
                count[disjoint ? "disjoint_pairs" : "overlapping_pairs"]++;
                const auto merged = apply_exact(n, base, *m), lr = apply_exact(n, base, l), rl = apply_exact(n, base, r);
                const auto lr2 = lr ? apply_exact(n, *lr, r) : std::nullopt;
                const auto rl2 = rl ? apply_exact(n, *rl, l) : std::nullopt;
                const bool agrees = merged ? lr2 == merged && rl2 == merged : !lr2 && !rl2;
                count[agrees ? "exact_outcome_matches_serial_oracle" : "exact_outcome_serial_mismatches"]++;
                if (!merged) {
                    count["jointly_invalid"]++;
                    count["jointly_invalid_" + kind]++;
                    continue;
                }
                count["jointly_valid"]++;
                if (naive_union(n, base, l, r) == rows(n, *merged)) {
                    count["naive_metadata_union_correct"]++;
                } else {
                    count["naive_metadata_union_wrong"]++;
                    count["naive_metadata_union_wrong_" + kind]++;
                    count[std::string("naive_metadata_union_wrong_") + (disjoint ? "disjoint" : "overlapping")]++;
                }
                for (std::size_t k = 1; k <= 3; ++k) check_intervals(n, base, *merged, *m, k, count);
            }
    }
    std::cout << "nodes=" << n << '\n';
    for (const auto& [key, value] : count) std::cout << key << '=' << value << '\n';
    bool ok = count["exact_outcome_serial_mismatches"] == 0;
    for (std::size_t k = 1; k <= 3; ++k)
        ok = ok && count["k" + std::to_string(k) + "_false_negatives"] == 0 &&
             count["k" + std::to_string(k) + "_invariant_failures"] == 0;
    return ok ? 0 : 1;
}

// Two single requests on a base graph, for the independent Python oracle.
int cli(char** argv) {
    const std::size_t n = std::stoul(argv[0]);
    const Edges base = parse(argv[1]);
    const Batch l{parse(argv[2]), parse(argv[3])}, r{parse(argv[4]), parse(argv[5])};
    const auto m = merge(l, r);
    if (!apply_exact(n, base, l) || !apply_exact(n, base, r) || !m) {
        std::cout << "prepared=0\n";
        return 0;
    }
    const auto merged = apply_exact(n, base, *m);
    const auto lr = apply_exact(n, *apply_exact(n, base, l), r), rl = apply_exact(n, *apply_exact(n, base, r), l);
    const Edges edges = candidate(base, *m);
    Rows as_rows(n, 0);
    for (Edge e : edges) as_rows[e.from] |= 1U << e.to;
    std::cout << "prepared=1\nvalid=" << merged.has_value() << "\nedges=" << text(n, as_rows)
              << "\nreachability=" << text(n, rows(n, edges))
              << "\nnaive_match=" << (naive_union(n, base, l, r) == rows(n, edges))
              << "\nserial_match=" << (merged ? lr == merged && rl == merged : !lr && !rl) << '\n';
    return 0;
}

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int scale(char** argv) {
    const std::size_t n = std::stoull(argv[0]), k = std::stoull(argv[6]), repeats = std::stoull(argv[4]);
    const double ratio = std::stod(argv[1]);
    const std::string locality = argv[2], sharing = argv[5];
    const std::uint64_t seed = std::stoull(argv[3]);
    const auto updates = static_cast<std::size_t>(std::ceil(static_cast<double>(n) * ratio));
    auto clock = std::chrono::steady_clock::now;

    auto t = clock();
    const Csr g = make_tree_dag(n, 4, 8, seed, sharing == "local-sharing");
    const double build_ms = ms_since(t);
    const Batch b = make_update_batch(g, updates / 2, updates - updates / 2, 4, locality == "local", seed + 1);
    t = clock();
    const Dominators dom = dominators(g);
    const double dominator_ms = ms_since(t);
    // Dominators of G plus the additions dominate in both G and G' (both are subgraphs of it).
    const Dominators dom_union = dominators(apply_batch(g, {b.add, {}}).g);
    std::size_t root_base = 0, root_union = 0, root_add = 0;
    Node whole = b.add.empty() ? b.del[0].from : b.add[0].from;
    std::unordered_set<Node> groups;
    for (const Edges* x : {&b.add, &b.del})
        for (Edge e : *x) {
            const Node lca = dom.lca(e.from, e.to);
            root_base += lca == 0;
            root_add += lca == 0 && x == &b.add;
            root_union += dom_union.lca(e.from, e.to) == 0;
            groups.insert(top_branch(lca, 4));
            whole = dom.lca(dom.lca(whole, e.from), e.to);
        }

    std::vector<double> samples;
    Applied applied;
    for (std::size_t i = 0; i < repeats; ++i) {
        t = clock();
        applied = apply_batch(g, b);
        samples.push_back(ms_since(t));
        if (applied.error) throw std::runtime_error(applied.error);
    }
    std::sort(samples.begin(), samples.end());

    t = clock();
    const auto label = postorder_labels(g);
    auto sets = build_sets(g, label, k);
    const double label_ms = ms_since(t);
    t = clock();
    const auto order = topo_order(applied.g);
    const Csr parents = reverse(applied.g);
    const double prep_ms = ms_since(t);
    t = clock();
    const RepairStats st = repair(applied.g, parents, order, b, label, sets, k);
    const double repair_ms = ms_since(t);
    t = clock();
    const auto rebuilt = build_sets(applied.g, label, k);
    const double rebuild_ms = ms_since(t);
    std::size_t fragments = 0;
    for (const Set& s : sets) fragments += s.size();

    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double mutations = static_cast<double>(b.add.size() + b.del.size());
    std::cout << std::fixed << std::setprecision(3)
              << "nodes,base_edges,updates,update_ratio,additions,deletions,locality,sharing,repeats,build_ms,"
                 "dominator_build_ms,apply_ms,base_storage_bytes,peak_rss_bytes,whole_batch_selects_root,"
                 "root_edge_lca_fraction,root_edge_lca_fraction_union,root_addition_lca_fraction,"
                 "distinct_top_groups,k,label_and_sets_ms,"
                 "repair_prep_ms,repair_ms,repair_visited,repair_changed,rebuild_ms,mean_fragments\n"
              << n << ',' << g.dst.size() << ',' << updates << ',' << ratio << ',' << b.add.size() << ','
              << b.del.size() << ',' << locality << ',' << sharing << ',' << repeats << ',' << build_ms << ','
              << dominator_ms << ',' << samples[samples.size() / 2] << ',' << g.bytes() << ','
              << usage.ru_maxrss << ',' << (whole == 0) << ',' << root_base / mutations << ','
              << root_union / mutations << ',' << static_cast<double>(root_add) / static_cast<double>(b.add.size())
              << ',' << groups.size() << ',' << k << ',' << label_ms << ','
              << prep_ms << ',' << repair_ms << ',' << st.visited << ',' << st.changed << ',' << rebuild_ms << ','
              << static_cast<double>(fragments) / static_cast<double>(n) << '\n';
    return 0;
}

int test() {
    // Emergent path: 0->1 and 1->2 together create 0->2, which neither private delta records.
    const Batch a01{{{0, 1}}, {}}, a12{{{1, 2}}, {}};
    require(naive_union(3, {}, a01, a12)[0] == 0b010 && rows(3, *apply_exact(3, {}, *merge(a01, a12)))[0] == 0b110,
            "emergent path");
    // Diamond: deleting either branch keeps 0->3; deleting both removes it.
    const Edges diamond{{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    const Batch d13{{}, {{1, 3}}}, d23{{}, {{2, 3}}};
    require((naive_union(4, diamond, d13, d23)[0] >> 3 & 1U) && !(rows(4, *apply_exact(4, diamond, *merge(d13, d23)))[0] >> 3 & 1U),
            "diamond deletion");
    // Each addition is valid alone; together they form a cycle, and so does either serial order.
    require(apply_exact(2, {}, a01) && !apply_exact(2, {}, *merge(a01, Batch{{{1, 0}}, {}})), "joint cycle");
    require(!apply_exact(2, {{0, 1}}, *merge(Batch{{}, {}}, a01)), "existing addition");
    require(!merge(a01, a01), "same edge in two deltas");

    require(from_edges(4, {{2, 3}, {0, 2}, {0, 1}, {0, 1}}).dst.size() == 3, "duplicate edge kept");
    const Csr path = from_edges(3, {{0, 1}, {1, 2}});
    require(apply_batch(path, {{{2, 0}}, {}}).error, "cycle accepted");
    require(apply_batch(path, {{}, {{0, 1}}}).error, "unrooted result accepted");
    require(apply_batch(path, {{{0, 1}}, {}}).error && apply_batch(path, {{}, {{0, 2}}}).error &&
                apply_batch(path, {{{0, 2}}, {{0, 2}}}).error,
            "invalid preconditions accepted");
    const Dominators dom = dominators(from_edges(6, {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 4}, {1, 5}}));
    require(dom.idom[3] == 0 && dom.idom[4] == 3 && dom.lca(4, 5) == 0, "dominator tree");

    std::mt19937_64 rng(91);
    for (int round = 0; round < 200; ++round) {  // sparse engine vs exact model, random forward batches
        constexpr std::size_t n = 8;
        Edges base;
        Batch b;
        for (Node x = 0; x < n; ++x)
            for (Node y = x + 1; y < n; ++y)
                if (rng() % 5 == 0) base.push_back({x, y});
        for (Edge e : base)
            if (rng() % 7 == 0) b.del.push_back(e);
        for (Node x = 0; x < n; ++x)
            for (Node y = x + 1; y < n; ++y)
                if (!has(base, {x, y}) && rng() % 11 == 0) b.add.push_back({x, y});
        const Applied sparse = apply_batch(from_edges(n, base), b, false);
        require(!sparse.error && Edges(apply_exact(n, base, b).value()) == candidate(base, b), "sparse vs exact");
        for (Node v = 0; v < n; ++v)
            for (Node c : sparse.g.kids(v)) require(has(candidate(base, b), {v, c}), "sparse edge set");
        require(sparse.g.dst.size() == candidate(base, b).size(), "sparse edge count");
    }

    const Csr tree = make_tree_dag(10000, 4, 0, 11, true);
    const Dominators tdom = dominators(tree);
    auto root_hits = [&](bool local) {
        std::size_t hits = 0;
        for (Edge e : make_update_batch(tree, 500, 0, 4, local, 12).add) hits += tdom.lca(e.from, e.to) == 0;
        return hits;
    };
    require(root_hits(false) > root_hits(true), "locality should reduce root-selected edges");
    const Csr dag = make_tree_dag(1000, 4, 8, 9, true);
    require(!apply_batch(dag, make_update_batch(dag, 50, 50, 4, false, 10)).error, "generated batch must stay rooted");

    // Online closest-gap merging is not optimal: 12 falsely covered labels vs 11 offline.
    auto garbage = [](const Set& s) {
        std::size_t covered = 0;
        for (Range r : s) covered += r.second - r.first + 1;
        return covered - 6;
    };
    Set online;
    for (Node p : {0U, 5U, 20U, 9U, 13U, 17U}) online.push_back({p, p}), online = compress(online, 2);
    require(garbage(online) == 12 && garbage(compress({{0, 0}, {5, 5}, {20, 20}, {9, 9}, {13, 13}, {17, 17}}, 2)) == 11,
            "online vs offline compression");

    for (int round = 0; round < 300; ++round) {  // repair keeps the invariant on random rooted DAGs
        const std::size_t n = 6 + rng() % 26, k = 1 + rng() % 4;
        const Node branching = 2 + static_cast<Node>(rng() % 3);
        const Csr g = make_tree_dag(n, branching, 3, rng(), false);
        const Batch b = make_update_batch(g, rng() % 6, rng() % 2, branching, false, rng());
        const Applied after = apply_batch(g, b);
        require(!after.error, "generated batch rejected");
        const auto label = postorder_labels(g);
        auto sets = build_sets(g, label, k);
        repair(after.g, reverse(after.g), topo_order(after.g), b, label, sets, k);
        require(invariant_holds(after.g, label, sets, k), "repair broke the invariant");
    }
    std::cout << "all tests passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "test";
        if (mode == "test") return test();
        if (mode == "exhaustive") return exhaustive(argc > 2 ? std::stoul(argv[2]) : 5);
        if (mode == "cli" && argc == 8) return cli(argv + 2);
        if (mode == "scale" && argc == 9) return scale(argv + 2);
        std::cerr << "usage: dh test | exhaustive [N] | cli N BASE L_ADD L_DEL R_ADD R_DEL |\n"
                     "       scale NODES RATIO global|local SEED REPEATS local-sharing|cross-sharing K\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
