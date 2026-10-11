#include "hpc_partition_bfs_v8.h"
#include "hpc_partition_children.h"
#include "hpc_v8_stream_checkpoint.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace {

std::string one_index_key(const std::vector<int32_t>& data) {
    std::string key;
    key.resize(data.size() * sizeof(int32_t));
    if (!data.empty()) {
        std::memcpy(key.data(), data.data(), data.size() * sizeof(int32_t));
    }
    return key;
}

const std::vector<int32_t>& span_view(
    const std::vector<int32_t>& pool,
    const std::pair<uint32_t, uint32_t>& span) {
    static thread_local std::vector<int32_t> tls_view;
    const auto [off, len] = span;
    tls_view.assign(
        pool.begin() + static_cast<std::ptrdiff_t>(off),
        pool.begin() + static_cast<std::ptrdiff_t>(off + len));
    return tls_view;
}

thread_local std::vector<int32_t> g_leaf_ckpt_tls;

const std::vector<int32_t>& leaf_one_index_view(
    const HpcPartitionNode& leaf,
    const HpcPartitionV8Context* v8_ctx) {
    if (v8_ctx == nullptr) {
        return leaf.one_index;
    }
    if (!leaf.one_index.empty()) {
        return leaf.one_index;
    }
    if (v8_ctx->stream_ckpt_active &&
        hpc_v8_stream_ckpt_leaf_one_index_view(*v8_ctx, leaf, g_leaf_ckpt_tls)) {
        return g_leaf_ckpt_tls;
    }
    return v8_ctx->node_one_index_view(leaf);
}

void gather_one_index_values(
    const HpcPartitionNode& node,
    const HpcPartitionV8Context* v8_ctx,
    std::vector<int32_t>& out) {
    if (v8_ctx != nullptr && v8_ctx->stream_ckpt_active) {
        std::vector<int32_t> ckpt_view;
        if (hpc_v8_stream_ckpt_leaf_one_index_view(*v8_ctx, node, ckpt_view)) {
            out.insert(out.end(), ckpt_view.begin(), ckpt_view.end());
            return;
        }
    }
    if (v8_ctx != nullptr) {
        const auto sit = v8_ctx->node_index_span.find(&node);
        if (sit != v8_ctx->node_index_span.end()) {
            const auto [off, len] = sit->second;
            out.insert(
                out.end(),
                v8_ctx->index_pool.begin() + static_cast<std::ptrdiff_t>(off),
                v8_ctx->index_pool.begin() + static_cast<std::ptrdiff_t>(off + len));
            return;
        }
    }
    if (!node.one_index.empty()) {
        out.insert(out.end(), node.one_index.begin(), node.one_index.end());
        return;
    }
    for (const auto& kv : node.next_partition) {
        if (kv.second) {
            gather_one_index_values(*kv.second, v8_ctx, out);
        }
    }
    for (const auto& kv : node.next_partition_pk) {
        if (kv.second) {
            gather_one_index_values(*kv.second, v8_ctx, out);
        }
    }
}

thread_local std::unordered_map<const HpcPartitionNode*, std::vector<int32_t>> g_merged_one_index;

}  // namespace

std::pair<uint32_t, uint32_t> HpcPartitionV8Context::append_index_pool(std::vector<int32_t> data) {
    std::sort(data.begin(), data.end());
    data.erase(std::unique(data.begin(), data.end()), data.end());
    const std::string key = one_index_key(data);
    const auto hit = index_dedup.find(key);
    if (hit != index_dedup.end()) {
        return hit->second;
    }
    const uint32_t off = static_cast<uint32_t>(index_pool.size());
    const uint32_t len = static_cast<uint32_t>(data.size());
    index_pool.insert(index_pool.end(), data.begin(), data.end());
    const std::pair<uint32_t, uint32_t> span{off, len};
    index_dedup.emplace(key, span);
    return span;
}

void HpcPartitionV8Context::bind_node_one_index(HpcPartitionNode& node, std::vector<int32_t> data) {
    if (data.empty()) {
        return;
    }
    node_index_span[&node] = append_index_pool(std::move(data));
    node.one_index.clear();
    node.one_index.shrink_to_fit();
    leaf_one_index_compacted = true;
}

void HpcPartitionV8Context::bind_node_oip_one_index(HpcPartitionNode& node, std::vector<int32_t> data) {
    if (data.empty()) {
        return;
    }
    node_oip_span[&node] = append_index_pool(std::move(data));
    node.oip_one_index.clear();
    node.oip_one_index.shrink_to_fit();
}

const std::vector<int32_t>& HpcPartitionV8Context::node_one_index_view(const HpcPartitionNode& node) const {
    static thread_local std::vector<int32_t> tls_empty;
    const auto it = node_index_span.find(&node);
    if (it != node_index_span.end()) {
        return span_view(index_pool, it->second);
    }
    if (!node.one_index.empty()) {
        return node.one_index;
    }
    tls_empty.clear();
    return tls_empty;
}

const std::vector<int32_t>& HpcPartitionV8Context::node_oip_one_index_view(const HpcPartitionNode& node) const {
    static thread_local std::vector<int32_t> tls_empty;
    const auto it = node_oip_span.find(&node);
    if (it != node_oip_span.end()) {
        return span_view(index_pool, it->second);
    }
    if (!node.oip_one_index.empty()) {
        return node.oip_one_index;
    }
    tls_empty.clear();
    return tls_empty;
}

void HpcPartitionV8Context::clear_index_pool() {
    index_pool.clear();
    index_dedup.clear();
    node_index_span.clear();
    node_oip_span.clear();
}

std::shared_ptr<const std::vector<int32_t>> HpcOneIndexInternPool::intern(std::vector<int32_t> data) {
    const std::string key = one_index_key(data);
    const auto it = entries.find(key);
    if (it != entries.end()) {
        return it->second;
    }
    auto sp = std::make_shared<const std::vector<int32_t>>(std::move(data));
    entries.emplace(one_index_key(*sp), sp);
    return sp;
}

void HpcPartitionV8Context::intern_node_one_index(HpcPartitionNode& node) {
    if (node.one_index.empty()) {
        return;
    }
    auto sp = one_index_intern.intern(std::move(node.one_index));
    one_index_ref[&node] = sp;
}

void HpcPartitionV8Context::intern_all_one_index(HpcPartitionNode& root) {
    (void)root;
}

const std::vector<int32_t>& HpcPartitionV8Context::one_index_of(const HpcPartitionNode& node) const {
    const auto it = one_index_ref.find(&node);
    if (it != one_index_ref.end()) {
        return *it->second;
    }
    return node.one_index;
}

const std::vector<int32_t>& hpc_v8_node_one_index(
    const HpcPartitionNode& node,
    const HpcPartitionV8Context* v8_ctx) {
    if (v8_ctx != nullptr && (node.is_mgn_leaf || node.mgn_key >= 0)) {
        return v8_ctx->leaf_one_index_of(node);
    }
    if (v8_ctx != nullptr) {
        const auto& pooled = v8_ctx->node_one_index_view(node);
        if (!pooled.empty()) {
            return pooled;
        }
    }
    if (!node.one_index.empty()) {
        return node.one_index;
    }
    if (v8_ctx != nullptr && hpc_node_has_children(node)) {
        auto& cached = g_merged_one_index[&node];
        if (cached.empty()) {
            gather_one_index_values(node, v8_ctx, cached);
            std::sort(cached.begin(), cached.end());
            cached.erase(std::unique(cached.begin(), cached.end()), cached.end());
        }
        return cached;
    }
    return node.one_index;
}

void hpc_v8_clear_merged_one_index_cache() {
    g_merged_one_index.clear();
}

void HpcPartitionV8Context::stash_leaf_one_index(HpcPartitionNode& leaf) {
    if (leaf.one_index.empty()) {
        return;
    }
    if (stream_ckpt_active) {
        hpc_v8_stream_ckpt_spill_leaf_one_index(*this, leaf);
        leaf_one_index_compacted = true;
        return;
    }
    bind_node_one_index(leaf, std::move(leaf.one_index));
    leaf_one_index_compacted = true;
}

const std::vector<int32_t>& HpcPartitionV8Context::leaf_one_index_of(const HpcPartitionNode& leaf) const {
    return leaf_one_index_view(leaf, this);
}

void HpcPartitionV8Context::compact_leaf_one_index() {
    if (leaf_one_index_compacted) {
        return;
    }
    for (auto& kv : leaf_one_index) {
        if (kv.first != nullptr && !kv.second.empty()) {
            bind_node_one_index(*const_cast<HpcPartitionNode*>(kv.first), std::move(kv.second));
        }
    }
    leaf_one_index.clear();
    leaf_one_index_compacted = true;
}

void HpcPartitionV8Context::release_leaf_one_index(const HpcPartitionNode* leaf) {
    if (leaf == nullptr) {
        return;
    }
    stream_leaf_one_ckpt_span.erase(leaf);
    node_index_span.erase(leaf);
    leaf_one_index_span.erase(leaf);
    if (!leaf_one_index_compacted) {
        leaf_one_index.erase(leaf);
    }
}

void HpcPartitionV8Context::clear_leaf_one_index() {
    leaf_one_index.clear();
    leaf_one_index_pool.clear();
    leaf_one_index_span.clear();
    leaf_one_index_compacted = false;
    clear_index_pool();
}

void HpcPartitionV8Context::clear_mgn_leaf_col_ids(const std::vector<HpcPartitionNode*>& mgn_leaves) {
    for (HpcPartitionNode* leaf : mgn_leaves) {
        if (leaf == nullptr) {
            continue;
        }
        leaf->ids.clear();
        leaf->ids.shrink_to_fit();
    }
}

void hpc_v8_drop_merged_one_index_cache(const HpcPartitionNode* node) {
    if (node == nullptr) {
        return;
    }
    g_merged_one_index.erase(node);
}

void HpcPartitionV8Context::release_cache_hits() {
    cache.hits_mmap.reset();
    cache.hit_pool.clear();
    cache.hit_pool.clear();
    cache.hit_off.clear();
    cache.nth_hits.clear();
    cache.col_ids.clear();
    cache.hit_pool.shrink_to_fit();
    cache.hit_off.shrink_to_fit();
    cache.nth_hits.shrink_to_fit();
    cache.col_ids.shrink_to_fit();
}

void HpcPartitionV8Context::release_partition_metadata() {
    col_perm.clear();
    col_perm.shrink_to_fit();
    ranges.clear();
    one_index_ref.clear();
    one_index_intern.entries.clear();
    leaf_one_index.clear();
    // index_pool + spans kept until MGN/preflat complete
}

namespace {

void clear_oip_segments_recursive(HpcPartitionNode& node) {
    node.oip_segments.clear();
    node.oip_segments.shrink_to_fit();
    hpc_node_for_each_child(node, [&](HpcPartitionNode& child) {
        clear_oip_segments_recursive(child);
    });
}

}  // namespace

void HpcPartitionV8Context::clear_all_oip_segments(HpcPartitionNode& root) {
    clear_oip_segments_recursive(root);
}

void HpcPartitionV8Context::clear_preflat() {
    preflat_part = HpcFlatPart{};
    preflat_partition_nodes = 0;
    preflat_route_entries = 0;
    preflat_leaf_nodes = 0;
    preflat_index_pool_len = 0;
    partition_preflat_done = false;
}

void HpcPartitionV8Context::clear_mgn_stream() {
    mgn_stream_rows.clear();
    mgn_stream_rows.shrink_to_fit();
    mgn_stream_idx.clear();
    mgn_stream_idx.shrink_to_fit();
}
