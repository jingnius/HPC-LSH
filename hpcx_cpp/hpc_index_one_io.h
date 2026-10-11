#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#define HPC_INDEX_ONE_MAGIC 0x314F5849u /* 'IXO1' little-endian */

struct HpcIndexOneEntry {
    uint64_t col_id = 0;
    std::vector<int32_t> hits;
};

// Stream-write index_one built from SIGB (C++ side).
bool hpc_index_one_write(
    const std::string& path,
    const std::unordered_map<uint64_t, std::vector<int32_t>>& index_one,
    std::string* err);

bool hpc_index_one_read(
    const std::string& path,
    std::vector<HpcIndexOneEntry>* out,
    uint32_t* n_cols,
    std::string* err);
