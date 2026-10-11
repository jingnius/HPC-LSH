#include "hpc_partition_view.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>


static int read_i32(const unsigned char*& p, const unsigned char* end) {
    if (p + 4 > end) {
        throw std::runtime_error("partition_view: truncated");
    }
    const int v = static_cast<int>(
        static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
    p += 4;
    return v;
}

static const int* read_vec_ptr(const unsigned char*& p, const unsigned char* end, int& out_len) {
    out_len = read_i32(p, end);
    if (out_len < 0 || p + static_cast<std::size_t>(out_len) * 4 > end) {
        throw std::runtime_error("partition_view: bad vec");
    }
    const int* ptr = reinterpret_cast<const int*>(p);
    p += static_cast<std::size_t>(out_len) * 4;
    return ptr;
}

bool partition_view_attach(const unsigned char* data, std::size_t len, HpcFlatPartView& out) {
    if (len < 12 || std::memcmp(data, "HPCPFLT2", 8) != 0) {
        return false;
    }
    const unsigned char* p = data + 8;
    const unsigned char* end = data + len;
    out.root_node = read_i32(p, end);
    out.n_nodes = read_i32(p, end);
    out.one_index_pool = read_vec_ptr(p, end, out.one_index_pool_len);
    int n_off = 0;
    out.one_off = read_vec_ptr(p, end, n_off);
    int n_olen = 0;
    out.one_len = read_vec_ptr(p, end, n_olen);
    out.oip_pool = read_vec_ptr(p, end, out.oip_pool_len);
    int n_oip_off = 0;
    out.oip_off = read_vec_ptr(p, end, n_oip_off);
    int n_oip_np = 0;
    out.oip_num_parts = read_vec_ptr(p, end, n_oip_np);
    out.route_mask = read_vec_ptr(p, end, out.route_mask_len);
    out.route_child_ids = read_vec_ptr(p, end, out.route_child_ids_len);
    out.route_child_off = read_vec_ptr(p, end, out.route_child_off_len);
    int n_rent = 0;
    out.route_ent_off = read_vec_ptr(p, end, n_rent);
    int n_rent_len = 0;
    out.route_ent_len = read_vec_ptr(p, end, n_rent_len);
    int n_mgn = 0;
    out.node_mgn_key = read_vec_ptr(p, end, n_mgn);
    if (p + static_cast<std::size_t>(out.n_nodes) > end) {
        return false;
    }
    out.node_is_leaf = p;
    return true;
}

static const int* one_index_ptr(const HpcFlatPartView& p, int node, int& out_len) {
    out_len = 0;
    if (node < 0 || node >= p.n_nodes) {
        return nullptr;
    }
    out_len = p.one_len[node];
    if (out_len <= 0) {
        return p.one_index_pool;
    }
    return p.one_index_pool + p.one_off[node];
}

static bool oip_part_contains(const int* part, int nh, int h) {
    if (nh <= 0) {
        return false;
    }
    return std::binary_search(part, part + nh, h);
}

static int query_mask_from_sig(
    const int* sig,
    int sig_len,
    const int* oip_pool,
    int oip_start,
    int oip_num_parts) {
    if (sig_len <= 0 || oip_num_parts <= 0) {
        return 0;
    }
    static thread_local std::vector<uint8_t> used;
    if (static_cast<int>(used.size()) < sig_len) {
        used.resize(static_cast<std::size_t>(sig_len));
    }
    std::fill(used.begin(), used.begin() + sig_len, static_cast<uint8_t>(0));
    int mask = 0;
    int pos = oip_start;
    for (int i = 0; i < oip_num_parts; ++i) {
        const int nh = oip_pool[pos++];
        const int* part = oip_pool + pos;
        pos += nh;
        for (int k = 0; k < sig_len; ++k) {
            if (used[static_cast<std::size_t>(k)] != 0) {
                continue;
            }
            if (!oip_part_contains(part, nh, sig[k])) {
                continue;
            }
            mask |= (1 << i);
            used[static_cast<std::size_t>(k)] = 1;
            break;
        }
    }
    return mask;
}

void partition_view_filter_sig_into(
    const HpcFlatPartView& p,
    int node,
    const int* sig,
    int sig_len,
    std::vector<int>& out) {
    out.clear();
    if (sig_len <= 0) {
        return;
    }
    int len = 0;
    const int* one = one_index_ptr(p, node, len);
    if (one == nullptr || len <= 0) {
        return;
    }
    out.reserve(static_cast<std::size_t>(sig_len));
    int i = 0;
    int j = 0;
    while (i < sig_len && j < len) {
        if (sig[i] == one[j]) {
            out.push_back(sig[i]);
            ++i;
            ++j;
        } else if (sig[i] < one[j]) {
            ++i;
        } else {
            ++j;
        }
    }
}

std::vector<int> partition_view_filter_sig(
    const HpcFlatPartView& p,
    int node,
    const int* sig,
    int sig_len) {
    std::vector<int> out;
    partition_view_filter_sig_into(p, node, sig, sig_len, out);
    return out;
}

int partition_view_route_mask_for_sig(
    const HpcFlatPartView& p,
    int node,
    const int* sig,
    int sig_len) {
    if (node < 0 || node >= p.n_nodes || sig_len <= 0) {
        return 0;
    }
    return query_mask_from_sig(
        sig, sig_len, p.oip_pool, p.oip_off[node], p.oip_num_parts[node]);
}

std::vector<int> partition_view_route_children(
    const HpcFlatPartView& p,
    int node,
    const std::vector<int>& sig_f) {
    std::vector<int> out;
    if (node < 0 || node >= p.n_nodes) {
        return out;
    }
    const int e0 = p.route_ent_off[node];
    const int el = p.route_ent_len[node];
    if (el <= 0) {
        return out;
    }
    const int mask = partition_view_route_mask_for_sig(
        p, node, sig_f.data(), static_cast<int>(sig_f.size()));
    for (int i = 0; i < el; ++i) {
        const int ei = e0 + i;
        if (p.route_mask[ei] == mask) {
            const int c0 = p.route_child_off[ei];
            const int c1 = p.route_child_off[ei + 1];
            out.reserve(static_cast<std::size_t>(c1 - c0));
            for (int j = c0; j < c1; ++j) {
                out.push_back(p.route_child_ids[j]);
            }
            return out;
        }
    }
    return out;
}

bool partition_view_is_leaf(const HpcFlatPartView& p, int node) {
    if (node < 0 || node >= p.n_nodes) {
        return false;
    }
    return p.node_is_leaf[node] != 0;
}

int partition_view_mgn_key(const HpcFlatPartView& p, int node) {
    if (node < 0 || node >= p.n_nodes) {
        return -1;
    }
    return p.node_mgn_key[node];
}
