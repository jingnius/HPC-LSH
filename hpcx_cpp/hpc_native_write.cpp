#include "hpc_native_write.h"

#include "hpc_ct_ops.h"
#include "hpc_mgn_compact.h"
#include "hpc_partition_bfs_v8.h"
#include "hpc_partition_build.h"
#include "hpc_partition_children.h"
#include "hpc_partition_flat.h"
#include "hpc_v8_stream_checkpoint.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

struct FlatPoolDedup {
    std::unordered_map<std::string, std::pair<int, int>> one;
    std::unordered_map<std::string, std::pair<int, int>> oip;
};

FlatPoolDedup& flat_pool_dedup() {
    static thread_local FlatPoolDedup dedup;
    return dedup;
}

std::string ints_key(const std::vector<int>& vals) {
    if (vals.empty()) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(vals.data()), vals.size() * sizeof(int));
}

void append_oip_to_flat(
    HpcFlatPart& pf,
    const HpcPartitionNode& part,
    const std::vector<int32_t>& one_index,
    HpcPartitionV8Context* v8_ctx) {
    std::vector<int> chunk;
    int np_out = 0;
    auto commit = [&]() {
        std::string key = std::to_string(np_out) + ":" + ints_key(chunk);
        auto& seen = flat_pool_dedup().oip;
        const auto it = seen.find(key);
        if (it != seen.end()) {
            pf.oip_off.push_back(it->second.first);
            pf.oip_num_parts.push_back(it->second.second);
            return;
        }
        const int off = static_cast<int>(pf.oip_pool.size());
        pf.oip_off.push_back(off);
        pf.oip_num_parts.push_back(np_out);
        pf.oip_pool.insert(pf.oip_pool.end(), chunk.begin(), chunk.end());
        seen.emplace(std::move(key), std::make_pair(off, np_out));
    };
    if (!part.oip_segments.empty()) {
        np_out = static_cast<int>(part.oip_segments.size());
        for (int seg = 0; seg < np_out; ++seg) {
            const auto& hits32 = part.oip_segments[static_cast<std::size_t>(seg)];
            std::vector<int> hits(hits32.begin(), hits32.end());
            std::sort(hits.begin(), hits.end());
            chunk.push_back(static_cast<int>(hits.size()));
            chunk.insert(chunk.end(), hits.begin(), hits.end());
        }
        commit();
        return;
    }
    HpcOneIndexPartition oip = part.one_index_partition;
    if (oip.empty()) {
        const int k = hpc_partition_segment_count(part);
        static thread_local std::vector<int32_t> oip_ckpt_tls;
        const std::vector<int32_t>& oip_use = [&]() -> const std::vector<int32_t>& {
            if (v8_ctx != nullptr && v8_ctx->stream_ckpt_active &&
                hpc_v8_stream_ckpt_oip_view(*v8_ctx, part, oip_ckpt_tls)) {
                return oip_ckpt_tls;
            }
            if (v8_ctx != nullptr) {
                const std::vector<int32_t>& pooled = v8_ctx->node_oip_one_index_view(part);
                if (!pooled.empty()) {
                    return pooled;
                }
            }
            if (!part.oip_one_index.empty()) {
                return part.oip_one_index;
            }
            return one_index;
        }();
        if (k <= 0 || oip_use.empty()) {
            commit();
            return;
        }
        oip = hpc_assign_oneindex_adaptive(oip_use, part.segment_boundaries, k);
    }
    if (oip.empty()) {
        commit();
        return;
    }
    int max_seg = 0;
    for (const auto& kv : oip) {
        max_seg = std::max(max_seg, kv.first);
    }
    np_out = max_seg + 1;
    for (int seg = 0; seg <= max_seg; ++seg) {
        std::vector<int> hits;
        const auto it = oip.find(seg);
        if (it != oip.end()) {
            hits.assign(it->second.begin(), it->second.end());
            std::sort(hits.begin(), hits.end());
        }
        chunk.push_back(static_cast<int>(hits.size()));
        chunk.insert(chunk.end(), hits.begin(), hits.end());
    }
    commit();
}

void append_one_index_to_flat(
    HpcFlatPart& pf,
    const HpcPartitionNode& part,
    HpcPartitionV8Context* v8_ctx) {
    const std::vector<int32_t>& one_src = hpc_v8_node_one_index(part, v8_ctx);
    std::vector<int> one_sorted(one_src.begin(), one_src.end());
    std::sort(one_sorted.begin(), one_sorted.end());
    one_sorted.erase(std::unique(one_sorted.begin(), one_sorted.end()), one_sorted.end());
    std::string key = ints_key(one_sorted);
    auto& seen = flat_pool_dedup().one;
    const auto it = seen.find(key);
    if (it != seen.end()) {
        pf.one_off.push_back(it->second.first);
        pf.one_len.push_back(it->second.second);
    } else {
        const int off = static_cast<int>(pf.one_index_pool.size());
        const int len = static_cast<int>(one_sorted.size());
        pf.one_off.push_back(off);
        pf.one_len.push_back(len);
        pf.one_index_pool.insert(pf.one_index_pool.end(), one_sorted.begin(), one_sorted.end());
        seen.emplace(std::move(key), std::make_pair(off, len));
    }
    if (v8_ctx != nullptr) {
        if (part.mgn_key >= 0) {
            v8_ctx->release_leaf_one_index(&part);
        } else if (hpc_node_has_children(part)) {
            hpc_v8_drop_merged_one_index_cache(&part);
        }
    }
}

void append_routes_to_flat(
    HpcFlatPart& pf,
    const HpcPartitionNode& part,
    const std::unordered_map<int, int>& node_id_to_flat) {
    pf.route_ent_off.push_back(static_cast<int>(pf.route_mask.size()));
    int route_count = 0;
    std::vector<std::pair<int, std::vector<int>>> route_entries;
    route_entries.reserve(part.route.size());
    for (const auto& kv : part.route) {
        route_entries.emplace_back(kv.first, kv.second);
    }
    std::sort(route_entries.begin(), route_entries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    for (const auto& ent : route_entries) {
        std::vector<int> child_flat_ids;
        child_flat_ids.reserve(ent.second.size());
        for (const int child_node_id : ent.second) {
            const auto cit = node_id_to_flat.find(child_node_id);
            if (cit != node_id_to_flat.end()) {
                child_flat_ids.push_back(cit->second);
            }
        }
        if (child_flat_ids.empty()) {
            continue;
        }
        pf.route_mask.push_back(ent.first);
        pf.route_child_off.push_back(static_cast<int>(pf.route_child_ids.size()));
        pf.route_child_ids.insert(pf.route_child_ids.end(), child_flat_ids.begin(), child_flat_ids.end());
        route_count += 1;
    }
    pf.route_ent_len.push_back(route_count);
}

void release_partition_node_payload(HpcPartitionNode& node, HpcPartitionV8Context* v8_ctx) {
    if (v8_ctx != nullptr && node.mgn_key >= 0) {
        v8_ctx->release_leaf_one_index(&node);
    }
    node.one_index.clear();
    node.one_index.shrink_to_fit();
    node.ids.clear();
    node.ids.shrink_to_fit();
    node.segment_boundaries.clear();
    node.segment_boundaries.shrink_to_fit();
    node.one_index_partition.clear();
    node.oip_segments.clear();
    node.oip_segments.shrink_to_fit();
    node.oip_one_index.clear();
    node.oip_one_index.shrink_to_fit();
    node.route.clear();
    node.split_dim.clear();
    node.split_dim.shrink_to_fit();
    if (v8_ctx != nullptr && hpc_node_has_children(node)) {
        hpc_v8_drop_merged_one_index_cache(&node);
    }
}

void strip_partition_tree(HpcPartitionNode& node) {
    hpc_node_clear_children(node);
}

bool build_partition_flat_order(
    HpcPartitionNode& root,
    std::unordered_map<int, int>& node_id_to_flat,
    std::vector<HpcPartitionNode*>& flat_order) {
    std::unordered_map<const HpcPartitionNode*, int> idmap;
    flat_order.clear();
    node_id_to_flat.clear();

    std::function<int(HpcPartitionNode*)> assign_ids;
    assign_ids = [&](HpcPartitionNode* part) -> int {
        if (part == nullptr) {
            return 0;
        }
        const auto it = idmap.find(part);
        if (it != idmap.end()) {
            return it->second;
        }
        const int nid = static_cast<int>(flat_order.size());
        idmap[part] = nid;
        node_id_to_flat[part->node_id] = nid;
        flat_order.push_back(part);
        hpc_node_for_each_child_ptr(*part, [&](HpcPartitionNode* child) {
            if (child != nullptr && child->mgn_key < 0) {
                assign_ids(child);
            }
        });
        return nid;
    };

    assign_ids(&root);

    for (std::size_t pi = 0; pi < flat_order.size(); ++pi) {
        HpcPartitionNode* part = flat_order[pi];
        for (const auto& ent : part->route) {
            for (const int child_node_id : ent.second) {
                if (HpcPartitionNode* child = hpc_node_find_child_by_node_id(*part, child_node_id)) {
                    assign_ids(child);
                }
            }
        }
    }
    return true;
}

bool fill_partition_flat_part(
    HpcPartitionNode& root,
    HpcFlatPart& pf,
    HpcNativeBuildStats& stats,
    HpcPartitionV8Context* v8_ctx,
    std::string* err) {
    std::unordered_map<int, int> node_id_to_flat;
    std::vector<HpcPartitionNode*> flat_order;
    build_partition_flat_order(root, node_id_to_flat, flat_order);

    pf = HpcFlatPart{};
    flat_pool_dedup() = {};
    pf.root_node = node_id_to_flat.count(root.node_id) ? node_id_to_flat.at(root.node_id) : 0;
    pf.one_off.reserve(flat_order.size());
    pf.one_len.reserve(flat_order.size());
    pf.oip_off.reserve(flat_order.size());
    pf.oip_num_parts.reserve(flat_order.size());
    pf.route_ent_off.reserve(flat_order.size());
    pf.route_ent_len.reserve(flat_order.size());
    pf.node_is_leaf.reserve(flat_order.size());
    pf.node_mgn_key.reserve(flat_order.size());

    int route_entries = 0;
    int leaf_nodes = 0;
    for (HpcPartitionNode* part : flat_order) {
        const std::vector<int32_t>& one_src = hpc_v8_node_one_index(*part, v8_ctx);
        append_one_index_to_flat(pf, *part, v8_ctx);
        append_oip_to_flat(pf, *part, one_src, v8_ctx);
        append_routes_to_flat(pf, *part, node_id_to_flat);
        route_entries += pf.route_ent_len.back();
        const bool is_leaf = part->mgn_key >= 0;
        pf.node_is_leaf.push_back(is_leaf ? 1 : 0);
        pf.node_mgn_key.push_back(part->mgn_key);
        if (is_leaf) {
            leaf_nodes += 1;
        }
        release_partition_node_payload(*part, v8_ctx);
    }
    pf.route_child_off.push_back(static_cast<int>(pf.route_child_ids.size()));

    stats.partition_nodes = static_cast<int>(flat_order.size());
    stats.route_entries = route_entries;
    stats.leaf_nodes = leaf_nodes;
    stats.index_pool_len = static_cast<int>(pf.one_index_pool.size());
    (void)err;
    return true;
}

bool partition_tree_to_flat(
    HpcPartitionNode& root,
    std::vector<unsigned char>& blob,
    HpcNativeBuildStats& stats,
    std::string* err,
    HpcPartitionV8Context* v8_ctx) {
    if (v8_ctx != nullptr && v8_ctx->partition_preflat_done) {
        blob = partition_flat_encode(v8_ctx->preflat_part);
        stats.partition_nodes = v8_ctx->preflat_partition_nodes;
        stats.route_entries = v8_ctx->preflat_route_entries;
        stats.leaf_nodes = v8_ctx->preflat_leaf_nodes;
        stats.index_pool_len = v8_ctx->preflat_index_pool_len;
        stats.partition_flat_bytes = static_cast<int>(blob.size());
        strip_partition_tree(root);
        hpc_v8_clear_merged_one_index_cache();
        v8_ctx->clear_preflat();
        (void)err;
        return true;
    }

    HpcFlatPart pf;
    if (!fill_partition_flat_part(root, pf, stats, v8_ctx, err)) {
        return false;
    }
    strip_partition_tree(root);
    if (v8_ctx != nullptr) {
        hpc_v8_clear_merged_one_index_cache();
    }

    blob = partition_flat_encode(pf);
    stats.partition_flat_bytes = static_cast<int>(blob.size());
    (void)err;
    return true;
}

bool v8_preflat_partition_tree_impl(
    HpcPartitionNode& root,
    HpcPartitionV8Context& ctx,
    HpcNativeBuildStats& stats,
    std::string* err) {
    if (!fill_partition_flat_part(root, ctx.preflat_part, stats, &ctx, err)) {
        return false;
    }
    ctx.preflat_partition_nodes = stats.partition_nodes;
    ctx.preflat_route_entries = stats.route_entries;
    ctx.preflat_leaf_nodes = stats.leaf_nodes;
    ctx.preflat_index_pool_len = stats.index_pool_len;
    ctx.partition_preflat_done = true;
    strip_partition_tree(root);
    return true;
}

int env_int_local(const char* key, int def) {
    const char* v = std::getenv(key);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    return std::atoi(v);
}

bool env_enabled_local(const char* key, bool def_true) {
    const char* v = std::getenv(key);
    if (v == nullptr) {
        return def_true;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y';
}

enum class MgnStorageKind { PACKED, MATRIX, COMPACT };

struct MgnSelectCfg {
    int depth_stop = 50;
    int nary_min_cols = 2048;
    int flat_max_cols = 512;
    int nary_max_L = 64;
};

MgnStorageKind select_mgn_storage(const HpcFlatMgnLeaf& leaf, const MgnSelectCfg& cfg) {
    const int L = leaf.L > 0 ? leaf.L : 1;
    const int n = leaf.n_cols > 0 ? leaf.n_cols : static_cast<int>(leaf.rows.size());
    if (L > cfg.nary_max_L) {
        return MgnStorageKind::MATRIX;
    }
    if (n <= cfg.flat_max_cols) {
        return MgnStorageKind::PACKED;
    }
    if (L <= cfg.nary_max_L && n >= cfg.nary_min_cols && leaf.stop_depth_proxy <= cfg.depth_stop) {
        return MgnStorageKind::COMPACT;
    }
    return MgnStorageKind::PACKED;
}

MgnCompactTree mgn_rows_to_compact(const HpcFlatMgnLeaf& leaf) {
    const int L = leaf.L > 0 ? leaf.L : 1;
    const int n = static_cast<int>(leaf.rows.size());
    std::vector<int> cids(static_cast<std::size_t>(n));
    std::vector<int> hits(static_cast<std::size_t>(n));
    std::vector<int> path_lens(static_cast<std::size_t>(n), L);
    std::vector<int> path_bits(static_cast<std::size_t>(n) * static_cast<std::size_t>(L));
    for (int i = 0; i < n; ++i) {
        const HpcFlatMgnRow& row = leaf.rows[static_cast<std::size_t>(i)];
        cids[static_cast<std::size_t>(i)] = static_cast<int>(row.cid);
        hits[static_cast<std::size_t>(i)] = row.hit;
        for (int j = 0; j < L; ++j) {
            const int bit_pos = L - 1 - j;
            path_bits[static_cast<std::size_t>(i) * static_cast<std::size_t>(L) + static_cast<std::size_t>(j)] =
                static_cast<int>((row.path_bits >> bit_pos) & 1ULL);
        }
    }
    return mgn_flat_to_compact(
        cids.data(), hits.data(), path_lens.data(), path_bits.data(), n);
}

std::vector<HpcFlatMgnRow> resolve_mgn_leaf_rows(
    HpcFlatMgnLeaf& leaf,
    HpcPartitionV8Context* v8_ctx) {
    if (!leaf.rows.empty()) {
        return std::move(leaf.rows);
    }
    if (v8_ctx == nullptr || leaf.mgn_key < 0) {
        return {};
    }
    if (static_cast<std::size_t>(leaf.mgn_key) >= v8_ctx->mgn_stream_idx.size()) {
        return {};
    }
    const std::vector<uint32_t>& idxs =
        v8_ctx->mgn_stream_idx[static_cast<std::size_t>(leaf.mgn_key)];
    if (idxs.empty()) {
        return {};
    }
    std::vector<HpcFlatMgnRow> rows;
    rows.reserve(idxs.size());
    for (const uint32_t idx : idxs) {
        rows.push_back(v8_ctx->mgn_stream_rows[static_cast<std::size_t>(idx)]);
    }
    return rows;
}

bool attach_mgn_to_parts(
    std::vector<HpcFlatMgnLeaf>& mgn_leaves,
    HpcxWriteParts& parts,
    std::string* err,
    HpcPartitionV8Context* v8_ctx) {
    struct FlatItem {
        int key = 0;
        int L = 0;
        std::vector<HpcFlatMgnRow> rows;
    };
    struct WideLeaf {
        int key = 0;
        int L = 0;
        int n_words = 0;
        std::vector<HpcFlatMgnRow> rows;
        std::vector<uint64_t> bits;
    };
    std::vector<FlatItem> packed_items;
    std::map<int, std::vector<WideLeaf>> wide_by_words;
    std::map<int, std::vector<std::pair<int, std::vector<HpcFlatMgnRow>>>> matrix_by_l;
    std::vector<std::pair<int, MgnCompactTree>> compact_items;

    const bool build_v7 = env_enabled_local("HPC_BUILD_V7", false);
    MgnSelectCfg sel_cfg;
    if (build_v7) {
        sel_cfg.depth_stop = env_int_local("HPC_DEPTH_STOP", 50);
        sel_cfg.nary_min_cols = env_int_local("HPC_MGN_NARY_MIN_COLS", 2048);
        sel_cfg.flat_max_cols = env_int_local("HPC_MGN_FLAT_MAX_COLS", 512);
        sel_cfg.nary_max_L = env_int_local("HPC_MGN_NARY_MAX_L", 64);
    }

    for (HpcFlatMgnLeaf& leaf : mgn_leaves) {
        if (leaf.store_compact) {
            compact_items.push_back({leaf.mgn_key, std::move(leaf.compact)});
            continue;
        }
        std::vector<HpcFlatMgnRow> rows = resolve_mgn_leaf_rows(leaf, v8_ctx);
        if (rows.empty()) {
            continue;
        }
        HpcFlatMgnLeaf work_leaf = std::move(leaf);
        work_leaf.rows = std::move(rows);
        FlatItem item;
        item.key = work_leaf.mgn_key;
        item.L = work_leaf.L > 0 ? work_leaf.L : static_cast<int>(work_leaf.rows[0].path_bits > 0 ? 64 : 1);
        if (item.L > 64) {
            const int n_words = work_leaf.wide_words > 0 ? work_leaf.wide_words : ((item.L + 63) >> 6);
            if (work_leaf.wide_bits.size() != work_leaf.rows.size() * static_cast<std::size_t>(n_words)) {
                if (err) {
                    *err = "wide leaf bit words missing";
                }
                return false;
            }
            WideLeaf wl;
            wl.key = item.key;
            wl.L = item.L;
            wl.n_words = n_words;
            wl.rows = std::move(work_leaf.rows);
            wl.bits = std::move(work_leaf.wide_bits);
            wide_by_words[n_words].push_back(std::move(wl));
            continue;
        }
        if (build_v7) {
            const MgnStorageKind st = select_mgn_storage(work_leaf, sel_cfg);
            if (st == MgnStorageKind::COMPACT) {
                compact_items.push_back({item.key, mgn_rows_to_compact(work_leaf)});
                continue;
            }
            if (st == MgnStorageKind::MATRIX) {
                matrix_by_l[item.L].push_back({item.key, std::move(work_leaf.rows)});
                continue;
            }
            item.rows = std::move(work_leaf.rows);
            packed_items.push_back(std::move(item));
            continue;
        }
        item.rows = std::move(work_leaf.rows);
        packed_items.push_back(std::move(item));
    }
    mgn_leaves.clear();
    mgn_leaves.shrink_to_fit();
    if (v8_ctx != nullptr) {
        v8_ctx->clear_mgn_stream();
    }

    if (!packed_items.empty()) {
        int total = 0;
        for (const FlatItem& item : packed_items) {
            total += static_cast<int>(item.rows.size());
        }
        OwnedPackedPool pool;
        pool.cids.assign(static_cast<std::size_t>(total), 0);
        pool.bits.assign(static_cast<std::size_t>(total), 0);
        pool.spop.assign(static_cast<std::size_t>(total), 0);
        pool.leaf_off.assign(packed_items.size(), 0);
        pool.leaf_n.assign(packed_items.size(), 0);
        int pos = 0;
        for (std::size_t li = 0; li < packed_items.size(); ++li) {
            const FlatItem& item = packed_items[li];
            pool.leaf_off[li] = pos;
            pool.leaf_n[li] = static_cast<int32_t>(item.rows.size());
            for (const HpcFlatMgnRow& row : item.rows) {
                pool.cids[static_cast<std::size_t>(pos)] = static_cast<int32_t>(row.cid);
                pool.bits[static_cast<std::size_t>(pos)] = row.path_bits;
                pool.spop[static_cast<std::size_t>(pos)] = static_cast<uint8_t>(row.hit);
                pos += 1;
            }
        }
        const int pool_id = static_cast<int>(parts.packed_pools.size());
        parts.packed_pools.push_back(std::move(pool));
        for (std::size_t li = 0; li < packed_items.size(); ++li) {
            const FlatItem& item = packed_items[li];
            HpcxMgnEntry ent;
            ent.kind = HpcxMgnEntry::PACKED;
            ent.pool_id = pool_id;
            ent.leaf_idx = static_cast<int>(li);
            ent.L = item.L;
            ent.n = static_cast<int>(item.rows.size());
            parts.tree_mgn[item.key] = ent;
        }
        packed_items.clear();
        packed_items.shrink_to_fit();
    }

    for (const auto& w_kv : wide_by_words) {
        const int n_words = w_kv.first;
        const auto& items = w_kv.second;
        int total = 0;
        for (const WideLeaf& wl : items) {
            total += static_cast<int>(wl.rows.size());
        }
        OwnedWidePool pool;
        pool.n_words = n_words;
        pool.cids.assign(static_cast<std::size_t>(total), 0);
        pool.bits.assign(static_cast<std::size_t>(total) * static_cast<std::size_t>(n_words), 0);
        pool.leaf_off.assign(items.size(), 0);
        pool.leaf_n.assign(items.size(), 0);
        int pos = 0;
        for (std::size_t li = 0; li < items.size(); ++li) {
            const WideLeaf& wl = items[li];
            pool.leaf_off[li] = pos;
            pool.leaf_n[li] = static_cast<int32_t>(wl.rows.size());
            for (std::size_t ri = 0; ri < wl.rows.size(); ++ri) {
                pool.cids[static_cast<std::size_t>(pos)] = static_cast<int32_t>(wl.rows[ri].cid);
                std::memcpy(
                    pool.bits.data() + static_cast<std::size_t>(pos) * static_cast<std::size_t>(n_words),
                    wl.bits.data() + ri * static_cast<std::size_t>(n_words),
                    static_cast<std::size_t>(n_words) * sizeof(uint64_t));
                pos += 1;
            }
        }
        const int pool_id = static_cast<int>(parts.wide_pools.size());
        parts.wide_pools.push_back(std::move(pool));
        for (std::size_t li = 0; li < items.size(); ++li) {
            HpcxMgnEntry ent;
            ent.kind = HpcxMgnEntry::WIDE;
            ent.pool_id = pool_id;
            ent.leaf_idx = static_cast<int>(li);
            ent.L = items[li].L;
            ent.n = static_cast<int>(items[li].rows.size());
            parts.tree_mgn[items[li].key] = ent;
        }
    }
    wide_by_words.clear();

    for (const auto& l_kv : matrix_by_l) {
        const int l_val = l_kv.first;
        const auto& items = l_kv.second;
        int total = 0;
        for (const auto& it : items) {
            total += static_cast<int>(it.second.size());
        }
        OwnedMatrixPool pool;
        pool.L = static_cast<int16_t>(l_val);
        pool.cids.assign(static_cast<std::size_t>(total), 0);
        pool.mat.assign(static_cast<std::size_t>(total) * static_cast<std::size_t>(l_val), 0);
        pool.spop.assign(static_cast<std::size_t>(total), 0);
        pool.leaf_off.assign(items.size(), 0);
        pool.leaf_n.assign(items.size(), 0);
        int pos = 0;
        for (std::size_t li = 0; li < items.size(); ++li) {
            pool.leaf_off[li] = pos;
            pool.leaf_n[li] = static_cast<int32_t>(items[li].second.size());
            for (const HpcFlatMgnRow& row : items[li].second) {
                pool.cids[static_cast<std::size_t>(pos)] = static_cast<int32_t>(row.cid);
                pool.spop[static_cast<std::size_t>(pos)] = static_cast<uint8_t>(row.hit);
                for (int j = 0; j < l_val; ++j) {
                    const int bit_pos = l_val - 1 - j;
                    const int bit = static_cast<int>((row.path_bits >> bit_pos) & 1ULL);
                    pool.mat[static_cast<std::size_t>(pos) * static_cast<std::size_t>(l_val) + static_cast<std::size_t>(j)] =
                        static_cast<uint8_t>(bit);
                }
                pos += 1;
            }
        }
        const int pool_id = static_cast<int>(parts.matrix_pools.size());
        parts.matrix_pools.push_back(std::move(pool));
        for (std::size_t li = 0; li < items.size(); ++li) {
            HpcxMgnEntry ent;
            ent.kind = HpcxMgnEntry::MATRIX;
            ent.pool_id = pool_id;
            ent.leaf_idx = static_cast<int>(li);
            ent.L = l_val;
            ent.n = static_cast<int>(items[li].second.size());
            parts.tree_mgn[items[li].first] = ent;
        }
    }
    matrix_by_l.clear();

    for (const auto& kv : compact_items) {
        HpcxMgnEntry ent;
        ent.kind = HpcxMgnEntry::COMPACT;
        ent.compact = kv.second;
        parts.tree_mgn[kv.first] = ent;
    }

    if (parts.tree_mgn.empty()) {
        if (err) {
            *err = "no MGN entries built from native tree";
        }
        return false;
    }
    return true;
}

std::vector<unsigned char> make_meta_json(int band, const HpcNativeBuildStats& ps, int n_views, int n_pools, int n_compact) {
    std::ostringstream os;
    os << "{"
       << "\"format\":\"HPCX\","
       << "\"version\":1,"
       << "\"band\":" << band << ","
       << "\"n_views\":" << n_views << ","
       << "\"n_pools\":" << n_pools << ","
       << "\"n_compact\":" << n_compact << ","
       << "\"partition_nodes\":" << ps.partition_nodes << ","
       << "\"partition_flat_bytes\":" << ps.partition_flat_bytes << ","
       << "\"route_entries\":" << ps.route_entries << ","
       << "\"index_pool_len\":" << ps.index_pool_len << ","
       << "\"leaf_nodes\":" << ps.leaf_nodes << ","
       << "\"build_from\":\"" << (env_enabled_local("HPC_BUILD_V7", false) ? "cpp_native_v7" : "cpp_native") << "\""
       << "}";
    const std::string text = os.str();
    return std::vector<unsigned char>(text.begin(), text.end());
}

}  // namespace

bool v8_preflat_partition_tree(
    HpcPartitionNode& root,
    HpcPartitionV8Context& ctx,
    HpcNativeBuildStats& stats,
    std::string* err) {
    return v8_preflat_partition_tree_impl(root, ctx, stats, err);
}

bool hpcx_write_from_native_tree(
    HpcPartitionNode& root,
    std::vector<HpcFlatMgnLeaf>& mgn_leaves,
    int band,
    const std::string& out_hpcx_path,
    HpcxWriteStats* stats,
    HpcNativeBuildStats* native_stats,
    std::string* err,
    HpcPartitionV8Context* v8_ctx) {
    const auto t0 = std::chrono::steady_clock::now();
    if (v8_ctx != nullptr) {
        hpc_v8_clear_merged_one_index_cache();
    }

    HpcNativeBuildStats local_stats;
    HpcxWriteParts parts;
    parts.band = static_cast<uint32_t>(band);

    if (!partition_tree_to_flat(root, parts.partition, local_stats, err, v8_ctx)) {
        return false;
    }
    if (!attach_mgn_to_parts(mgn_leaves, parts, err, v8_ctx)) {
        return false;
    }

    int n_compact = 0;
    for (const auto& kv : parts.tree_mgn) {
        if (kv.second.kind == HpcxMgnEntry::COMPACT) {
            n_compact += 1;
        }
    }
    parts.meta = make_meta_json(
        band,
        local_stats,
        static_cast<int>(parts.tree_mgn.size()),
        static_cast<int>(parts.packed_pools.size() + parts.matrix_pools.size()),
        n_compact);

    const auto t_compute_done = std::chrono::steady_clock::now();
    HpcxWriteStats wstats;
    if (!hpcx_write_parts(parts, out_hpcx_path, &wstats, err)) {
        return false;
    }

    if (stats != nullptr) {
        *stats = wstats;
        stats->compute_s = std::chrono::duration<double>(t_compute_done - t0).count();
        stats->write_s = wstats.write_s;
    }
    if (native_stats != nullptr) {
        *native_stats = local_stats;
    }
    return true;
}
