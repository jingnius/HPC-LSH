#include "hpc_partition_build.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <unordered_set>

namespace {

int env_int(const char* key, int def) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') {
        return def;
    }
    return std::atoi(v);
}

bool env_enabled(const char* key, bool def_true) {
    const char* v = std::getenv(key);
    if (v == nullptr) {
        return def_true;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return !(s == "0" || s == "false" || s == "no");
}

int dim_order_key(const std::string& dim_name) {
    static const std::unordered_map<std::string, int> order = {
        {"first1", 1},  {"second1", 2}, {"third1", 3},  {"fourth1", 4}, {"fifth1", 5},
        {"sixth1", 6},  {"seventh1", 7}, {"eighth1", 8}, {"ninth1", 9},  {"tenth1", 10},
    };
    const auto it = order.find(dim_name);
    if (it != order.end()) {
        return it->second;
    }
    if (dim_name.rfind("nth", 0) == 0) {
        const std::string tail = dim_name.substr(3);
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), ::isdigit)) {
            return std::stoi(tail);
        }
    }
    return 1000;
}

std::string nth_dim_name(int n) {
    static const char* names[] = {
        "first1", "second1", "third1", "fourth1", "fifth1",
        "sixth1", "seventh1", "eighth1", "ninth1", "tenth1",
    };
    if (n >= 1 && n <= 10) {
        return names[n - 1];
    }
    return "nth" + std::to_string(n);
}

int nth_from_dim_name(const std::string& dim_name) {
    static const std::unordered_map<std::string, int> order = {
        {"first1", 1},  {"second1", 2}, {"third1", 3},  {"fourth1", 4}, {"fifth1", 5},
        {"sixth1", 6},  {"seventh1", 7}, {"eighth1", 8}, {"ninth1", 9},  {"tenth1", 10},
    };
    const auto it = order.find(dim_name);
    if (it != order.end()) {
        return it->second;
    }
    if (dim_name.rfind("nth", 0) == 0) {
        const std::string tail = dim_name.substr(3);
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), ::isdigit)) {
            return std::stoi(tail);
        }
    }
    return 1000;
}

std::vector<int32_t> active_hits_span(
    const int32_t* hits,
    int len,
    const std::unordered_set<int32_t>& part_index_set) {
    std::vector<int32_t> out;
    if (hits == nullptr || len <= 0) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) {
        if (part_index_set.find(hits[i]) != part_index_set.end()) {
            out.push_back(hits[i]);
        }
    }
    return out;
}

int partition_depth_proxy(
    const HpcPartitionNode& part,
    const HpcIndexOneStore& index_one,
    const std::vector<int64_t>& col_ids) {
    const auto& part_index = part.one_index;
    if (part_index.empty()) {
        return 0;
    }
    std::unordered_map<int32_t, int> rank_map;
    rank_map.reserve(part_index.size());
    for (std::size_t i = 0; i < part_index.size(); ++i) {
        rank_map[part_index[i]] = static_cast<int>(i);
    }
    std::unordered_set<int32_t> pi_set(part_index.begin(), part_index.end());
    int mx = 0;
    for (const int64_t ic : col_ids) {
        const int32_t* hits = nullptr;
        int hit_len = 0;
        if (!index_one.find_hits(ic, &hits, &hit_len) || hits == nullptr || hit_len <= 0) {
            continue;
        }
        const std::vector<int32_t> active = active_hits_span(hits, hit_len, pi_set);
        if (!active.empty()) {
            const int32_t max_hit = *std::max_element(active.begin(), active.end());
            const auto rit = rank_map.find(max_hit);
            if (rit != rank_map.end()) {
                mx = std::max(mx, rit->second + 1);
            }
        }
    }
    return std::max(mx, static_cast<int>(part_index.size()));
}

void set_index_bounds(HpcPartitionNode& part) {
    if (part.one_index.empty()) {
        return;
    }
    part.min_1index = *std::min_element(part.one_index.begin(), part.one_index.end());
    part.max_1index = *std::max_element(part.one_index.begin(), part.one_index.end());
}

struct SplitQuality {
    bool ok = false;
    double max_ratio = 1.0;
    double entropy = 0.0;
};

SplitQuality split_quality(const std::vector<int64_t>& col_ids, const std::map<std::string, int>& counts, double max_child_ratio) {
    SplitQuality out;
    const int n = static_cast<int>(col_ids.size());
    if (n == 0) {
        return out;
    }
    std::vector<int> non_empty;
    for (const auto& kv : counts) {
        if (kv.second > 0) {
            non_empty.push_back(kv.second);
        }
    }
    if (static_cast<int>(non_empty.size()) < 2) {
        return out;
    }
    out.max_ratio = static_cast<double>(*std::max_element(non_empty.begin(), non_empty.end())) / static_cast<double>(n);
    if (out.max_ratio > max_child_ratio) {
        return out;
    }
    double entropy = 0.0;
    for (const int c : non_empty) {
        const double p = static_cast<double>(c) / static_cast<double>(n);
        entropy -= p * std::log2(p);
    }
    out.entropy = entropy;
    out.ok = true;
    return out;
}

struct SplitCountsResult {
    std::vector<int32_t> boundaries;
    HpcOneIndexPartition one_index_partition;
    std::map<std::string, int> counts;
};

SplitCountsResult split_child_counts(
    const std::vector<int64_t>& col_ids,
    const HpcIndexOneStore& index_one,
    const std::vector<int32_t>& part_index,
    int nth,
    int num_segments) {
    SplitCountsResult out;
    std::unordered_set<int32_t> part_index_set(part_index.begin(), part_index.end());
    out.boundaries = hpc_adaptive_first1_boundaries_nth(col_ids, index_one, nth, num_segments);
    out.one_index_partition = hpc_assign_oneindex_adaptive(part_index, out.boundaries, num_segments);
    for (const int64_t ic : col_ids) {
        const int32_t* hits = nullptr;
        int hit_len = 0;
        const std::vector<int32_t> active =
            index_one.find_hits(ic, &hits, &hit_len)
                ? active_hits_span(hits, hit_len, part_index_set)
                : std::vector<int32_t>{};
        const std::string key = hpc_csp_cskey_1index_zipfast_gai(active, part_index, out.one_index_partition);
        out.counts[key] += 1;
    }
    return out;
}

struct SplitEval {
    std::string dim_name;
    std::vector<int32_t> boundaries;
    HpcOneIndexPartition one_index_partition;
    std::map<std::string, int> counts;
    bool ok = false;
    int non_empty = 0;
    double score = 0.0;
};

std::vector<SplitEval> evaluate_split_dims(
    const std::vector<int64_t>& col_ids,
    const HpcIndexOneStore& index_one,
    const std::vector<int32_t>& part_index,
    int num_segments,
    int max_nth,
    double max_child_ratio,
    const std::vector<std::string>* dim_names) {
    std::vector<std::string> ranked;
    ranked.reserve(static_cast<std::size_t>(max_nth));
    for (int n = 1; n <= max_nth; ++n) {
        ranked.push_back(nth_dim_name(n));
    }
    if (dim_names != nullptr) {
        std::unordered_set<std::string> allowed(dim_names->begin(), dim_names->end());
        std::vector<std::string> filtered;
        for (const auto& d : ranked) {
            if (allowed.find(d) != allowed.end()) {
                filtered.push_back(d);
            }
        }
        ranked = std::move(filtered);
    }
    std::vector<SplitEval> evals;
    evals.reserve(ranked.size());
    for (const auto& dim_name : ranked) {
        SplitEval ev;
        ev.dim_name = dim_name;
        SplitCountsResult sc = split_child_counts(
            col_ids,
            index_one,
            part_index,
            nth_from_dim_name(dim_name),
            num_segments);
        ev.boundaries = std::move(sc.boundaries);
        ev.one_index_partition = std::move(sc.one_index_partition);
        ev.counts = std::move(sc.counts);
        SplitQuality q = split_quality(col_ids, ev.counts, max_child_ratio);
        ev.ok = q.ok;
        for (const auto& c : ev.counts) {
            if (c.second > 0) {
                ev.non_empty += 1;
            }
        }
        ev.score = q.entropy - q.max_ratio;
        evals.push_back(std::move(ev));
    }
    return evals;
}

struct ChosenSplit {
    std::string dim_name;
    std::vector<int32_t> boundaries;
    HpcOneIndexPartition one_index_partition;
    bool forced = false;
    bool valid = false;
};

ChosenSplit choose_split_from_evals(const std::vector<SplitEval>& evals) {
    ChosenSplit out;
    const SplitEval* best = nullptr;
    double best_score = -1.0;
    for (const auto& ev : evals) {
        if (!ev.ok) {
            continue;
        }
        if (ev.score > best_score) {
            best_score = ev.score;
            best = &ev;
        }
    }
    if (best != nullptr) {
        out.dim_name = best->dim_name;
        out.boundaries = best->boundaries;
        out.one_index_partition = best->one_index_partition;
        out.valid = true;
        return out;
    }
    std::vector<const SplitEval*> splittable;
    for (const auto& ev : evals) {
        if (ev.non_empty >= 2) {
            splittable.push_back(&ev);
        }
    }
    if (splittable.empty()) {
        return out;
    }
    const SplitEval* forced_best = splittable[0];
    for (const SplitEval* ev : splittable) {
        if (ev->score > forced_best->score) {
            forced_best = ev;
        }
    }
    out.dim_name = forced_best->dim_name;
    out.boundaries = forced_best->boundaries;
    out.one_index_partition = forced_best->one_index_partition;
    out.forced = true;
    out.valid = true;
    return out;
}

std::pair<std::vector<int>, int> k_candidates_for_node(
    int n_cols,
    int depth,
    int k_min,
    int k_max,
    int min_cols,
    int num_layers,
    int root_k) {
    if (root_k >= 0 && depth == 0) {
        return {{root_k}, root_k};
    }
    const int layers_left = std::max(num_layers - depth, 1);
    const double n_ref = std::max(static_cast<double>(n_cols), static_cast<double>(min_cols) * 2.0);
    int k_target = static_cast<int>(std::ceil(
        std::pow(n_ref, 1.0 / layers_left) / std::pow(static_cast<double>(min_cols), 1.0 / layers_left)));
    k_target = std::max(k_min, std::min(k_max, k_target));
    std::vector<int> cands = {k_target};
    if (k_target - 1 >= k_min) {
        cands.push_back(k_target - 1);
    }
    if (k_target + 1 <= k_max) {
        cands.push_back(k_target + 1);
    }
    std::sort(cands.begin(), cands.end(), [k_target](int a, int b) {
        const auto key = [k_target](int k) {
            return std::make_pair(k == k_target ? 0 : 1, std::abs(k - k_target));
        };
        return key(a) < key(b);
    });
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    return {cands, k_target};
}

std::pair<std::vector<int>, int> k_candidates_large_part(
    int n_cols,
    int n_global,
    int depth,
    int k_min,
    int k_max,
    int min_cols,
    int num_layers,
    int root_k,
    const HpcPartitionBuildConfig& cfg) {
    auto base = k_candidates_for_node(n_cols, depth, k_min, k_max, min_cols, num_layers, root_k);
    if (n_cols < cfg.large_part_cols) {
        return base;
    }
    const int k_lo = std::max(k_min, cfg.large_k_min);
    const int k_hi = std::max(k_max, cfg.large_k_max);
    std::vector<int> ordered;
    auto push_unique = [&](int k) {
        if (k >= k_lo && k <= k_hi && std::find(ordered.begin(), ordered.end(), k) == ordered.end()) {
            ordered.push_back(k);
        }
    };
    push_unique(base.second);
    for (int k = k_lo; k <= k_hi; ++k) {
        push_unique(k);
    }
    for (int k : base.first) {
        push_unique(k);
    }
    return {ordered, base.second};
}

struct AdaptivePick {
    ChosenSplit chosen;
    int seg_k = 0;
    int split_attempts = 0;
    int k_target = 0;
    bool failed = false;
    bool has_fallback = false;
};

AdaptivePick pick_best_split_adaptive_k(
    const std::vector<int64_t>& col_ids,
    const HpcIndexOneStore& index_one,
    const std::vector<int32_t>& part_index,
    int max_nth,
    int k_min,
    int k_max,
    double max_child_ratio,
    int initial_dims,
    int depth,
    int min_cols,
    int num_layers,
    int n_global,
    int root_k,
    const HpcPartitionBuildConfig& cfg) {
    AdaptivePick out;
    std::vector<std::string> ranked;
    ranked.reserve(static_cast<std::size_t>(max_nth));
    for (int n = 1; n <= max_nth; ++n) {
        ranked.push_back(nth_dim_name(n));
    }
    std::vector<std::string> primary;
    std::vector<std::string> extra;
    int eff_initial_dims = initial_dims;
    int eff_k_max = k_max;
    if (static_cast<int>(col_ids.size()) >= cfg.large_part_cols && env_enabled("HPC_LARGE_PART_SPLIT", true)) {
        const double ratio = std::max(static_cast<double>(col_ids.size()) / std::max(min_cols, 1), 2.0);
        eff_initial_dims = std::min(
            cfg.large_max_split_dims,
            std::max(initial_dims, static_cast<int>(std::ceil(std::log2(ratio))) + 3));
        eff_k_max = std::max(k_max, cfg.large_k_max);
    }
    for (const auto& d : ranked) {
        if (dim_order_key(d) <= eff_initial_dims) {
            primary.push_back(d);
        } else {
            extra.push_back(d);
        }
    }
    auto [k_candidates, k_target] = k_candidates_large_part(
        static_cast<int>(col_ids.size()), n_global, depth, k_min, eff_k_max, min_cols, num_layers, root_k, cfg);
    out.k_target = k_target;
    ChosenSplit fallback;
    int fallback_k = 0;
    for (const int k : k_candidates) {
        auto evals_primary = evaluate_split_dims(
            col_ids, index_one, part_index, k, max_nth, max_child_ratio, &primary);
        out.split_attempts += static_cast<int>(evals_primary.size());
        ChosenSplit chosen = choose_split_from_evals(evals_primary);
        if (chosen.valid && !chosen.forced) {
            out.chosen = chosen;
            out.seg_k = k;
            return out;
        }
        if (chosen.valid && !fallback.valid) {
            fallback = chosen;
            fallback.valid = true;
            fallback_k = k;
        }
        if (!extra.empty()) {
            auto evals_extra = evaluate_split_dims(
                col_ids, index_one, part_index, k, max_nth, max_child_ratio, &extra);
            out.split_attempts += static_cast<int>(evals_extra.size());
            evals_primary.insert(evals_primary.end(), evals_extra.begin(), evals_extra.end());
            ChosenSplit chosen2 = choose_split_from_evals(evals_primary);
            if (chosen2.valid && !chosen2.forced) {
                out.chosen = chosen2;
                out.seg_k = k;
                return out;
            }
            if (chosen2.valid && !fallback.valid) {
                fallback = chosen2;
                fallback_k = k;
            }
        }
    }
    if (fallback.valid) {
        out.chosen = fallback;
        out.seg_k = fallback_k;
        out.has_fallback = true;
        out.failed = false;
        return out;
    }
    out.failed = true;
    return out;
}

void apply_adaptive_split(
    HpcPartitionNode& part,
    const std::vector<int64_t>& col_ids,
    const HpcIndexOneStore& index_one,
    const std::vector<int32_t>& boundaries,
    const HpcOneIndexPartition& one_index_partition) {
    std::unordered_set<int32_t> part_index_set(part.one_index.begin(), part.one_index.end());
    part.segment_boundaries = boundaries;
    part.one_index_partition = one_index_partition;
    for (const int64_t ic : col_ids) {
        const int32_t* hits = nullptr;
        int hit_len = 0;
        const std::vector<int32_t> active =
            index_one.find_hits(ic, &hits, &hit_len)
                ? active_hits_span(hits, hit_len, part_index_set)
                : std::vector<int32_t>{};
        const std::string key = hpc_csp_cskey_1index_zipfast_gai(active, part.one_index, one_index_partition);
        auto cit = part.next_partition.find(key);
        if (cit == part.next_partition.end()) {
            auto child = std::make_unique<HpcPartitionNode>();
            cit = part.next_partition.emplace(key, std::move(child)).first;
        }
        cit->second->ids.push_back(ic);
        for (const int32_t h : active) {
            cit->second->one_index.push_back(h);
        }
    }
}

void json_escape(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            os << '\\';
        }
        os << c;
    }
    os << '"';
}

void write_json_array(std::ostream& os, const std::vector<int64_t>& vals) {
    os << "[";
    for (std::size_t i = 0; i < vals.size(); ++i) {
        if (i) {
            os << ",";
        }
        os << vals[i];
    }
    os << "]";
}

void write_json_i32_array(std::ostream& os, const std::vector<int32_t>& vals) {
    os << "[";
    for (std::size_t i = 0; i < vals.size(); ++i) {
        if (i) {
            os << ",";
        }
        os << vals[i];
    }
    os << "]";
}

void write_partition_node_json(std::ostream& os, const HpcPartitionNode& node) {
    os << "{";
    os << "\"node_id\":" << node.node_id << ",";
    os << "\"ids\":"; write_json_array(os, node.ids); os << ",";
    os << "\"1Index\":"; write_json_i32_array(os, node.one_index); os << ",";
    os << "\"segment_boundaries\":"; write_json_i32_array(os, node.segment_boundaries); os << ",";
    os << "\"oneIndex_partition\":{";
    bool first_oip = true;
    const int seg_k = hpc_partition_segment_count(node);
    HpcOneIndexPartition oip_view = node.one_index_partition;
    if (oip_view.empty() && seg_k > 0 && !node.one_index.empty()) {
        oip_view = hpc_assign_oneindex_adaptive(node.one_index, node.segment_boundaries, seg_k);
    }
    std::vector<int> seg_keys;
    seg_keys.reserve(oip_view.size());
    for (const auto& kv : oip_view) {
        seg_keys.push_back(kv.first);
    }
    std::sort(seg_keys.begin(), seg_keys.end());
    for (const int seg : seg_keys) {
        if (!first_oip) {
            os << ",";
        }
        first_oip = false;
        os << "\"" << seg << "\":";
        std::vector<int32_t> hits(oip_view.at(seg).begin(), oip_view.at(seg).end());
        std::sort(hits.begin(), hits.end());
        write_json_i32_array(os, hits);
    }
    os << "},";
    if (!node.split_dim.empty()) {
        os << "\"split_dim\":"; json_escape(os, node.split_dim); os << ",";
        os << "\"split_k\":" << node.split_k << ",";
        os << "\"split_forced\":" << (node.split_forced ? "true" : "false") << ",";
    }
    os << "\"nextPartition\":{";
    bool first_child = true;
    for (const auto& kv : node.next_partition) {
        if (!first_child) {
            os << ",";
        }
        first_child = false;
        json_escape(os, kv.first);
        os << ":";
        write_partition_node_json(os, *kv.second);
    }
    os << "}";
    os << "}";
}

int count_nodes(const HpcPartitionNode& node) {
    int n = 1;
    for (const auto& kv : node.next_partition) {
        n += count_nodes(*kv.second);
    }
    return n;
}

void assign_node_ids(HpcPartitionNode& node, int* next_id) {
    node.node_id = (*next_id)++;
    for (auto& kv : node.next_partition) {
        assign_node_ids(*kv.second, next_id);
    }
}

}  // namespace

int hpc_partition_segment_count(const HpcPartitionNode& part) {
    if (part.split_k > 0) {
        return part.split_k;
    }
    if (!part.segment_boundaries.empty()) {
        return static_cast<int>(part.segment_boundaries.size()) + 1;
    }
    return static_cast<int>(part.one_index_partition.size());
}

HpcPartitionBuildResult hpc_run_partition_bfs(
    const HpcIndexOneStore& index_one,
    const HpcPartitionBuildConfig& cfg) {
    HpcPartitionBuildConfig c = cfg;
    if (env_enabled("HPC_DEPTH_AWARE", true)) {
        c.depth_stop = env_int("HPC_DEPTH_STOP", c.depth_stop);
        c.depth_extra_l = env_int("HPC_DEPTH_EXTRA_L", c.depth_extra_l);
    }
    if (env_enabled("HPC_LARGE_PART_SPLIT", true)) {
        c.large_part_cols = env_int("HPC_LARGE_PART_COLS", c.large_part_cols);
        c.large_k_min = env_int("HPC_LARGE_K_MIN", c.large_k_min);
        c.large_k_max = env_int("HPC_LARGE_K_MAX", c.large_k_max);
        c.large_max_split_dims = env_int("HPC_MAX_SPLIT_DIMS", c.large_max_split_dims);
    }
    const bool build_v7 = env_enabled("HPC_BUILD_V7", false);
    if (build_v7) {
        c.max_mgn_leaf_cols = env_int("HPC_MAX_MGN_LEAF_COLS", c.max_mgn_leaf_cols);
        c.mgn_nary_min_cols = env_int("HPC_MGN_NARY_MIN_COLS", c.mgn_nary_min_cols);
        c.mgn_flat_max_cols = env_int("HPC_MGN_FLAT_MAX_COLS", c.mgn_flat_max_cols);
        c.mgn_nary_max_L = env_int("HPC_MGN_NARY_MAX_L", c.mgn_nary_max_L);
    }

    HpcPartitionBuildResult result;
    int max_hits = 0;
    for (std::size_t i = 0; i + 1 < index_one.hit_off.size(); ++i) {
        max_hits = std::max(max_hits, static_cast<int>(index_one.hit_off[i + 1] - index_one.hit_off[i]));
    }
    const int max_nth = std::min(std::max(max_hits, 1), c.max_split_dims);

    auto root = std::make_unique<HpcPartitionNode>();
    root->ids = index_one.col_ids;
    for (std::size_t i = 0; i + 1 < index_one.hit_off.size(); ++i) {
        const int32_t* hits = index_one.hit_pool.data() + index_one.hit_off[i];
        const int len = static_cast<int>(index_one.hit_off[i + 1] - index_one.hit_off[i]);
        root->one_index.insert(root->one_index.end(), hits, hits + len);
    }
    std::sort(root->one_index.begin(), root->one_index.end());
    root->one_index.erase(std::unique(root->one_index.begin(), root->one_index.end()), root->one_index.end());
    set_index_bounds(*root);

    const int n_cols = static_cast<int>(index_one.size());
    const int last_hpc_layer = c.num_hpc_layers;
    const int max_extend_layer = last_hpc_layer + c.depth_extra_l;

    struct QueueItem {
        HpcPartitionNode* part;
        int depth;
    };
    std::deque<QueueItem> queue;
    queue.push_back({root.get(), 0});

    auto leaf_now = [&](HpcPartitionNode& part, int depth, const std::vector<int64_t>& col_ids) {
        const int dp = partition_depth_proxy(part, index_one, col_ids);
        const int n = static_cast<int>(col_ids.size());
        if (!build_v7) {
            if (depth < last_hpc_layer) {
                if (n <= c.min_cols) {
                    return std::make_tuple(true, dp);
                }
                return std::make_tuple(false, dp);
            }
            if (dp > c.depth_stop && depth < max_extend_layer) {
                return std::make_tuple(false, dp);
            }
            return std::make_tuple(true, dp);
        }
        if (depth < last_hpc_layer) {
            return std::make_tuple(n <= c.min_cols, dp);
        }
        if (n > c.max_mgn_leaf_cols && depth < max_extend_layer) {
            return std::make_tuple(false, dp);
        }
        if (n > c.min_cols && depth < max_extend_layer) {
            if (dp > c.depth_stop) {
                return std::make_tuple(false, dp);
            }
            return std::make_tuple(false, dp);
        }
        if (dp > c.depth_stop && depth < max_extend_layer) {
            return std::make_tuple(false, dp);
        }
        return std::make_tuple(true, dp);
    };

    auto mark_mgn_leaf = [&](HpcPartitionNode* part, int depth) {
        part->is_mgn_leaf = true;
        part->stop_depth = depth;
        part->stop_depth_proxy = partition_depth_proxy(*part, index_one, part->ids);
        result.mgn_leaves.push_back(part);
    };

    while (!queue.empty()) {
        QueueItem item = queue.front();
        queue.pop_front();
        HpcPartitionNode* part = item.part;
        const int depth = item.depth;
        const auto [stop, dp] = leaf_now(*part, depth, part->ids);
        (void)dp;
        if (stop) {
            mark_mgn_leaf(part, depth);
            continue;
        }

        const int root_k = (depth == 0) ? c.root_num_segments : -1;
        AdaptivePick pick = pick_best_split_adaptive_k(
            part->ids,
            index_one,
            part->one_index,
            max_nth,
            c.adaptive_k_min,
            c.adaptive_k_max,
            c.max_child_ratio,
            c.initial_split_dims,
            depth,
            c.min_cols,
            c.num_hpc_layers,
            n_cols,
            root_k,
            c);
        if (pick.failed || !pick.chosen.valid) {
            if (build_v7 && part->split_retry_count < 1
                && static_cast<int>(part->ids.size()) > c.min_cols
                && depth < max_extend_layer) {
                part->split_retry_count += 1;
                queue.push_front({part, depth});
                continue;
            }
            mark_mgn_leaf(part, depth);
            continue;
        }

        part->split_dim = pick.chosen.dim_name;
        part->split_k = pick.seg_k > 0 ? pick.seg_k : pick.k_target;
        part->split_forced = pick.chosen.forced;

        apply_adaptive_split(
            *part,
            part->ids,
            index_one,
            pick.chosen.boundaries,
            pick.chosen.one_index_partition);

        for (auto& kv : part->next_partition) {
            HpcPartitionNode* child = kv.second.get();
            std::sort(child->one_index.begin(), child->one_index.end());
            child->one_index.erase(
                std::unique(child->one_index.begin(), child->one_index.end()), child->one_index.end());
            set_index_bounds(*child);
            const auto [c_stop, c_dp] = leaf_now(*child, depth + 1, child->ids);
            (void)c_dp;
            if (c_stop) {
                mark_mgn_leaf(child, depth + 1);
            } else {
                queue.push_back({child, depth + 1});
            }
        }
    }

    int next_id = 0;
    assign_node_ids(*root, &next_id);

    {
        std::vector<HpcPartitionNode*> unique_leaves;
        unique_leaves.reserve(result.mgn_leaves.size());
        std::unordered_map<HpcPartitionNode*, bool> seen;
        for (HpcPartitionNode* leaf : result.mgn_leaves) {
            if (leaf != nullptr && seen.find(leaf) == seen.end()) {
                seen[leaf] = true;
                unique_leaves.push_back(leaf);
            }
        }
        result.mgn_leaves = std::move(unique_leaves);
    }

    result.root = std::move(root);
    result.hpc_nodes = count_nodes(*result.root);
    return result;
}

bool hpc_partition_write_json(
    const HpcPartitionNode& root,
    const std::vector<HpcPartitionNode*>& mgn_leaves,
    const std::string& path,
    std::string* err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) {
            *err = "cannot open partition json: " + path;
        }
        return false;
    }
    out << "{\"root\":";
    write_partition_node_json(out, root);
    out << ",\"mgn_leaf_ids\":[";
    for (std::size_t i = 0; i < mgn_leaves.size(); ++i) {
        if (i) {
            out << ",";
        }
        out << mgn_leaves[i]->node_id;
    }
    out << "],\"hpc_nodes\":" << count_nodes(root) << "}";
    return static_cast<bool>(out);
}
