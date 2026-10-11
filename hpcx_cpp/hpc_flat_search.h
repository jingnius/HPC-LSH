#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "hpc_mgn_compact.h"

inline int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    int c = 0;
    while (x) {
        x &= x - 1;
        ++c;
    }
    return c;
#endif
}

uint64_t zip_0_path_to_int(const int* sig, int sig_len, const int* one_index, int one_len);

// Count matched positions on one_index (matches Python zip path bit_count for min_hits filter).
int zip_0_path_popcount(const int* sig, int sig_len, const int* one_index, int one_len);

struct ZipPathQ {
    uint64_t q = 0;
    int popcount = 0;
};

ZipPathQ zip_0_path_q(const int* sig, int sig_len, const int* one_index, int one_len);

struct FlatPackedPool {
    const int32_t* cids = nullptr;
    const uint64_t* bits = nullptr;
    const uint8_t* spop = nullptr;
    const int64_t* leaf_off = nullptr;
    const int32_t* leaf_n = nullptr;
    int64_t n_rows = 0;
    int32_t n_leaves = 0;
};

struct CompactSearchIndex {
    std::unordered_map<int, int> key_to_idx;
    std::unordered_map<int, std::vector<int32_t>> leaves_by_key;
};

CompactSearchIndex compact_build_index(const MgnCompactTree& tree);

struct FlatMatrixPool {
    const int32_t* cids = nullptr;
    const uint8_t* mat = nullptr;
    const uint8_t* spop = nullptr;
    const int64_t* leaf_off = nullptr;
    const int32_t* leaf_n = nullptr;
    int64_t n_rows = 0;
    int32_t n_leaves = 0;
    int16_t L = 0;
};

struct FlatWidePool {
    const int32_t* cids = nullptr;
    const uint64_t* bits = nullptr;
    const int64_t* leaf_off = nullptr;
    const int32_t* leaf_n = nullptr;
    int64_t n_rows = 0;
    int32_t n_leaves = 0;
    int32_t n_words = 0;
};

// Pack zip_0_path bits for L>64. Bit j of one_index lives in word j/64, bit j%64.
void zip_0_path_words(
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    std::vector<uint64_t>& words,
    int& popcount);

void flat_wide_find_leaves_q(
    const FlatWidePool& pool,
    int leaf_idx,
    const uint64_t* q_words,
    int n_words,
    int qpc,
    int min_hits,
    std::vector<int32_t>& out_cids);

void flat_packed_find_leaves_q(
    const FlatPackedPool& pool,
    int leaf_idx,
    uint64_t q,
    int qpc,
    int min_hits,
    std::vector<int32_t>& out_cids);

void matrix_qbits_from_sig(
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    int L,
    std::vector<uint8_t>& qbits_out,
    int& qpc_out);

void flat_matrix_find_leaves_q_cached(
    const FlatMatrixPool& pool,
    int leaf_idx,
    const uint8_t* qbits,
    int qpc,
    int min_hits,
    std::vector<int32_t>& out_cids);

void flat_matrix_find_leaves_q(
    const FlatMatrixPool& pool,
    int leaf_idx,
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    int min_hits,
    std::vector<int32_t>& out_cids);

void zip_0_path_list(const int* sig, int sig_len, const int* one_index, int one_len, std::vector<int>& out);

void zip_0_path_list_into(
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    std::vector<int>& out);

void compact_find_leaves(
    const MgnCompactTree& tree,
    const CompactSearchIndex& index,
    const std::vector<int>& value_mgn,
    std::vector<int32_t>& out_cids);

// Global OHS: each edge is a signature one-position. A target matches when
// every one-position on its path is present in the sorted query.
void global_ohs_find_leaves(
    const MgnCompactTree& tree,
    const CompactSearchIndex& index,
    const std::vector<int>& query_positions,
    std::vector<int32_t>& out_cids);
