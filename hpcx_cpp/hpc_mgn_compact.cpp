#include "hpc_mgn_compact.h"

#include <stdexcept>

#include <unordered_map>
#include <utility>
#include <vector>

MgnCompactTree mgn_compact_from_flat_result(const MgnFlatBuildResult& flat) {
    MgnCompactTree out;
    out.root_key = 1;

    if (flat.nodes.empty() && flat.leaves.empty()) {
        out.node_keys = {1};
        out.parent_keys = {-1};
        out.node_bits = {-1};
        out.child_off = {0, 0};
        return out;
    }

    std::unordered_map<int, int> id_to_key;
    id_to_key[0] = 1;
    int next_key = 2;

    std::unordered_map<int, std::vector<int>> children;

    out.node_keys.push_back(1);
    out.parent_keys.push_back(-1);
    out.node_bits.push_back(-1);

    for (std::size_t i = 0; i < flat.nodes.size(); ++i) {
        const int parent_id = flat.nodes[i].parent;
        const int bit = flat.nodes[i].bit;
        const int parent_key = id_to_key[parent_id];
        const int key = next_key++;
        const int node_id = static_cast<int>(i) + 1;
        id_to_key[node_id] = key;

        out.node_keys.push_back(key);
        out.parent_keys.push_back(parent_key);
        out.node_bits.push_back(bit);
        children[parent_key].push_back(key);
    }

    out.child_off.reserve(out.node_keys.size() + 1);
    out.child_keys.clear();
    for (int key : out.node_keys) {
        out.child_off.push_back(static_cast<int>(out.child_keys.size()));
        const auto it = children.find(key);
        if (it != children.end()) {
            for (int ck : it->second) {
                out.child_keys.push_back(ck);
            }
        }
    }
    out.child_off.push_back(static_cast<int>(out.child_keys.size()));

    for (const auto& leaf : flat.leaves) {
        const auto it = id_to_key.find(leaf.node_id);
        if (it == id_to_key.end()) {
            continue;
        }
        out.leaf_node_keys.push_back(it->second);
        out.leaf_cids.push_back(leaf.cid);
        out.leaf_hits.push_back(leaf.hit);
    }

    return out;
}

MgnCompactTree mgn_flat_to_compact(
    const int* cids,
    const int* hits,
    const int* path_lens,
    const int* path_bits,
    int n_rows) {
    if (n_rows <= 0) {
        MgnFlatBuildResult empty;
        return mgn_compact_from_flat_result(empty);
    }
    const MgnFlatBuildResult flat = mgn_flat_build(cids, hits, path_lens, path_bits, n_rows);
    return mgn_compact_from_flat_result(flat);
}

static void append_i32(std::vector<unsigned char>& out, int v) {
    out.push_back(static_cast<unsigned char>(v & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
}

static int read_i32(const unsigned char*& p, const unsigned char* end) {
    if (p + 4 > end) {
        throw std::runtime_error("compact bytes truncated");
    }
    int v = static_cast<int>(p[0]) | (static_cast<int>(p[1]) << 8) |
            (static_cast<int>(p[2]) << 16) | (static_cast<int>(p[3]) << 24);
    p += 4;
    return v;
}

std::vector<unsigned char> mgn_compact_to_bytes(const MgnCompactTree& tree) {
    std::vector<unsigned char> out;
    out.reserve(64);
    out.push_back('M');
    out.push_back('G');
    out.push_back('N');
    out.push_back('C');
    append_i32(out, tree.position_trie ? 2 : 1);
    append_i32(out, tree.root_key);
    const int n = static_cast<int>(tree.node_keys.size());
    append_i32(out, n);
    for (int i = 0; i < n; ++i) {
        append_i32(out, tree.node_keys[static_cast<std::size_t>(i)]);
    }
    for (int i = 0; i < n; ++i) {
        append_i32(out, tree.parent_keys[static_cast<std::size_t>(i)]);
    }
    for (int i = 0; i < n; ++i) {
        append_i32(out, tree.node_bits[static_cast<std::size_t>(i)]);
    }
    const int noff = static_cast<int>(tree.child_off.size());
    append_i32(out, noff);
    for (int i = 0; i < noff; ++i) {
        append_i32(out, tree.child_off[static_cast<std::size_t>(i)]);
    }
    const int nc = static_cast<int>(tree.child_keys.size());
    append_i32(out, nc);
    for (int i = 0; i < nc; ++i) {
        append_i32(out, tree.child_keys[static_cast<std::size_t>(i)]);
    }
    const int nl = static_cast<int>(tree.leaf_node_keys.size());
    append_i32(out, nl);
    for (int i = 0; i < nl; ++i) {
        append_i32(out, tree.leaf_node_keys[static_cast<std::size_t>(i)]);
        append_i32(out, tree.leaf_cids[static_cast<std::size_t>(i)]);
        append_i32(out, tree.leaf_hits[static_cast<std::size_t>(i)]);
    }
    return out;
}

MgnCompactTree mgn_compact_from_bytes(const unsigned char* data, std::size_t len) {
    MgnCompactTree out;
    const unsigned char* p = data;
    const unsigned char* end = data + len;
    if (len < 8 || p[0] != 'M' || p[1] != 'G' || p[2] != 'N' || p[3] != 'C') {
        throw std::runtime_error("bad compact magic");
    }
    p += 4;
    const int version = read_i32(p, end);
    if (version != 1 && version != 2) {
        throw std::runtime_error("unsupported compact version");
    }
    out.position_trie = version == 2;
    out.root_key = read_i32(p, end);
    const int n = read_i32(p, end);
    out.node_keys.resize(static_cast<std::size_t>(n));
    out.parent_keys.resize(static_cast<std::size_t>(n));
    out.node_bits.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        out.node_keys[static_cast<std::size_t>(i)] = read_i32(p, end);
    }
    for (int i = 0; i < n; ++i) {
        out.parent_keys[static_cast<std::size_t>(i)] = read_i32(p, end);
    }
    for (int i = 0; i < n; ++i) {
        out.node_bits[static_cast<std::size_t>(i)] = read_i32(p, end);
    }
    const int noff = read_i32(p, end);
    out.child_off.resize(static_cast<std::size_t>(noff));
    for (int i = 0; i < noff; ++i) {
        out.child_off[static_cast<std::size_t>(i)] = read_i32(p, end);
    }
    const int nc = read_i32(p, end);
    out.child_keys.resize(static_cast<std::size_t>(nc));
    for (int i = 0; i < nc; ++i) {
        out.child_keys[static_cast<std::size_t>(i)] = read_i32(p, end);
    }
    const int nl = read_i32(p, end);
    out.leaf_node_keys.resize(static_cast<std::size_t>(nl));
    out.leaf_cids.resize(static_cast<std::size_t>(nl));
    out.leaf_hits.resize(static_cast<std::size_t>(nl));
    for (int i = 0; i < nl; ++i) {
        out.leaf_node_keys[static_cast<std::size_t>(i)] = read_i32(p, end);
        out.leaf_cids[static_cast<std::size_t>(i)] = read_i32(p, end);
        out.leaf_hits[static_cast<std::size_t>(i)] = read_i32(p, end);
    }
    return out;
}
