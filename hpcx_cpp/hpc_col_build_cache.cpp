#include "hpc_col_build_cache.h"

#include "hpc_ct_ops.h"

#include <algorithm>
#include <map>
#include <sys/mman.h>
#include <unordered_set>

namespace {

int32_t nth_hit_from_span(const int32_t* hits, int hit_len, int nth) {
    if (nth <= 0 || hits == nullptr || hit_len <= 0) {
        return 0;
    }
    if (hit_len >= nth) {
        return hits[nth - 1];
    }
    return hits[hit_len - 1];
}

}  // namespace

HpcColBuildCache HpcColBuildCache::from_index_one(const HpcIndexOneStore& index_one, int max_nth) {
    HpcColBuildCache out;
    out.max_nth = std::max(1, max_nth);
    out.col_ids = index_one.col_ids;
    out.hit_off = index_one.hit_off;
    out.hit_pool = index_one.hit_pool;
    const std::size_t n = out.col_ids.size();
    out.nth_hits.assign(n * static_cast<std::size_t>(out.max_nth), 0);
    for (std::size_t col_idx = 0; col_idx < n; ++col_idx) {
        const int64_t cid = out.col_ids[col_idx];
        for (int nth = 1; nth <= out.max_nth; ++nth) {
            out.nth_hits[col_idx * static_cast<std::size_t>(out.max_nth) + static_cast<std::size_t>(nth - 1)] =
                index_one.nth_hit(cid, nth);
        }
    }
    return out;
}

HpcColBuildCache HpcColBuildCache::from_sig_hits_mmap(HpcSigHitsMmap hits_mmap, int max_nth) {
    HpcColBuildCache out;
    out.max_nth = std::max(1, max_nth);
    out.hits_mmap = std::move(hits_mmap);
    out.col_ids = out.hits_mmap.col_ids;
    out.hit_off = out.hits_mmap.hit_off;
    // v8 mmap path: nth_hits left empty; nth_hit_idx reads from mmap on demand.
    return out;
}

HpcColBuildCache HpcColBuildCache::from_index_one_move(HpcIndexOneStore& index_one, int max_nth) {
    HpcColBuildCache out;
    out.max_nth = std::max(1, max_nth);
    const std::size_t n = index_one.col_ids.size();
    out.nth_hits.assign(n * static_cast<std::size_t>(out.max_nth), 0);
    for (std::size_t col_idx = 0; col_idx < n; ++col_idx) {
        const int64_t cid = index_one.col_ids[col_idx];
        for (int nth = 1; nth <= out.max_nth; ++nth) {
            out.nth_hits[col_idx * static_cast<std::size_t>(out.max_nth) + static_cast<std::size_t>(nth - 1)] =
                index_one.nth_hit(cid, nth);
        }
    }
    out.col_ids = std::move(index_one.col_ids);
    out.hit_off = std::move(index_one.hit_off);
    out.hit_pool = std::move(index_one.hit_pool);
    index_one.clear();
    return out;
}

int32_t HpcColBuildCache::nth_hit_idx(uint32_t col_idx, int nth) const {
    if (nth <= 0 || nth > max_nth || col_idx >= col_ids.size()) {
        return 0;
    }
    if (!nth_hits.empty()) {
        return nth_hits[col_idx * static_cast<std::size_t>(max_nth) + static_cast<std::size_t>(nth - 1)];
    }
    const int32_t* hits = nullptr;
    int hit_len = 0;
    hits_span(col_idx, &hits, &hit_len);
    return nth_hit_from_span(hits, hit_len, nth);
}

void HpcColBuildCache::hits_span(uint32_t col_idx, const int32_t** hits, int* len) const {
    if (hits == nullptr || len == nullptr) {
        return;
    }
    if (col_idx >= col_ids.size()) {
        *hits = nullptr;
        *len = 0;
        return;
    }
    if (hits_mmap.base != nullptr && hits_mmap.base != MAP_FAILED) {
        const int32_t* pool = hits_mmap.hit_pool();
        if (pool == nullptr) {
            *hits = nullptr;
            *len = 0;
            return;
        }
        *hits = pool + hit_off[col_idx];
        *len = static_cast<int>(hit_off[col_idx + 1] - hit_off[col_idx]);
        return;
    }
    const uint32_t off = hit_off[col_idx];
    const uint32_t end = hit_off[col_idx + 1];
    *hits = hit_pool.data() + off;
    *len = static_cast<int>(end - off);
}

void HpcColBuildCache::active_hits_into(
    uint32_t col_idx,
    const std::unordered_set<int32_t>& part_index_set,
    std::vector<int32_t>& out) const {
    out.clear();
    if (col_idx >= col_ids.size()) {
        return;
    }
    const int32_t* hits = nullptr;
    int hit_len = 0;
    hits_span(col_idx, &hits, &hit_len);
    if (hits == nullptr || hit_len <= 0) {
        return;
    }
    out.reserve(static_cast<std::size_t>(hit_len));
    for (int i = 0; i < hit_len; ++i) {
        if (part_index_set.find(hits[i]) != part_index_set.end()) {
            out.push_back(hits[i]);
        }
    }
}

std::vector<int32_t> HpcColBuildCache::active_hits_sorted(
    uint32_t col_idx,
    const int32_t* part_index,
    int part_len) const {
    std::unordered_set<int32_t> pi_set(part_index, part_index + part_len);
    std::vector<int32_t> out;
    active_hits_into(col_idx, pi_set, out);
    return out;
}

std::vector<int32_t> HpcColBuildCache::boundaries_nth(
    uint32_t begin,
    uint32_t end,
    const std::vector<uint32_t>& perm,
    int nth,
    int k) const {
    if (begin >= end || k <= 0 || nth <= 0) {
        return {};
    }
    std::map<int32_t, int> hist;
    for (uint32_t pi = begin; pi < end; ++pi) {
        hist[nth_hit_idx(perm[pi], nth)] += 1;
    }
    const double threshold = static_cast<double>(end - begin) / static_cast<double>(k);
    std::vector<int32_t> sorted_pos;
    sorted_pos.reserve(hist.size());
    for (const auto& kv : hist) {
        sorted_pos.push_back(kv.first);
    }
    std::vector<int32_t> boundaries;
    int cum = 0;
    int segments_done = 0;
    for (const int32_t pos : sorted_pos) {
        cum += hist[pos];
        while (segments_done < k - 1 && cum >= static_cast<int>(threshold)) {
            boundaries.push_back(pos);
            cum = 0;
            segments_done += 1;
        }
    }
    if (!sorted_pos.empty()) {
        while (static_cast<int>(boundaries.size()) < k - 1) {
            boundaries.push_back(sorted_pos.back());
        }
    }
    const int keep = std::max(k - 1, 0);
    if (static_cast<int>(boundaries.size()) > keep) {
        boundaries.resize(static_cast<std::size_t>(keep));
    }
    return boundaries;
}
