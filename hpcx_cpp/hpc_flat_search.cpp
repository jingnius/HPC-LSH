#include "hpc_flat_search.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

int zip_0_path_popcount(const int* sig, int sig_len, const int* one_index, int one_len) {
    if (one_len <= 0) {
        return 0;
    }
    int i = 0;
    int count = 0;
    for (int j = 0; j < one_len; ++j) {
        const int index = one_index[j];
        while (i < sig_len && sig[i] < index) {
            ++i;
        }
        if (i < sig_len && sig[i] == index) {
            ++count;
            ++i;
        }
    }
    return count;
}

ZipPathQ zip_0_path_q(const int* sig, int sig_len, const int* one_index, int one_len) {
    ZipPathQ out;
    if (one_len <= 0) {
        return out;
    }
    int i = 0;
    for (int j = 0; j < one_len; ++j) {
        const int index = one_index[j];
        while (i < sig_len && sig[i] < index) {
            ++i;
        }
        out.q <<= 1;
        if (i < sig_len && sig[i] == index) {
            out.q |= 1ULL;
            ++out.popcount;
            ++i;
        }
    }
    return out;
}

uint64_t zip_0_path_to_int(const int* sig, int sig_len, const int* one_index, int one_len) {
    uint64_t q = 0;
    if (one_len <= 0) {
        return 0;
    }
    int i = 0;
    for (int j = 0; j < one_len; ++j) {
        const int index = one_index[j];
        while (i < sig_len && sig[i] < index) {
            ++i;
        }
        q <<= 1;
        if (i < sig_len && sig[i] == index) {
            q |= 1ULL;
            ++i;
        }
    }
    return q;
}

void zip_0_path_list_into(
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    std::vector<int>& out) {
    out.clear();
    if (one_len <= 0) {
        return;
    }
    out.resize(static_cast<std::size_t>(one_len));
    int i = 0;
    for (int j = 0; j < one_len; ++j) {
        const int index = one_index[j];
        while (i < sig_len && sig[i] < index) {
            ++i;
        }
        if (i < sig_len && sig[i] == index) {
            out[static_cast<std::size_t>(j)] = 1;
            ++i;
        } else {
            out[static_cast<std::size_t>(j)] = 0;
        }
    }
}

void zip_0_path_list(
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    std::vector<int>& out) {
    zip_0_path_list_into(sig, sig_len, one_index, one_len, out);
}

static inline bool row_passes_q(uint64_t s, uint64_t q, int min_hits, int qpc, int sp) {
    if ((s & ~q) != 0ULL) {
        return false;
    }
    if (min_hits > 0) {
        if (sp < min_hits || sp > qpc) {
            return false;
        }
        const int hits = popcount64(s & q);
        if (hits < min_hits) {
            return false;
        }
    }
    return true;
}

static void flat_packed_emit_row(
    const FlatPackedPool& pool,
    int64_t row,
    uint64_t q,
    int qpc,
    int min_hits,
    std::vector<int32_t>& out_cids) {
    const uint64_t s = pool.bits[row];
    int sp = popcount64(s);
    if (pool.spop != nullptr) {
        sp = static_cast<int>(pool.spop[row]);
    }
    if (!row_passes_q(s, q, min_hits, qpc, sp)) {
        return;
    }
    out_cids.push_back(pool.cids[row]);
}

void zip_0_path_words(
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    std::vector<uint64_t>& words,
    int& popcount) {
    popcount = 0;
    const int n_words = one_len > 0 ? (one_len + 63) >> 6 : 0;
    words.assign(static_cast<std::size_t>(n_words), 0);
    if (n_words <= 0 || one_index == nullptr) {
        return;
    }
    int i = 0;
    for (int j = 0; j < one_len; ++j) {
        const int index = one_index[j];
        while (i < sig_len && sig != nullptr && sig[i] < index) {
            ++i;
        }
        if (sig != nullptr && i < sig_len && sig[i] == index) {
            words[static_cast<std::size_t>(j >> 6)] |= 1ULL << (j & 63);
            ++popcount;
            ++i;
        }
    }
}

void flat_wide_find_leaves_q(
    const FlatWidePool& pool,
    int leaf_idx,
    const uint64_t* q_words,
    int n_words,
    int qpc,
    int min_hits,
    std::vector<int32_t>& out_cids) {
    if (min_hits > 0 && qpc < min_hits) {
        return;
    }
    if (q_words == nullptr || n_words <= 0 || n_words != pool.n_words) {
        return;
    }
    if (leaf_idx < 0 || leaf_idx >= pool.n_leaves) {
        return;
    }
    const int64_t off = pool.leaf_off[leaf_idx];
    const int32_t n = pool.leaf_n[leaf_idx];
    if (n <= 0) {
        return;
    }
    for (int32_t ri = 0; ri < n; ++ri) {
        const uint64_t* s = pool.bits + (off + ri) * n_words;
        bool subset = true;
        for (int w = 0; w < n_words; ++w) {
            if ((s[w] & ~q_words[w]) != 0ULL) {
                subset = false;
                break;
            }
        }
        if (!subset) {
            continue;
        }
        int hits = 0;
        for (int w = 0; w < n_words; ++w) {
            hits += popcount64(s[w]);
        }
        if (min_hits > 0 && hits < min_hits) {
            continue;
        }
        out_cids.push_back(pool.cids[off + ri]);
    }
}

void flat_packed_find_leaves_q(
    const FlatPackedPool& pool,
    int leaf_idx,
    uint64_t q,
    int qpc,
    int min_hits,
    std::vector<int32_t>& out_cids) {
    if (min_hits > 0 && qpc < min_hits) {
        return;
    }
    if (leaf_idx < 0 || leaf_idx >= pool.n_leaves) {
        return;
    }
    const int64_t off = pool.leaf_off[leaf_idx];
    const int32_t n = pool.leaf_n[leaf_idx];
    if (n <= 0) {
        return;
    }
    const uint64_t not_q = ~q;
    const uint64_t* bits = pool.bits + off;
#if defined(__AVX2__)
    int32_t i = 0;
    const __m256i vnot_q = _mm256_set1_epi64x(static_cast<int64_t>(not_q));
    for (; i + 4 <= n; i += 4) {
        const __m256i sv = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bits + i));
        const __m256i masked = _mm256_and_si256(sv, vnot_q);
        const __m256i zero = _mm256_setzero_si256();
        const __m256i cmp = _mm256_cmpeq_epi64(masked, zero);
        const int pass_mask = _mm256_movemask_epi8(cmp);
        if (pass_mask != -1) {
            for (int32_t k = 0; k < 4; ++k) {
                const int64_t row = off + i + k;
                if ((bits[i + k] & not_q) != 0ULL) {
                    continue;
                }
                flat_packed_emit_row(pool, row, q, qpc, min_hits, out_cids);
            }
            continue;
        }
        for (int32_t k = 0; k < 4; ++k) {
            const int64_t row = off + i + k;
            flat_packed_emit_row(pool, row, q, qpc, min_hits, out_cids);
        }
    }
    for (; i < n; ++i) {
        const int64_t row = off + i;
        if ((bits[i] & not_q) != 0ULL) {
            continue;
        }
        flat_packed_emit_row(pool, row, q, qpc, min_hits, out_cids);
    }
#else
    for (int32_t i = 0; i < n; ++i) {
        const int64_t row = off + i;
        if ((bits[i] & not_q) != 0ULL) {
            continue;
        }
        flat_packed_emit_row(pool, row, q, qpc, min_hits, out_cids);
    }
#endif
}

void matrix_qbits_from_sig(
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    int L,
    std::vector<uint8_t>& qbits_out,
    int& qpc_out) {
    qpc_out = 0;
    if (L <= 0 || one_len <= 0) {
        qbits_out.clear();
        return;
    }
    qbits_out.assign(static_cast<std::size_t>(L), 0);
    int i = 0;
    const int tail_start = one_len - L;
    for (int j = 0; j < one_len; ++j) {
        const int index = one_index[j];
        while (i < sig_len && sig[i] < index) {
            ++i;
        }
        const uint8_t bit =
            (i < sig_len && sig[i] == index) ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
        if (bit) {
            ++qpc_out;
            ++i;
        }
        if (j >= tail_start) {
            qbits_out[static_cast<std::size_t>(j - tail_start)] = bit;
        }
    }
}

void flat_matrix_find_leaves_q_cached(
    const FlatMatrixPool& pool,
    int leaf_idx,
    const uint8_t* qbits,
    int qpc,
    int min_hits,
    std::vector<int32_t>& out_cids) {
    if (min_hits > 0 && qpc < min_hits) {
        return;
    }
    if (leaf_idx < 0 || leaf_idx >= pool.n_leaves || pool.L <= 0 || qbits == nullptr) {
        return;
    }
    const int64_t off = pool.leaf_off[leaf_idx];
    const int32_t n = pool.leaf_n[leaf_idx];
    if (n <= 0) {
        return;
    }
    const int L = pool.L;
    for (int32_t ri = 0; ri < n; ++ri) {
        const int64_t row = off + ri;
        const uint8_t* row_mat = pool.mat + row * L;
        bool valid = true;
        int hits = 0;
        for (int j = 0; j < L; ++j) {
            const uint8_t mb = row_mat[j];
            const uint8_t qb = qbits[j];
            if (mb > qb) {
                valid = false;
                break;
            }
            hits += static_cast<int>(mb & qb);
        }
        if (!valid) {
            continue;
        }
        if (min_hits > 0 && pool.spop != nullptr) {
            const int sp = static_cast<int>(pool.spop[row]);
            if (sp < min_hits || sp > qpc) {
                continue;
            }
        }
        if (min_hits <= 0 || hits >= min_hits) {
            out_cids.push_back(pool.cids[row]);
        }
    }
}

void flat_matrix_find_leaves_q(
    const FlatMatrixPool& pool,
    int leaf_idx,
    const int* sig,
    int sig_len,
    const int* one_index,
    int one_len,
    int min_hits,
    std::vector<int32_t>& out_cids) {
    std::vector<uint8_t> qbits;
    int qpc = 0;
    matrix_qbits_from_sig(sig, sig_len, one_index, one_len, pool.L, qbits, qpc);
    flat_matrix_find_leaves_q_cached(pool, leaf_idx, qbits.data(), qpc, min_hits, out_cids);
}

CompactSearchIndex compact_build_index(const MgnCompactTree& tree) {
    CompactSearchIndex out;
    out.key_to_idx.reserve(tree.node_keys.size() * 2 + 1);
    for (std::size_t i = 0; i < tree.node_keys.size(); ++i) {
        out.key_to_idx[tree.node_keys[i]] = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < tree.leaf_node_keys.size(); ++i) {
        out.leaves_by_key[tree.leaf_node_keys[i]].push_back(tree.leaf_cids[i]);
    }
    return out;
}

static bool compare_func_int_str(int node_bit, int seq_val) {
    if (node_bit == 1 && seq_val == 0) {
        return false;
    }
    return true;
}

void global_ohs_find_leaves(
    const MgnCompactTree& tree,
    const CompactSearchIndex& index,
    const std::vector<int>& query_positions,
    std::vector<int32_t>& out_cids) {
    const auto idx_of = [&](int key) -> int {
        const auto it = index.key_to_idx.find(key);
        return it == index.key_to_idx.end() ? -1 : it->second;
    };
    const int root_idx = idx_of(tree.root_key);
    if (root_idx < 0) {
        return;
    }
    const auto has_pos = [&](int pos) -> bool {
        return std::binary_search(query_positions.begin(), query_positions.end(), pos);
    };
    std::vector<int> stack;
    stack.push_back(root_idx);
    while (!stack.empty()) {
        const int ni = stack.back();
        stack.pop_back();
        const int node_key = tree.node_keys[static_cast<std::size_t>(ni)];
        const auto leaf_it = index.leaves_by_key.find(node_key);
        if (leaf_it != index.leaves_by_key.end()) {
            out_cids.insert(out_cids.end(), leaf_it->second.begin(), leaf_it->second.end());
        }
        const int c0 = tree.child_off[static_cast<std::size_t>(ni)];
        const int c1 = tree.child_off[static_cast<std::size_t>(ni + 1)];
        for (int ci = c0; ci < c1; ++ci) {
            const int child_key = tree.child_keys[static_cast<std::size_t>(ci)];
            const int cni = idx_of(child_key);
            if (cni < 0) {
                continue;
            }
            const int pos = tree.node_bits[static_cast<std::size_t>(cni)];
            if (has_pos(pos)) {
                stack.push_back(cni);
            }
        }
    }
}

void compact_find_leaves(
    const MgnCompactTree& tree,
    const CompactSearchIndex& index,
    const std::vector<int>& value_mgn,
    std::vector<int32_t>& out_cids) {
    const auto idx_of = [&](int key) -> int {
        const auto it = index.key_to_idx.find(key);
        return it == index.key_to_idx.end() ? -1 : it->second;
    };

    const int root_idx = idx_of(tree.root_key);
    if (root_idx < 0) {
        return;
    }

    std::vector<std::pair<int, int>> queue;
    queue.reserve(64);
    if (tree.node_bits[static_cast<std::size_t>(root_idx)] == -1) {
        const int c0 = tree.child_off[static_cast<std::size_t>(root_idx)];
        const int c1 = tree.child_off[static_cast<std::size_t>(root_idx + 1)];
        for (int ci = c0; ci < c1; ++ci) {
            const int child_key = tree.child_keys[static_cast<std::size_t>(ci)];
            const int cni = idx_of(child_key);
            if (cni >= 0) {
                queue.emplace_back(cni, 0);
            }
        }
    } else {
        queue.emplace_back(root_idx, 0);
    }

    std::size_t head = 0;
    while (head < queue.size()) {
        const int ni = queue[head].first;
        const int layer = queue[head].second;
        ++head;
        const int bit = tree.node_bits[static_cast<std::size_t>(ni)];
        const int seq_val =
            layer < static_cast<int>(value_mgn.size()) ? value_mgn[static_cast<std::size_t>(layer)] : 0;
        if (layer >= static_cast<int>(value_mgn.size()) || !compare_func_int_str(bit, seq_val)) {
            continue;
        }
        const int node_key = tree.node_keys[static_cast<std::size_t>(ni)];
        const int c0 = tree.child_off[static_cast<std::size_t>(ni)];
        const int c1 = tree.child_off[static_cast<std::size_t>(ni + 1)];
        for (int ci = c0; ci < c1; ++ci) {
            const int child_key = tree.child_keys[static_cast<std::size_t>(ci)];
            const int cni = idx_of(child_key);
            if (cni >= 0) {
                queue.emplace_back(cni, layer + 1);
            }
        }
        const auto leaf_it = index.leaves_by_key.find(node_key);
        if (leaf_it != index.leaves_by_key.end()) {
            out_cids.insert(out_cids.end(), leaf_it->second.begin(), leaf_it->second.end());
        }
    }
}
