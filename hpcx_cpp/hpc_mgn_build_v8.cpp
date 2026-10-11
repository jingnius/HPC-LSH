#include "hpc_partition_bfs_v8.h"

#include "hpc_flat_search.h"
#include "hpc_mgn_build.h"
#include "hpc_mgn_compact.h"
#include "hpc_sig_index.h"
#include "hpc_sig_io.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

bool env_on(const char* key) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return !(s == "0" || s == "false" || s == "no");
}

bool global_ohs_enabled() {
    return env_on("HPC_GLOBAL_OHS");
}

bool one_level_ohs_enabled() {
    return env_on("HPC_ONE_LEVEL");
}

struct GlobalOhsTrie {
    // One node per signature one-position. Zero-runs are not materialized.
    std::vector<std::map<int, int>> child_at{std::map<int, int>{}};
    MgnFlatBuildResult flat;

    void add(const int* hits, int n_hits, int cid, int min_hits) {
        if (min_hits > 0 && n_hits < min_hits) {
            return;
        }
        int cur = 0;
        for (int j = 0; j < n_hits; ++j) {
            const int pos = hits[j];
            const auto it = child_at[static_cast<std::size_t>(cur)].find(pos);
            if (it == child_at[static_cast<std::size_t>(cur)].end()) {
                if (child_at.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max() - 2)) {
                    throw std::runtime_error("global OHS trie exceeded 32-bit node ids");
                }
                const int new_id = static_cast<int>(child_at.size());
                child_at.emplace_back();
                child_at[static_cast<std::size_t>(cur)].emplace(pos, new_id);
                flat.nodes.push_back(MgnFlatNode{cur, pos});
                cur = new_id;
            } else {
                cur = it->second;
            }
        }
        flat.leaves.push_back(MgnFlatLeaf{cur, cid, n_hits});
    }
};

}  // namespace

bool hpc_build_mgn_from_sigb_v8(
    const std::string& sig_native_path,
    int num_perm,
    const std::vector<HpcPartitionNode*>& mgn_leaves,
    HpcPartitionV8Context& v8_ctx,
    std::vector<HpcFlatMgnLeaf>* out,
    std::string* err) {
    if (out == nullptr) {
        if (err) *err = "null out";
        return false;
    }
    out->clear();

    std::unordered_map<HpcPartitionNode*, bool> seen;
    std::vector<HpcPartitionNode*> unique_leaves;
    for (HpcPartitionNode* leaf : mgn_leaves) {
        if (leaf == nullptr || seen[leaf]) continue;
        seen[leaf] = true;
        unique_leaves.push_back(leaf);
    }

    const bool global_ohs = global_ohs_enabled();
    const bool position_ohs = global_ohs || one_level_ohs_enabled();
    std::unordered_map<HpcPartitionNode*, GlobalOhsTrie> global_tries;
    int next_key = 0;
    std::unordered_map<int64_t, HpcPartitionNode*> col_to_leaf;
    for (HpcPartitionNode* leaf : unique_leaves) {
        leaf->mgn_key = next_key++;
        const auto rit = v8_ctx.ranges.find(leaf);
        if (rit == v8_ctx.ranges.end()) continue;
        for (uint32_t pi = rit->second.first; pi < rit->second.second; ++pi) {
            const uint32_t col_idx = v8_ctx.col_perm[pi];
            col_to_leaf[v8_ctx.cache.col_id(col_idx)] = leaf;
        }
    }

    std::unordered_map<int, HpcFlatMgnLeaf*> key_to_leaf;
    out->reserve(unique_leaves.size());
    for (HpcPartitionNode* leaf : unique_leaves) {
        if (leaf->mgn_key < 0) continue;
        HpcFlatMgnLeaf item;
        item.mgn_key = leaf->mgn_key;
        const std::vector<int32_t>& leaf_one = hpc_v8_node_one_index(*leaf, &v8_ctx);
        item.L = static_cast<int>(leaf_one.size());
        const auto rit = v8_ctx.ranges.find(leaf);
        item.n_cols = rit == v8_ctx.ranges.end() ? 0 : static_cast<int>(rit->second.second - rit->second.first);
        item.stop_depth = leaf->stop_depth;
        item.stop_depth_proxy = leaf->stop_depth_proxy;
        out->push_back(std::move(item));
        key_to_leaf[leaf->mgn_key] = &out->back();
    }

    if (!hpc_sigb_for_each_column(
        sig_native_path,
        [&](const HpcColumnSig& col) {
            const auto it = col_to_leaf.find(static_cast<int64_t>(col.col_id));
            if (it == col_to_leaf.end()) return true;
            HpcPartitionNode* leaf = it->second;
            HpcFlatMgnLeaf* item = key_to_leaf[leaf->mgn_key];
            if (item == nullptr) return true;
            const std::vector<int> decoded = hpc_sig_zip_to_index_one(col.sig.data(), static_cast<int>(col.sig.size()));
            const std::vector<int32_t>& leaf_one = hpc_v8_node_one_index(*leaf, &v8_ctx);
            if (position_ohs) {
                (void)leaf_one;
                global_tries[leaf].add(
                    decoded.data(),
                    static_cast<int>(decoded.size()),
                    static_cast<int>(col.col_id),
                    num_perm);
                return true;
            }
            const int L = static_cast<int>(leaf_one.size());
            HpcFlatMgnRow row;
            row.cid = static_cast<int64_t>(col.col_id);
            if (L > 64) {
                const int n_words = (L + 63) >> 6;
                if (item->wide_words == 0) {
                    item->wide_words = n_words;
                }
                const std::size_t base = item->wide_bits.size();
                item->wide_bits.resize(base + static_cast<std::size_t>(n_words), 0);
                int si = 0;
                int hit = 0;
                const int n_sig = static_cast<int>(decoded.size());
                const int* sig = decoded.data();
                for (int j = 0; j < L; ++j) {
                    const int index = leaf_one[static_cast<std::size_t>(j)];
                    while (si < n_sig && sig[si] < index) {
                        ++si;
                    }
                    if (si < n_sig && sig[si] == index) {
                        item->wide_bits[base + static_cast<std::size_t>(j >> 6)] |=
                            1ULL << (j & 63);
                        ++hit;
                        ++si;
                    }
                }
                row.hit = hit;
            } else {
                const ZipPathQ qp = zip_0_path_q(
                    decoded.data(), static_cast<int>(decoded.size()),
                    leaf_one.data(), L);
                row.path_bits = qp.q;
                row.hit = qp.popcount;
            }
            item->rows.push_back(row);
            return true;
        },
        err)) {
        return false;
    }
    if (!position_ohs) {
        return true;
    }
    for (HpcFlatMgnLeaf& item : *out) {
        HpcPartitionNode* leaf = nullptr;
        for (HpcPartitionNode* cand : unique_leaves) {
            if (cand != nullptr && cand->mgn_key == item.mgn_key) {
                leaf = cand;
                break;
            }
        }
        if (leaf == nullptr) {
            continue;
        }
        GlobalOhsTrie& trie = global_tries[leaf];
        item.store_compact = true;
        item.compact = mgn_compact_from_flat_result(trie.flat);
        item.compact.position_trie = true;
        item.n_cols = static_cast<int>(trie.flat.leaves.size());
        item.rows.clear();
        std::fprintf(
            stderr,
            "[%s] mgn_key=%d trie_nodes=%zu leaves=%zu\n",
            global_ohs ? "hpc_global_ohs" : "hpc_one_level_ohs",
            item.mgn_key,
            trie.flat.nodes.size(),
            trie.flat.leaves.size());
    }
    return true;
}
