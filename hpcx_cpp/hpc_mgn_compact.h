#pragma once

#include <cstddef>
#include <vector>

#include "hpc_mgn_flat.h"

struct MgnCompactTree {
    int root_key = 1;
    // Version 2: node_bits are sample-order positions of signature ones, not 0/1 bits.
    bool position_trie = false;
    std::vector<int> node_keys;
    std::vector<int> parent_keys;
    std::vector<int> node_bits;  // -1 = root marker
    std::vector<int> child_off;
    std::vector<int> child_keys;
    std::vector<int> leaf_node_keys;
    std::vector<int> leaf_cids;
    std::vector<int> leaf_hits;
};

MgnCompactTree mgn_compact_from_flat_result(const MgnFlatBuildResult& flat);

MgnCompactTree mgn_flat_to_compact(
    const int* cids,
    const int* hits,
    const int* path_lens,
    const int* path_bits,
    int n_rows);

std::vector<unsigned char> mgn_compact_to_bytes(const MgnCompactTree& tree);
MgnCompactTree mgn_compact_from_bytes(const unsigned char* data, std::size_t len);
