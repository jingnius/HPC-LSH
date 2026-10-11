#include "hpc_mgn_flat.h"

#include <array>
#include <vector>

MgnFlatBuildResult mgn_flat_build(
    const int* cids,
    const int* hits,
    const int* path_lens,
    const int* path_bits,
    int n_rows) {
    MgnFlatBuildResult out;
    if (n_rows <= 0) {
        return out;
    }

    // child_map[node_id] -> [child_bit0_id, child_bit1_id], -1 if absent
    std::vector<std::array<int, 2>> child_map(1);
    child_map[0] = {-1, -1};

    int path_offset = 0;
    for (int row = 0; row < n_rows; ++row) {
        const int plen = path_lens[row];
        int cur = 0;
        for (int j = 0; j < plen; ++j) {
            const int bit = path_bits[path_offset + j] & 1;
            if (child_map[cur][bit] < 0) {
                const int new_id = static_cast<int>(child_map.size());
                child_map.push_back({-1, -1});
                child_map[cur][bit] = new_id;
                out.nodes.push_back(MgnFlatNode{cur, bit});
                cur = new_id;
            } else {
                cur = child_map[cur][bit];
            }
        }
        path_offset += plen;
        out.leaves.push_back(MgnFlatLeaf{cur, cids[row], hits[row]});
    }
    return out;
}
