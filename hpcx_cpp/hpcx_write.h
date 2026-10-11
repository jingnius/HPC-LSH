#pragma once

#include "hpc_hpcx.h"

#include <cstddef>
#include <cstdint>
#include <string>

struct HpcxWriteParts {
    uint32_t band = 0;
    std::vector<unsigned char> meta;
    std::vector<unsigned char> partition;
    std::vector<OwnedPackedPool> packed_pools;
    std::vector<OwnedWidePool> wide_pools;
    std::vector<OwnedMatrixPool> matrix_pools;
    std::unordered_map<int, HpcxMgnEntry> tree_mgn;
};

struct HpcxWriteStats {
    std::size_t file_bytes = 0;
    int n_views = 0;
    int n_pools = 0;
    int n_compact = 0;
    double write_s = 0.0;
    double compute_s = 0.0;
};

// Write HPCX from assembled sections (Phase D+ build path).
bool hpcx_write_parts(
    const HpcxWriteParts& parts,
    const std::string& out_path,
    HpcxWriteStats* stats = nullptr,
    std::string* err = nullptr);

// Serialize a loaded index back to HPCX (Phase D writer).
// Partition/meta sections are copied from the source mmap; pools/views/compact are rebuilt.
bool hpcx_write_from_index(
    const HpcxIndex& idx,
    const std::string& out_path,
    HpcxWriteStats* stats = nullptr,
    std::string* err = nullptr);
