#include "hpc_mgn_build.h"

#include "hpc_flat_search.h"
#include "hpc_sig_index.h"
#include "hpc_sig_io.h"

#include <unordered_map>

namespace {

std::vector<HpcPartitionNode*> unique_mgn_leaves(const std::vector<HpcPartitionNode*>& mgn_leaves) {
    std::vector<HpcPartitionNode*> out;
    out.reserve(mgn_leaves.size());
    std::unordered_map<HpcPartitionNode*, bool> seen;
    for (HpcPartitionNode* leaf : mgn_leaves) {
        if (leaf == nullptr || seen.find(leaf) != seen.end()) {
            continue;
        }
        seen[leaf] = true;
        out.push_back(leaf);
    }
    return out;
}

bool build_col_to_leaf(
    const std::vector<HpcPartitionNode*>& unique_leaves,
    std::unordered_map<int64_t, HpcPartitionNode*>* col_to_leaf,
    std::string* err) {
    if (col_to_leaf == nullptr) {
        if (err) {
            *err = "null col_to_leaf";
        }
        return false;
    }
    col_to_leaf->clear();
    int next_key = 0;
    for (HpcPartitionNode* leaf : unique_leaves) {
        if (leaf == nullptr) {
            continue;
        }
        leaf->mgn_key = next_key++;
        for (const int64_t cid : leaf->ids) {
            (*col_to_leaf)[cid] = leaf;
        }
    }
    return true;
}

}  // namespace

bool hpc_build_mgn_from_sigb(
    const std::string& sig_native_path,
    int num_perm,
    const std::vector<HpcPartitionNode*>& mgn_leaves,
    std::vector<HpcFlatMgnLeaf>* out,
    std::string* err) {
    if (out == nullptr) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    out->clear();

    const std::vector<HpcPartitionNode*> unique_leaves = unique_mgn_leaves(mgn_leaves);

    std::unordered_map<int64_t, HpcPartitionNode*> col_to_leaf;
    if (!build_col_to_leaf(unique_leaves, &col_to_leaf, err)) {
        return false;
    }

    std::unordered_map<int, HpcFlatMgnLeaf*> key_to_leaf;
    out->reserve(unique_leaves.size());
    for (HpcPartitionNode* leaf : unique_leaves) {
        if (leaf == nullptr || leaf->mgn_key < 0) {
            continue;
        }
        HpcFlatMgnLeaf item;
        item.mgn_key = leaf->mgn_key;
        item.L = static_cast<int>(leaf->one_index.size());
        item.n_cols = static_cast<int>(leaf->ids.size());
        item.stop_depth = leaf->stop_depth;
        item.stop_depth_proxy = leaf->stop_depth_proxy;
        out->push_back(std::move(item));
        key_to_leaf[leaf->mgn_key] = &out->back();
    }

    return hpc_sigb_for_each_column(
        sig_native_path,
        [&](const HpcColumnSig& col) {
            const auto it = col_to_leaf.find(static_cast<int64_t>(col.col_id));
            if (it == col_to_leaf.end()) {
                return true;
            }
            HpcPartitionNode* leaf = it->second;
            HpcFlatMgnLeaf* item = key_to_leaf[leaf->mgn_key];
            if (item == nullptr) {
                return true;
            }
            (void)num_perm;
            const std::vector<int> decoded = hpc_sig_zip_to_index_one(
                col.sig.data(), static_cast<int>(col.sig.size()));
            const ZipPathQ qp = zip_0_path_q(
                decoded.data(),
                static_cast<int>(decoded.size()),
                leaf->one_index.data(),
                static_cast<int>(leaf->one_index.size()));
            HpcFlatMgnRow row;
            row.cid = static_cast<int64_t>(col.col_id);
            row.path_bits = qp.q;
            row.hit = qp.popcount;
            item->rows.push_back(row);
            return true;
        },
        err);
}
