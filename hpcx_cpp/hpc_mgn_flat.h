#pragma once

#include <utility>
#include <vector>

struct MgnFlatNode {
    int parent;  // 0 = virtual root
    int bit;     // 0 or 1
};

struct MgnFlatLeaf {
    int node_id;  // index in child-map (0 = root)
    int cid;
    int hit;
};

struct MgnFlatBuildResult {
    std::vector<MgnFlatNode> nodes;  // creation order == NaryTree key order
    std::vector<MgnFlatLeaf> leaves;
};

// paths: concatenated bits; path_lens: length per row; parallel to cids/hits
MgnFlatBuildResult mgn_flat_build(
    const int* cids,
    const int* hits,
    const int* path_lens,
    const int* path_bits,
    int n_rows);
