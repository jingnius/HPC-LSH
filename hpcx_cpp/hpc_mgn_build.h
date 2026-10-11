#pragma once

#include "hpc_mgn_compact.h"
#include "hpc_partition_build.h"

#include <cstdint>
#include <string>
#include <vector>

struct HpcFlatMgnRow {
    int64_t cid = 0;
    uint64_t path_bits = 0;
    int hit = 0;
};

struct HpcFlatMgnLeaf {
    int mgn_key = 0;
    int L = 0;
    int n_cols = 0;
    int stop_depth = -1;
    int stop_depth_proxy = 0;
    std::vector<HpcFlatMgnRow> rows;
    // L>64: same zip bits as path_bits, packed across wide_words uint64 words (bit j in word j/64).
    int wide_words = 0;
    std::vector<uint64_t> wide_bits;
    // Global OHS: one OHS tree for every target, instead of a packed/matrix row scan.
    bool store_compact = false;
    MgnCompactTree compact;
};

// Stream SIGB columns into per-leaf flat MGN rows (zip_0_path + hit).
bool hpc_build_mgn_from_sigb(
    const std::string& sig_native_path,
    int num_perm,
    const std::vector<HpcPartitionNode*>& mgn_leaves,
    std::vector<HpcFlatMgnLeaf>* out,
    std::string* err);
