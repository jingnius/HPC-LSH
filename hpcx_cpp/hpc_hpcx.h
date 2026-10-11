#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hpc_flat_search.h"
#include "hpc_mgn_compact.h"
#include "hpc_partition_view.h"

struct OwnedPackedPool {
    std::vector<int32_t> cids;
    std::vector<uint64_t> bits;
    std::vector<uint8_t> spop;
    std::vector<int64_t> leaf_off;
    std::vector<int32_t> leaf_n;
    FlatPackedPool view() const;
};

struct OwnedWidePool {
    std::vector<int32_t> cids;
    std::vector<uint64_t> bits;
    std::vector<int64_t> leaf_off;
    std::vector<int32_t> leaf_n;
    int32_t n_words = 0;
    FlatWidePool view() const;
};

struct OwnedMatrixPool {
    std::vector<int32_t> cids;
    std::vector<uint8_t> mat;
    std::vector<uint8_t> spop;
    std::vector<int64_t> leaf_off;
    std::vector<int32_t> leaf_n;
    int16_t L = 0;
    FlatMatrixPool view() const;
};

struct HpcxMgnEntry {
    enum Kind { PACKED, MATRIX, COMPACT, WIDE } kind = PACKED;
    int pool_id = -1;
    int leaf_idx = 0;
    int L = 0;
    int n = 0;
    MgnCompactTree compact;
    CompactSearchIndex compact_index;
};

struct HpcxIndex {
    std::string path;
    int fd = -1;
    unsigned char* mmap_base = nullptr;
    std::size_t mmap_len = 0;
    uint32_t band = 0;
    HpcFlatPartView partition{};
    std::vector<OwnedPackedPool> packed_pools;
    std::vector<OwnedWidePool> wide_pools;
    std::vector<OwnedMatrixPool> matrix_pools;
    std::unordered_map<int, HpcxMgnEntry> tree_mgn;
};

std::unique_ptr<HpcxIndex> hpcx_load(const std::string& path, std::string* err = nullptr);

// After every band is loaded, fault the whole mapped file and the heap copies
// search actually walks, so later queries do not pay for those page faults.
void hpcx_prefault_resident(const HpcxIndex& idx);

bool hpcx_get_section(
    const HpcxIndex& idx,
    int sec,
    const unsigned char** data,
    std::size_t* len);

void hpcx_search_encoded(
    const HpcxIndex& idx,
    const int* encoded_query,
    int query_len,
    int min_hits,
    std::vector<int32_t>& out_cids);
