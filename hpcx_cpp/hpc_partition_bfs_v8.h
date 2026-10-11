#pragma once

#include "hpc_col_build_cache.h"
#include "hpc_mgn_build.h"
#include "hpc_partition_build.h"
#include "hpc_partition_flat.h"
#include "hpc_v8_stream_checkpoint.h"

struct HpcNativeBuildStats;

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct HpcOneIndexInternPool {
    std::unordered_map<std::string, std::shared_ptr<const std::vector<int32_t>>> entries;

    std::shared_ptr<const std::vector<int32_t>> intern(std::vector<int32_t> data);
};

struct HpcPartitionV8Context {
    std::vector<uint32_t> col_perm;
    HpcColBuildCache cache;
    std::unordered_map<const HpcPartitionNode*, std::pair<uint32_t, uint32_t>> ranges;
    HpcOneIndexInternPool one_index_intern;
    HpcFlatPart preflat_part;
    int preflat_partition_nodes = 0;
    int preflat_route_entries = 0;
    int preflat_leaf_nodes = 0;
    int preflat_index_pool_len = 0;
    bool partition_preflat_done = false;
    std::vector<HpcFlatMgnRow> mgn_stream_rows;
    std::vector<std::vector<uint32_t>> mgn_stream_idx;
    std::unordered_map<const HpcPartitionNode*, std::shared_ptr<const std::vector<int32_t>>> one_index_ref;
    std::vector<int32_t> index_pool;
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> index_dedup;
    std::unordered_map<const HpcPartitionNode*, std::pair<uint32_t, uint32_t>> node_index_span;
    std::unordered_map<const HpcPartitionNode*, std::pair<uint32_t, uint32_t>> node_oip_span;
    std::unordered_map<const HpcPartitionNode*, std::vector<int32_t>> leaf_one_index;
    bool leaf_one_index_compacted = false;
    std::vector<int32_t> leaf_one_index_pool;
    std::unordered_map<const HpcPartitionNode*, std::pair<uint32_t, uint32_t>> leaf_one_index_span;

    std::pair<uint32_t, uint32_t> append_index_pool(std::vector<int32_t> data);
    void bind_node_one_index(HpcPartitionNode& node, std::vector<int32_t> data);
    void bind_node_oip_one_index(HpcPartitionNode& node, std::vector<int32_t> data);
    const std::vector<int32_t>& node_one_index_view(const HpcPartitionNode& node) const;
    const std::vector<int32_t>& node_oip_one_index_view(const HpcPartitionNode& node) const;
    void clear_index_pool();

    void stash_leaf_one_index(HpcPartitionNode& leaf);
    const std::vector<int32_t>& leaf_one_index_of(const HpcPartitionNode& leaf) const;
    void compact_leaf_one_index();
    void release_leaf_one_index(const HpcPartitionNode* leaf);
    void clear_leaf_one_index();
    static void clear_mgn_leaf_col_ids(const std::vector<HpcPartitionNode*>& mgn_leaves);

    void intern_node_one_index(HpcPartitionNode& node);
    void intern_all_one_index(HpcPartitionNode& root);
    const std::vector<int32_t>& one_index_of(const HpcPartitionNode& node) const;
    void release_cache_hits();
    void release_partition_metadata();

    static void clear_all_oip_segments(HpcPartitionNode& root);
    void clear_preflat();
    void clear_mgn_stream();

    bool stream_ckpt_active = false;
    std::string stream_ckpt_dir;
    std::unique_ptr<HpcV8CkptFileHandle> stream_oip_ckpt;
    std::unique_ptr<HpcV8CkptFileHandle> stream_leaf_ckpt;
    std::unordered_map<const HpcPartitionNode*, std::pair<uint64_t, uint32_t>> stream_oip_ckpt_span;
    std::unordered_map<const HpcPartitionNode*, std::pair<uint64_t, uint32_t>> stream_leaf_one_ckpt_span;
};

const std::vector<int32_t>& hpc_v8_node_one_index(
    const HpcPartitionNode& node,
    const HpcPartitionV8Context* v8_ctx);

void hpc_v8_clear_merged_one_index_cache();
void hpc_v8_drop_merged_one_index_cache(const HpcPartitionNode* node);

bool v8_preflat_partition_tree(
    HpcPartitionNode& root,
    HpcPartitionV8Context& ctx,
    HpcNativeBuildStats& stats,
    std::string* err = nullptr);

// v6 remains the default backup path (unset HPC_BUILD_V8). v8 mmap-decodes SIG hits
// into a temp file and keeps hit_pool out of heap RSS during partition.
HpcPartitionBuildResult hpc_run_partition_bfs_v8(
    const std::string& sig_native_path,
    const HpcPartitionBuildConfig& cfg,
    HpcPartitionV8Context* v8_ctx,
    std::string* err);

bool hpc_build_mgn_from_sigb_v8(
    const std::string& sig_native_path,
    int num_perm,
    const std::vector<HpcPartitionNode*>& mgn_leaves,
    HpcPartitionV8Context& v8_ctx,
    std::vector<HpcFlatMgnLeaf>* out,
    std::string* err);
