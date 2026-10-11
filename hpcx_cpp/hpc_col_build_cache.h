#pragma once

#include "hpc_index_one_store.h"
#include "hpc_sig_hits_mmap.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

// Build-time column views for faster split evaluation (HPC_BUILD_V8).
struct HpcColBuildCache {
    int max_nth = 10;
    std::vector<int64_t> col_ids;
    std::vector<uint32_t> hit_off;
    std::vector<int32_t> hit_pool;
    // nth_hits[col_idx * max_nth + (nth - 1)] mirrors IndexOneStore::nth_hit.
    std::vector<int32_t> nth_hits;
    HpcSigHitsMmap hits_mmap;

    static HpcColBuildCache from_index_one(const HpcIndexOneStore& index_one, int max_nth = 10);
    static HpcColBuildCache from_index_one_move(HpcIndexOneStore& index_one, int max_nth = 10);
    static HpcColBuildCache from_sig_hits_mmap(HpcSigHitsMmap hits_mmap, int max_nth = 10);

    std::size_t size() const { return col_ids.size(); }

    int64_t col_id(uint32_t col_idx) const { return col_ids[col_idx]; }

    int32_t nth_hit_idx(uint32_t col_idx, int nth) const;

    void hits_span(uint32_t col_idx, const int32_t** hits, int* len) const;

    void active_hits_into(
        uint32_t col_idx,
        const std::unordered_set<int32_t>& part_index_set,
        std::vector<int32_t>& out) const;

    std::vector<int32_t> active_hits_sorted(
        uint32_t col_idx,
        const int32_t* part_index,
        int part_len) const;

    std::vector<int32_t> boundaries_nth(
        uint32_t begin,
        uint32_t end,
        const std::vector<uint32_t>& perm,
        int nth,
        int k) const;
};
