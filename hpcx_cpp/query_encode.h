#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using TokenToId = std::unordered_map<std::string, uint32_t>;

struct BandEncodeMeta {
    int band = -1;
    std::vector<int32_t> sample_order;
    // Sorted unique codes + CSR index lists (load-time flat lookup).
    std::vector<int32_t> code_keys;
    std::vector<int32_t> code_offsets;
    std::vector<int32_t> code_indices;
};

struct QueryPreprocessed {
    std::vector<std::string> tokens;
    std::vector<int32_t> cids;
};

std::vector<std::string> query_identity_preprocess(const std::vector<std::string>& values);

QueryPreprocessed query_full_preprocess(
    const std::vector<std::string>& tokens,
    const TokenToId& token_to_id);

BandEncodeMeta band_encode_meta_load(int band, const std::string& refs_dir, std::string* err);

std::vector<int32_t> encode_query_for_band(
    const std::vector<std::string>& prepped_tokens,
    const TokenToId& token_to_id,
    const BandEncodeMeta& meta);

std::vector<int32_t> encode_query_for_band_from_cids(
    const std::vector<int32_t>& query_cids,
    const BandEncodeMeta& meta);

void merge_sorted_band_candidates(
    const std::vector<std::vector<int32_t>>& per_band,
    std::vector<int32_t>& merged_out);

bool token_to_id_load(const std::string& dict_path, TokenToId* out, std::string* err);

std::vector<std::string> parse_json_string_array_field(const std::string& json_line, const char* key);
