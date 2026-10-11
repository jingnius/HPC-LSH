#pragma once

#include "hpc_mgn_build.h"
#include "hpc_partition_build.h"
#include "hpcx_write.h"

struct HpcPartitionV8Context;

#include <string>
#include <vector>

struct HpcNativeBuildStats {
    int partition_nodes = 0;
    int partition_flat_bytes = 0;
    int route_entries = 0;
    int leaf_nodes = 0;
    int index_pool_len = 0;
};

// Assemble HPCX directly from C++ partition tree + flat MGN leaves.
bool hpcx_write_from_native_tree(
    HpcPartitionNode& root,
    std::vector<HpcFlatMgnLeaf>& mgn_leaves,
    int band,
    const std::string& out_hpcx_path,
    HpcxWriteStats* stats,
    HpcNativeBuildStats* native_stats,
    std::string* err,
    HpcPartitionV8Context* v8_ctx = nullptr);
