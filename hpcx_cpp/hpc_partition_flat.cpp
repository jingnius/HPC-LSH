#include "hpc_partition_flat.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

static void write_u32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
}

static uint32_t read_u32(const unsigned char*& p) {
    uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    p += 4;
    return v;
}

static void write_i32(std::vector<unsigned char>& out, int v) {
    write_u32(out, static_cast<uint32_t>(v));
}

static int read_i32(const unsigned char*& p) {
    return static_cast<int>(read_u32(p));
}

static std::vector<std::unordered_set<int>> node_oip_sets(const HpcFlatPart& p, int node) {
    std::vector<std::unordered_set<int>> out;
    if (node < 0 || node >= static_cast<int>(p.oip_num_parts.size())) {
        return out;
    }
    const int np = p.oip_num_parts[static_cast<std::size_t>(node)];
    out.resize(static_cast<std::size_t>(np));
    int pos = p.oip_off[static_cast<std::size_t>(node)];
    for (int i = 0; i < np; ++i) {
        const int nh = p.oip_pool[static_cast<std::size_t>(pos++)];
        for (int j = 0; j < nh; ++j) {
            out[static_cast<std::size_t>(i)].insert(p.oip_pool[static_cast<std::size_t>(pos++)]);
        }
    }
    return out;
}

static int query_mask_from_sig(
    const std::vector<int>& sig,
    const std::vector<std::unordered_set<int>>& oip) {
    if (sig.empty() || oip.empty()) {
        return 0;
    }
    std::vector<int> sig_mut = sig;
    std::unordered_set<int> query_segments;
    for (std::size_t i = 0; i < oip.size(); ++i) {
        for (std::size_t k = 0; k < sig_mut.size(); ++k) {
            const int s = sig_mut[k];
            if (oip[i].find(s) != oip[i].end()) {
                query_segments.insert(static_cast<int>(i));
                sig_mut.erase(sig_mut.begin() + static_cast<std::ptrdiff_t>(k));
                break;
            }
        }
    }
    int mask = 0;
    for (int seg : query_segments) {
        mask |= (1 << seg);
    }
    return mask;
}

std::vector<unsigned char> partition_flat_encode(const HpcFlatPart& p) {
    std::vector<unsigned char> out;
    const char magic[8] = {'H', 'P', 'C', 'P', 'F', 'L', 'T', '2'};
    out.insert(out.end(), magic, magic + 8);
    write_i32(out, p.root_node);
    const int n = static_cast<int>(p.node_is_leaf.size());
    write_i32(out, n);
    auto write_vec = [&](const std::vector<int>& v) {
        write_i32(out, static_cast<int>(v.size()));
        for (int x : v) {
            write_i32(out, x);
        }
    };
    write_vec(p.one_index_pool);
    write_vec(p.one_off);
    write_vec(p.one_len);
    write_vec(p.oip_pool);
    write_vec(p.oip_off);
    write_vec(p.oip_num_parts);
    write_vec(p.route_mask);
    write_vec(p.route_child_ids);
    write_vec(p.route_child_off);
    write_vec(p.route_ent_off);
    write_vec(p.route_ent_len);
    write_vec(p.node_mgn_key);
    out.insert(out.end(), p.node_is_leaf.begin(), p.node_is_leaf.end());
    return out;
}

HpcFlatPart partition_flat_decode(const unsigned char* data, std::size_t len) {
    if (len < 12) {
        throw std::runtime_error("partition_flat_decode: too short");
    }
    const unsigned char* p = data;
    if (std::memcmp(p, "HPCPFLT2", 8) != 0) {
        throw std::runtime_error("partition_flat_decode: bad magic");
    }
    p += 8;
    HpcFlatPart out;
    out.root_node = read_i32(p);
    const int n = read_i32(p);
    auto read_vec = [&]() {
        std::vector<int> v;
        const int m = read_i32(p);
        v.reserve(static_cast<std::size_t>(m));
        for (int i = 0; i < m; ++i) {
            v.push_back(read_i32(p));
        }
        return v;
    };
    out.one_index_pool = read_vec();
    out.one_off = read_vec();
    out.one_len = read_vec();
    out.oip_pool = read_vec();
    out.oip_off = read_vec();
    out.oip_num_parts = read_vec();
    out.route_mask = read_vec();
    out.route_child_ids = read_vec();
    out.route_child_off = read_vec();
    out.route_ent_off = read_vec();
    out.route_ent_len = read_vec();
    out.node_mgn_key = read_vec();
    if (static_cast<std::size_t>(p - data) + static_cast<std::size_t>(n) > len) {
        throw std::runtime_error("partition_flat_decode: truncated flags");
    }
    out.node_is_leaf.assign(p, p + n);
    return out;
}

const int* partition_flat_one_index_ptr(const HpcFlatPart& p, int node, int& out_len) {
    out_len = 0;
    if (node < 0 || node >= static_cast<int>(p.one_off.size())) {
        return nullptr;
    }
    out_len = p.one_len[static_cast<std::size_t>(node)];
    if (out_len <= 0) {
        return p.one_index_pool.data();
    }
    return p.one_index_pool.data() + p.one_off[static_cast<std::size_t>(node)];
}

std::vector<int> partition_flat_filter_sig(
    const HpcFlatPart& p,
    int node,
    const std::vector<int>& sig) {
    std::vector<int> out;
    if (sig.empty()) {
        return out;
    }
    int len = 0;
    const int* one = partition_flat_one_index_ptr(p, node, len);
    if (one == nullptr || len <= 0) {
        return out;
    }
    std::unordered_set<int> idx_set(one, one + len);
    out.reserve(sig.size());
    for (int h : sig) {
        if (idx_set.find(h) != idx_set.end()) {
            out.push_back(h);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int> partition_flat_route_children(
    const HpcFlatPart& p,
    int node,
    const std::vector<int>& sig_f) {
    std::vector<int> out;
    if (node < 0 || node >= static_cast<int>(p.route_ent_off.size())) {
        return out;
    }
    const int e0 = p.route_ent_off[static_cast<std::size_t>(node)];
    const int el = p.route_ent_len[static_cast<std::size_t>(node)];
    if (el <= 0) {
        return out;
    }
    const auto oip = node_oip_sets(p, node);
    const int mask = query_mask_from_sig(sig_f, oip);
    for (int i = 0; i < el; ++i) {
        const int ei = e0 + i;
        if (p.route_mask[static_cast<std::size_t>(ei)] == mask) {
            const int c0 = p.route_child_off[static_cast<std::size_t>(ei)];
            const int c1 = p.route_child_off[static_cast<std::size_t>(ei + 1)];
            for (int j = c0; j < c1; ++j) {
                out.push_back(p.route_child_ids[static_cast<std::size_t>(j)]);
            }
            return out;
        }
    }
    return out;
}

bool partition_flat_is_leaf(const HpcFlatPart& p, int node) {
    if (node < 0 || node >= static_cast<int>(p.node_is_leaf.size())) {
        return false;
    }
    return p.node_is_leaf[static_cast<std::size_t>(node)] != 0;
}

int partition_flat_mgn_key(const HpcFlatPart& p, int node) {
    if (node < 0 || node >= static_cast<int>(p.node_mgn_key.size())) {
        return -1;
    }
    return p.node_mgn_key[static_cast<std::size_t>(node)];
}
