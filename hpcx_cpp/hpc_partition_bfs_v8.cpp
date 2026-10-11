#include "hpc_partition_bfs_v8.h"

#include "hpc_ct_ops.h"
#include "hpc_sig_hits_mmap.h"
#include "hpc_partition_children.h"
#include "hpc_v8_compress_flags.h"
#include "hpc_v8_cskey_flags.h"
#include "hpc_rss.h"
#include "hpc_v8_stream_checkpoint.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace {

void v8_profile_log(const char* phase, double dt) {
    if (std::getenv("HPCX_BUILD_PROFILE") == nullptr) return;
    std::cerr << "[hpcx_v8_profile] " << phase << " " << dt << "s\n";
}

int env_int(const char* key, int def) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    return std::atoi(v);
}

bool env_enabled(const char* key, bool def_true) {
    const char* v = std::getenv(key);
    if (v == nullptr) return def_true;
    std::string s(v);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return !(s == "0" || s == "false" || s == "no");
}

int dim_order_key(const std::string& dim_name) {
    static const std::unordered_map<std::string, int> order = {
        {"first1", 1}, {"second1", 2}, {"third1", 3}, {"fourth1", 4}, {"fifth1", 5},
        {"sixth1", 6}, {"seventh1", 7}, {"eighth1", 8}, {"ninth1", 9}, {"tenth1", 10},
    };
    const auto it = order.find(dim_name);
    if (it != order.end()) return it->second;
    if (dim_name.rfind("nth", 0) == 0) {
        const std::string tail = dim_name.substr(3);
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), ::isdigit)) return std::stoi(tail);
    }
    return 1000;
}

std::string nth_dim_name(int n) {
    static const char* names[] = {
        "first1", "second1", "third1", "fourth1", "fifth1",
        "sixth1", "seventh1", "eighth1", "ninth1", "tenth1",
    };
    if (n >= 1 && n <= 10) return names[n - 1];
    return "nth" + std::to_string(n);
}

int nth_from_dim_name(const std::string& dim_name) {
    static const std::unordered_map<std::string, int> order = {
        {"first1", 1}, {"second1", 2}, {"third1", 3}, {"fourth1", 4}, {"fifth1", 5},
        {"sixth1", 6}, {"seventh1", 7}, {"eighth1", 8}, {"ninth1", 9}, {"tenth1", 10},
    };
    const auto it = order.find(dim_name);
    if (it != order.end()) return it->second;
    if (dim_name.rfind("nth", 0) == 0) {
        const std::string tail = dim_name.substr(3);
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), ::isdigit)) return std::stoi(tail);
    }
    return 1000;
}

void set_index_bounds(HpcPartitionNode& part) {
    if (part.one_index.empty()) return;
    part.min_1index = *std::min_element(part.one_index.begin(), part.one_index.end());
    part.max_1index = *std::max_element(part.one_index.begin(), part.one_index.end());
}

struct PartIndexCtx {
    std::unordered_set<int32_t> set;
    std::unordered_map<int32_t, int> rank_map;

    explicit PartIndexCtx(const std::vector<int32_t>& part_index) {
        set.reserve(part_index.size());
        rank_map.reserve(part_index.size());
        for (std::size_t i = 0; i < part_index.size(); ++i) {
            set.insert(part_index[i]);
            rank_map[part_index[i]] = static_cast<int>(i);
        }
    }
};

int partition_depth_proxy_v8(
    const HpcPartitionNode& part,
    const HpcColBuildCache& cache,
    const std::vector<uint32_t>& perm,
    uint32_t begin,
    uint32_t end,
    const PartIndexCtx& pix,
    std::vector<int32_t>& active_scratch) {
    const auto& part_index = part.one_index;
    if (part_index.empty() || begin >= end) return 0;
    int mx = 0;
    for (uint32_t pi = begin; pi < end; ++pi) {
        cache.active_hits_into(perm[pi], pix.set, active_scratch);
        if (!active_scratch.empty()) {
            const int32_t max_hit = *std::max_element(active_scratch.begin(), active_scratch.end());
            const auto rit = pix.rank_map.find(max_hit);
            if (rit != pix.rank_map.end()) mx = std::max(mx, rit->second + 1);
        }
    }
    return std::max(mx, static_cast<int>(part_index.size()));
}

struct SplitQuality { bool ok = false; double max_ratio = 1.0; double entropy = 0.0; };

SplitQuality split_quality_count(int n, const std::map<std::string, int>& counts, double max_child_ratio) {
    SplitQuality out;
    if (n == 0) return out;
    std::vector<int> non_empty;
    for (const auto& kv : counts) if (kv.second > 0) non_empty.push_back(kv.second);
    if (static_cast<int>(non_empty.size()) < 2) return out;
    out.max_ratio = static_cast<double>(*std::max_element(non_empty.begin(), non_empty.end())) / static_cast<double>(n);
    if (out.max_ratio > max_child_ratio) return out;
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

SplitCountsResult split_child_counts_v8(
    uint32_t begin,
    uint32_t end,
    const std::vector<uint32_t>& perm,
    const HpcColBuildCache& cache,
    const std::vector<int32_t>& part_index,
    const PartIndexCtx& pix,
    std::vector<int32_t>& active_scratch,
    int nth,
    int num_segments) {
    SplitCountsResult out;
    out.boundaries = cache.boundaries_nth(begin, end, perm, nth, num_segments);
    out.one_index_partition = hpc_assign_oneindex_adaptive(part_index, out.boundaries, num_segments);
    for (uint32_t pi = begin; pi < end; ++pi) {
        cache.active_hits_into(perm[pi], pix.set, active_scratch);
        const std::string key = hpc_csp_cskey_1index_zipfast_gai(active_scratch, part_index, out.one_index_partition);
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

std::vector<SplitEval> evaluate_split_dims_v8(
    uint32_t begin,
    uint32_t end,
    const std::vector<uint32_t>& perm,
    const HpcColBuildCache& cache,
    const std::vector<int32_t>& part_index,
    const PartIndexCtx& pix,
    std::vector<int32_t>& active_scratch,
    int num_segments,
    int max_nth,
    double max_child_ratio,
    const std::vector<std::string>* dim_names) {
    std::vector<std::string> ranked;
    for (int n = 1; n <= max_nth; ++n) ranked.push_back(nth_dim_name(n));
    if (dim_names != nullptr) {
        std::unordered_set<std::string> allowed(dim_names->begin(), dim_names->end());
        std::vector<std::string> filtered;
        for (const auto& d : ranked) if (allowed.count(d)) filtered.push_back(d);
        ranked = std::move(filtered);
    }
    const int n_cols = static_cast<int>(end - begin);
    std::vector<SplitEval> evals;
    evals.reserve(ranked.size());
    for (const auto& dim_name : ranked) {
        SplitEval ev;
        ev.dim_name = dim_name;
        SplitCountsResult sc = split_child_counts_v8(
            begin, end, perm, cache, part_index, pix, active_scratch,
            nth_from_dim_name(dim_name), num_segments);
        ev.boundaries = std::move(sc.boundaries);
        ev.one_index_partition = std::move(sc.one_index_partition);
        ev.counts = std::move(sc.counts);
        SplitQuality q = split_quality_count(n_cols, ev.counts, max_child_ratio);
        ev.ok = q.ok;
        for (const auto& c : ev.counts) if (c.second > 0) ev.non_empty += 1;
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
        if (!ev.ok) continue;
        if (ev.score > best_score) { best_score = ev.score; best = &ev; }
    }
    if (best != nullptr) {
        out.dim_name = best->dim_name;
        out.boundaries = best->boundaries;
        out.one_index_partition = best->one_index_partition;
        out.valid = true;
        return out;
    }
    std::vector<const SplitEval*> splittable;
    for (const auto& ev : evals) if (ev.non_empty >= 2) splittable.push_back(&ev);
    if (splittable.empty()) return out;
    const SplitEval* forced_best = splittable[0];
    for (const SplitEval* ev : splittable) if (ev->score > forced_best->score) forced_best = ev;
    out.dim_name = forced_best->dim_name;
    out.boundaries = forced_best->boundaries;
    out.one_index_partition = forced_best->one_index_partition;
    out.forced = true;
    out.valid = true;
    return out;
}

std::pair<std::vector<int>, int> k_candidates_for_node(
    int n_cols, int depth, int k_min, int k_max, int min_cols, int num_layers, int root_k) {
    if (root_k >= 0 && depth == 0) return {{root_k}, root_k};
    const int layers_left = std::max(num_layers - depth, 1);
    const double n_ref = std::max(static_cast<double>(n_cols), static_cast<double>(min_cols) * 2.0);
    int k_target = static_cast<int>(std::ceil(
        std::pow(n_ref, 1.0 / layers_left) / std::pow(static_cast<double>(min_cols), 1.0 / layers_left)));
    k_target = std::max(k_min, std::min(k_max, k_target));
    std::vector<int> cands = {k_target};
    if (k_target - 1 >= k_min) cands.push_back(k_target - 1);
    if (k_target + 1 <= k_max) cands.push_back(k_target + 1);
    std::sort(cands.begin(), cands.end(), [k_target](int a, int b) {
        auto key = [k_target](int k) { return std::make_pair(k == k_target ? 0 : 1, std::abs(k - k_target)); };
        return key(a) < key(b);
    });
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    return {cands, k_target};
}

std::pair<std::vector<int>, int> k_candidates_large_part(
    int n_cols, int /*n_global*/, int depth, int k_min, int k_max, int min_cols, int num_layers, int root_k,
    const HpcPartitionBuildConfig& cfg) {
    auto base = k_candidates_for_node(n_cols, depth, k_min, k_max, min_cols, num_layers, root_k);
    if (n_cols < cfg.large_part_cols) return base;
    const int k_lo = std::max(k_min, cfg.large_k_min);
    const int k_hi = std::max(k_max, cfg.large_k_max);
    std::vector<int> ordered;
    auto push_unique = [&](int k) {
        if (k >= k_lo && k <= k_hi && std::find(ordered.begin(), ordered.end(), k) == ordered.end()) ordered.push_back(k);
    };
    push_unique(base.second);
    for (int k = k_lo; k <= k_hi; ++k) push_unique(k);
    for (int k : base.first) push_unique(k);
    return {ordered, base.second};
}

struct AdaptivePick {
    ChosenSplit chosen;
    int seg_k = 0;
    int split_attempts = 0;
    int k_target = 0;
    bool failed = false;
};

AdaptivePick pick_best_split_v8(
    uint32_t begin,
    uint32_t end,
    const std::vector<uint32_t>& perm,
    const HpcColBuildCache& cache,
    const std::vector<int32_t>& part_index,
    const PartIndexCtx& pix,
    std::vector<int32_t>& active_scratch,
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
    const int n_cols = static_cast<int>(end - begin);
    std::vector<std::string> ranked;
    for (int n = 1; n <= max_nth; ++n) ranked.push_back(nth_dim_name(n));
    std::vector<std::string> primary, extra;
    int eff_initial_dims = initial_dims;
    int eff_k_max = k_max;
    if (n_cols >= cfg.large_part_cols && env_enabled("HPC_LARGE_PART_SPLIT", true)) {
        const double ratio = std::max(static_cast<double>(n_cols) / std::max(min_cols, 1), 2.0);
        eff_initial_dims = std::min(cfg.large_max_split_dims, std::max(initial_dims, static_cast<int>(std::ceil(std::log2(ratio))) + 3));
        eff_k_max = std::max(k_max, cfg.large_k_max);
    }
    for (const auto& d : ranked) {
        if (dim_order_key(d) <= eff_initial_dims) primary.push_back(d);
        else extra.push_back(d);
    }
    auto [k_candidates, k_target] = k_candidates_large_part(n_cols, n_global, depth, k_min, eff_k_max, min_cols, num_layers, root_k, cfg);
    out.k_target = k_target;
    ChosenSplit fallback;
    int fallback_k = 0;
    for (const int k : k_candidates) {
        auto evals_primary = evaluate_split_dims_v8(
            begin, end, perm, cache, part_index, pix, active_scratch, k, max_nth, max_child_ratio, &primary);
        out.split_attempts += static_cast<int>(evals_primary.size());
        ChosenSplit chosen = choose_split_from_evals(evals_primary);
        if (chosen.valid && !chosen.forced) { out.chosen = chosen; out.seg_k = k; return out; }
        if (chosen.valid && !fallback.valid) { fallback = chosen; fallback_k = k; }
        if (!extra.empty()) {
            auto evals_extra = evaluate_split_dims_v8(
                begin, end, perm, cache, part_index, pix, active_scratch, k, max_nth, max_child_ratio, &extra);
            out.split_attempts += static_cast<int>(evals_extra.size());
            evals_primary.insert(evals_primary.end(), evals_extra.begin(), evals_extra.end());
            ChosenSplit chosen2 = choose_split_from_evals(evals_primary);
            if (chosen2.valid && !chosen2.forced) { out.chosen = chosen2; out.seg_k = k; return out; }
            if (chosen2.valid && !fallback.valid) { fallback = chosen2; fallback_k = k; }
        }
    }
    if (fallback.valid) { out.chosen = fallback; out.seg_k = fallback_k; return out; }
    out.failed = true;
    return out;
}

namespace {

struct SplitBucket {
    std::vector<uint32_t> col_indices;
    std::vector<int32_t> hits_raw;
};

void finalize_child_one_index(
    HpcPartitionNode& child,
    SplitBucket& bucket,
    bool union_set,
    bool keep_parent_so,
    const std::vector<int32_t>& parent_so,
    const HpcColBuildCache& cache,
    const PartIndexCtx& pix,
    std::vector<int32_t>& active_scratch) {
    if (keep_parent_so) {
        child.one_index = parent_so;
        return;
    }
    if (union_set) {
        std::unordered_set<int32_t> hit_set;
        hit_set.reserve(bucket.col_indices.size() * 4 + 1);
        for (const uint32_t col_idx : bucket.col_indices) {
            cache.active_hits_into(col_idx, pix.set, active_scratch);
            hit_set.insert(active_scratch.begin(), active_scratch.end());
        }
        child.one_index.assign(hit_set.begin(), hit_set.end());
        std::sort(child.one_index.begin(), child.one_index.end());
        return;
    }
    child.one_index = std::move(bucket.hits_raw);
    std::sort(child.one_index.begin(), child.one_index.end());
    child.one_index.erase(
        std::unique(child.one_index.begin(), child.one_index.end()),
        child.one_index.end());
}

}  // namespace

void apply_adaptive_split_v8(
    HpcPartitionNode& part,
    std::vector<uint32_t>& perm,
    uint32_t begin,
    uint32_t end,
    const HpcColBuildCache& cache,
    const PartIndexCtx& pix,
    std::vector<int32_t>& active_scratch,
    const std::vector<int32_t>& boundaries,
    const HpcOneIndexPartition& one_index_partition,
    HpcPartitionV8Context& ctx) {
    part.segment_boundaries = boundaries;
    part.one_index_partition.clear();
    part.oip_segments.clear();
    const bool union_set = hpc_v8_compress::split_union_set_enabled();
    const bool keep_parent_so = env_enabled("HPC_NO_LOCAL_REDUCTION", false);
    hpc_node_clear_children(part);
    uint32_t pos = begin;

    if (hpc_v8_cskey::packed_cskey_enabled()) {
        std::unordered_map<HpcPartCSKey, SplitBucket> pk_buckets;
        pk_buckets.reserve(static_cast<std::size_t>(end - begin) / 4 + 1);
        for (uint32_t pi = begin; pi < end; ++pi) {
            const uint32_t col_idx = perm[pi];
            cache.active_hits_into(col_idx, pix.set, active_scratch);
            const HpcPartCSKey key = hpc_csp_cskey_packed_1index_zipfast_gai(
                active_scratch, part.one_index, one_index_partition);
            SplitBucket& bucket = pk_buckets[key];
            bucket.col_indices.push_back(col_idx);
            if (!union_set) {
                bucket.hits_raw.insert(bucket.hits_raw.end(), active_scratch.begin(), active_scratch.end());
            }
        }
        std::vector<HpcPartCSKey> ordered;
        ordered.reserve(pk_buckets.size());
        for (const auto& kv : pk_buckets) {
            ordered.push_back(kv.first);
        }
        if (hpc_v8_cskey::cskey_sorted_buckets()) {
            const int n_parts = static_cast<int>(one_index_partition.size());
            std::sort(ordered.begin(), ordered.end(), [n_parts](HpcPartCSKey a, HpcPartCSKey b) {
                return hpc_cskey_format_packed(a, n_parts) < hpc_cskey_format_packed(b, n_parts);
            });
        }
        for (const HpcPartCSKey pk : ordered) {
            auto child = std::make_unique<HpcPartitionNode>();
            SplitBucket& bucket = pk_buckets[pk];
            const uint32_t cb = pos;
            for (const uint32_t col_idx : bucket.col_indices) {
                perm[pos++] = col_idx;
            }
            finalize_child_one_index(
                *child, bucket, union_set, keep_parent_so, part.one_index, cache, pix, active_scratch);
            ctx.ranges[child.get()] = {cb, pos};
            part.next_partition_pk[pk] = std::move(child);
        }
        return;
    }

    std::unordered_map<std::string, SplitBucket> buckets;
    buckets.reserve(static_cast<std::size_t>(end - begin) / 4 + 1);
    for (uint32_t pi = begin; pi < end; ++pi) {
        const uint32_t col_idx = perm[pi];
        cache.active_hits_into(col_idx, pix.set, active_scratch);
        const std::string key = hpc_csp_cskey_1index_zipfast_gai(
            active_scratch, part.one_index, one_index_partition);
        SplitBucket& bucket = buckets[key];
        bucket.col_indices.push_back(col_idx);
        if (!union_set) {
            bucket.hits_raw.insert(bucket.hits_raw.end(), active_scratch.begin(), active_scratch.end());
        }
    }
    for (auto& kv : buckets) {
        auto child = std::make_unique<HpcPartitionNode>();
        SplitBucket& bucket = kv.second;
        const uint32_t cb = pos;
        for (const uint32_t col_idx : bucket.col_indices) {
            perm[pos++] = col_idx;
        }
        finalize_child_one_index(
            *child, bucket, union_set, keep_parent_so, part.one_index, cache, pix, active_scratch);
        ctx.ranges[child.get()] = {cb, pos};
        part.next_partition[kv.first] = std::move(child);
    }
}

void assign_node_ids(HpcPartitionNode& node, int* next_id) {
    node.node_id = (*next_id)++;
    hpc_node_for_each_child(node, [&](HpcPartitionNode& child) { assign_node_ids(child, next_id); });
}

}  // namespace

HpcPartitionBuildResult hpc_run_partition_bfs_v8(
    const std::string& sig_native_path,
    const HpcPartitionBuildConfig& cfg,
    HpcPartitionV8Context* v8_ctx,
    std::string* err) {
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

    HpcPartitionBuildResult result;
    if (v8_ctx == nullptr) return result;

    HpcSigHitsMmap hits_mmap;
    if (!HpcSigHitsMmap::build_from_sigb(sig_native_path, &hits_mmap, err)) {
        return result;
    }

    int max_hits = 0;
    for (std::size_t i = 0; i + 1 < hits_mmap.hit_off.size(); ++i) {
        max_hits = std::max(
            max_hits,
            static_cast<int>(hits_mmap.hit_off[i + 1] - hits_mmap.hit_off[i]));
    }
    const int max_nth = std::min(std::max(max_hits, 1), c.max_split_dims);

    v8_ctx->cache = HpcColBuildCache::from_sig_hits_mmap(std::move(hits_mmap), max_nth);
    const std::size_t n_cols = v8_ctx->cache.size();

    auto root = std::make_unique<HpcPartitionNode>();
    std::unordered_set<int32_t> root_hit_set;
    root_hit_set.reserve(65536);
    for (std::size_t i = 0; i < n_cols; ++i) {
        const int32_t* hits = nullptr;
        int len = 0;
        v8_ctx->cache.hits_span(static_cast<uint32_t>(i), &hits, &len);
        if (hits == nullptr || len <= 0) {
            continue;
        }
        for (int j = 0; j < len; ++j) {
            root_hit_set.insert(hits[j]);
        }
    }
    root->one_index.assign(root_hit_set.begin(), root_hit_set.end());
    root_hit_set.clear();
    std::sort(root->one_index.begin(), root->one_index.end());
    v8_ctx->col_perm.resize(n_cols);
    for (std::size_t i = 0; i < n_cols; ++i) {
        v8_ctx->col_perm[i] = static_cast<uint32_t>(i);
    }
    v8_profile_log("sig_hits_mmap_to_cache", 0.0);

    set_index_bounds(*root);
    v8_ctx->ranges[root.get()] = {0, static_cast<uint32_t>(n_cols)};

    const int n_cols_i = static_cast<int>(n_cols);
    const int last_hpc_layer = c.num_hpc_layers;
    const int max_extend_layer = last_hpc_layer + c.depth_extra_l;

    struct QueueItem { HpcPartitionNode* part; int depth; };
    std::deque<QueueItem> queue;
    queue.push_back({root.get(), 0});
    std::vector<int32_t> active_scratch;
    active_scratch.reserve(64);

    auto leaf_now = [&](HpcPartitionNode& part, int depth, uint32_t begin, uint32_t end) {
        PartIndexCtx pix(part.one_index);
        const int dp = partition_depth_proxy_v8(
            part, v8_ctx->cache, v8_ctx->col_perm, begin, end, pix, active_scratch);
        const int n = static_cast<int>(end - begin);
        if (depth < last_hpc_layer) {
            if (n <= c.min_cols) return std::make_tuple(true, dp);
            return std::make_tuple(false, dp);
        }
        if (!env_enabled("HPC_NO_LOCAL_REDUCTION", false) && dp > c.depth_stop && depth < max_extend_layer) {
            return std::make_tuple(false, dp);
        }
        return std::make_tuple(true, dp);
    };

    auto mark_mgn_leaf = [&](HpcPartitionNode* part, int depth, uint32_t begin, uint32_t end) {
        part->is_mgn_leaf = true;
        part->stop_depth = depth;
        PartIndexCtx pix(part->one_index);
        part->stop_depth_proxy = partition_depth_proxy_v8(
            *part, v8_ctx->cache, v8_ctx->col_perm, begin, end, pix, active_scratch);
        part->ids.clear();
        part->ids.reserve(static_cast<std::size_t>(end - begin));
        for (uint32_t pi = begin; pi < end; ++pi) {
            part->ids.push_back(v8_ctx->cache.col_id(v8_ctx->col_perm[pi]));
        }
        v8_ctx->stash_leaf_one_index(*part);
        result.mgn_leaves.push_back(part);
    };

    const bool one_level = env_enabled("HPC_ONE_LEVEL", false);
    if (env_enabled("HPC_NO_LOCAL_REDUCTION", false)) {
        std::fprintf(
            stderr,
            "[hpc_no_local_reduction] children keep parent SO len=%zu\n",
            root->one_index.size());
    }
    if (env_enabled("HPC_GLOBAL_OHS", false)) {
        std::fprintf(
            stderr,
            "[hpc_global_ohs] single leaf n_cols=%zu one_index_len=%zu\n",
            n_cols,
            root->one_index.size());
        mark_mgn_leaf(root.get(), 0, 0, static_cast<uint32_t>(n_cols));
    } else while (!queue.empty()) {
        QueueItem item = queue.front();
        queue.pop_front();
        HpcPartitionNode* part = item.part;
        const int depth = item.depth;
        const auto rit = v8_ctx->ranges.find(part);
        if (rit == v8_ctx->ranges.end()) continue;
        const auto [begin, end] = rit->second;
        const auto [stop, dp] = leaf_now(*part, depth, begin, end);
        (void)dp;
        if (stop) {
            mark_mgn_leaf(part, depth, begin, end);
            continue;
        }

        PartIndexCtx pix(part->one_index);
        const int root_k = (depth == 0) ? c.root_num_segments : -1;
        AdaptivePick pick = pick_best_split_v8(
            begin, end, v8_ctx->col_perm, v8_ctx->cache, part->one_index, pix, active_scratch, max_nth,
            c.adaptive_k_min, c.adaptive_k_max, c.max_child_ratio, c.initial_split_dims,
            depth, c.min_cols, c.num_hpc_layers, n_cols_i, root_k, c);
        if (pick.failed || !pick.chosen.valid) {
            mark_mgn_leaf(part, depth, begin, end);
            continue;
        }

        part->split_dim = pick.chosen.dim_name;
        part->split_k = pick.seg_k > 0 ? pick.seg_k : pick.k_target;
        part->split_forced = pick.chosen.forced;
        apply_adaptive_split_v8(
            *part, v8_ctx->col_perm, begin, end, v8_ctx->cache, pix, active_scratch,
            pick.chosen.boundaries, pick.chosen.one_index_partition, *v8_ctx);
        part->oip_one_index = std::move(part->one_index);
        if (hpc_v8_stream_checkpoint_enabled()) {
            hpc_v8_stream_ckpt_spill_oip(*v8_ctx, *part);
        }

        hpc_node_for_each_child_ptr(*part, [&](HpcPartitionNode* child) {
            if (child == nullptr) {
                return;
            }
            set_index_bounds(*child);
            const auto cit = v8_ctx->ranges.find(child);
            if (cit == v8_ctx->ranges.end()) {
                return;
            }
            const auto [c_stop, c_dp] = leaf_now(*child, depth + 1, cit->second.first, cit->second.second);
            (void)c_dp;
            if (one_level || c_stop) {
                if (one_level) {
                    std::fprintf(
                        stderr,
                        "[hpc_one_level] leaf depth=%d n_cols=%u one_index_len=%zu\n",
                        depth + 1,
                        cit->second.second - cit->second.first,
                        child->one_index.size());
                }
                mark_mgn_leaf(child, depth + 1, cit->second.first, cit->second.second);
            } else {
                queue.push_back({child, depth + 1});
            }
        });
        if (hpc_v8_stream_checkpoint_enabled()) {
            hpc_v8_stream_ckpt_trim_terminal_parent(*v8_ctx, *part);
        }
    }

    if (hpc_v8_stream_checkpoint_enabled()) {
        hpc_v8_stream_ckpt_finish_partition(*v8_ctx);
    }
    if (std::getenv("HPCX_BUILD_PROFILE") != nullptr) {
        v8_profile_log("partition_vmrss_kb", static_cast<double>(vmrss_kb()));
    }

    int next_id = 0;
    assign_node_ids(*root, &next_id);

    std::vector<HpcPartitionNode*> unique_leaves;
    std::unordered_map<HpcPartitionNode*, bool> seen;
    for (HpcPartitionNode* leaf : result.mgn_leaves) {
        if (leaf != nullptr && !seen[leaf]) { seen[leaf] = true; unique_leaves.push_back(leaf); }
    }
    result.mgn_leaves = std::move(unique_leaves);
    if (env_enabled("HPC_NO_LOCAL_REDUCTION", false)) {
        std::fprintf(
            stderr,
            "[hpc_no_local_reduction] leaves=%zu nodes=%zu\n",
            result.mgn_leaves.size(),
            v8_ctx->ranges.size());
    }
    result.root = std::move(root);
    result.hpc_nodes = static_cast<int>(v8_ctx->ranges.size());
    return result;
}
