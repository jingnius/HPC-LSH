#pragma once

#include "hpc_ct_ops.h"  // HpcPartCSKey
#include "hpc_index_one_store.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct HpcPartitionBuildConfig {
    int num_hpc_layers = 6;
    int min_cols = 10;
    double max_child_ratio = 0.7;
    int initial_split_dims = 3;
    int max_split_dims = 10;
    int adaptive_k_min = 3;
    int adaptive_k_max = 5;
    int root_num_segments = 5;
    bool adaptive_num_segments = true;
    int depth_stop = 50;
    int depth_extra_l = 5;
    int large_part_cols = 100;
    int large_k_min = 4;
    int large_k_max = 7;
    int large_max_split_dims = 12;
    int max_mgn_leaf_cols = 5000;
    int mgn_nary_min_cols = 2048;
    int mgn_flat_max_cols = 512;
    int mgn_nary_max_L = 64;
};

struct HpcPartitionNode {
    int node_id = -1;
    std::vector<int64_t> ids;
    std::vector<int32_t> one_index;
    std::vector<int32_t> segment_boundaries;
    HpcOneIndexPartition one_index_partition;
    std::vector<std::vector<int32_t>> oip_segments;
    // v8: part_index snapshot at split; used to rebuild OIP after oip_segments release.
    std::vector<int32_t> oip_one_index;
    std::unordered_map<std::string, std::unique_ptr<HpcPartitionNode>> next_partition;
    std::unordered_map<HpcPartCSKey, std::unique_ptr<HpcPartitionNode>> next_partition_pk;
    std::string split_dim;
    int split_k = 0;
    bool split_forced = false;
    int min_1index = 0;
    int max_1index = 0;
    bool is_mgn_leaf = false;
    int mgn_key = -1;
    int stop_depth = -1;
    int stop_depth_proxy = 0;
    int split_retry_count = 0;
    // query routing: segment-presence mask -> child node_id(s)
    std::map<int, std::vector<int>> route;
};

struct HpcPartitionBuildResult {
    std::unique_ptr<HpcPartitionNode> root;
    std::vector<HpcPartitionNode*> mgn_leaves;
    int hpc_nodes = 0;
};

int hpc_partition_segment_count(const HpcPartitionNode& part);

HpcPartitionBuildResult hpc_run_partition_bfs(
    const HpcIndexOneStore& index_one,
    const HpcPartitionBuildConfig& cfg);

bool hpc_partition_write_json(
    const HpcPartitionNode& root,
    const std::vector<HpcPartitionNode*>& mgn_leaves,
    const std::string& path,
    std::string* err);
