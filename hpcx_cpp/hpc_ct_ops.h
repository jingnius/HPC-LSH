#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct HpcIndexOneStore;

using HpcIndexOneMap = std::unordered_map<int64_t, std::vector<int32_t>>;

// compactTree partition helpers (ported from jing_compactTree bytecode).
std::vector<int32_t> hpc_adaptive_first1_boundaries(
    const std::vector<int64_t>& col_ids,
    const std::unordered_map<int64_t, int32_t>& first1_map,
    int k);

std::vector<int32_t> hpc_adaptive_first1_boundaries_nth(
    const std::vector<int64_t>& col_ids,
    const HpcIndexOneStore& store,
    int nth,
    int k);

using HpcOneIndexPartition = std::unordered_map<int, std::unordered_set<int32_t>>;

HpcOneIndexPartition hpc_assign_oneindex_adaptive(
    const std::vector<int32_t>& one_index_list,
    const std::vector<int32_t>& boundaries,
    int k);

/** Active hits in column index_one order (matches hpc_hot_core.active_hits_in_part). */
std::vector<int32_t> hpc_active_hits_in_part(
    const std::vector<int32_t>& ic_hits,
    const std::unordered_set<int32_t>& part_index_set);

using HpcPartCSKey = uint32_t;

std::vector<int> hpc_csp_cskey_bits_1index_zipfast_gai(
    const std::vector<int32_t>& active,
    const std::vector<int32_t>& one_index,
    const HpcOneIndexPartition& one_index_partition);

HpcPartCSKey hpc_cskey_pack_bits(const std::vector<int>& bits);

std::string hpc_cskey_format_packed(HpcPartCSKey key, int n_parts);

/** Segment indices with bit==1 in a packed child key. */
std::vector<int> hpc_cskey_active_segments(HpcPartCSKey key, int n_parts);

std::string hpc_csp_cskey_1index_zipfast_gai(
    const std::vector<int32_t>& active,
    const std::vector<int32_t>& one_index,
    const HpcOneIndexPartition& one_index_partition);

HpcPartCSKey hpc_csp_cskey_packed_1index_zipfast_gai(
    const std::vector<int32_t>& active,
    const std::vector<int32_t>& one_index,
    const HpcOneIndexPartition& one_index_partition);

std::string hpc_format_py_list_key(const std::vector<int>& bits);
