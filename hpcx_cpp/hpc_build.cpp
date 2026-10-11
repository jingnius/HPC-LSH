#include "hpc_build.h"

#include "hpc_band_format.h"
#include "hpc_hpcx.h"
#include "hpc_mgn_compact.h"
#include "hpc_partition_flat.h"
#include "hpc_pickle.h"
#include "hpcx_write.h"
#include "hpc_zip.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct FlatRow {
    int64_t cid = 0;
    unsigned __int128 path = 0;
    std::vector<uint8_t> path_bytes;
    int hit = 0;
};

struct FlatItem {
    int key = 0;
    std::vector<FlatRow> rows;
    int L = 0;
};

struct PartBuildStats {
    int partition_nodes = 0;
    int partition_flat_bytes = 0;
    int route_entries = 0;
    int index_pool_len = 0;
    int leaf_nodes = 0;
    int leaf_nodes_expected = 0;
    bool leaf_match = false;
};

struct PartNodeData {
    std::vector<int> one_index;
    std::vector<std::vector<int>> oip;
    std::vector<std::pair<int, std::vector<int>>> route;
    bool is_leaf = false;
    int mgn_key = -1;
};

const PickleValue* dict_child(const PickleDict* d, const char* key) {
    if (d == nullptr) {
        return nullptr;
    }
    const PicklePtr* p = d->get(key);
    return p != nullptr && *p ? p->get() : nullptr;
}

int dict_int(const PickleDict* d, const char* key, int def = 0) {
    const PickleValue* v = dict_child(d, key);
    return v != nullptr && v->kind == PickleValue::kInt ? static_cast<int>(v->i) : def;
}

const PickleDict* as_dict_ptr(const PickleValue* v) {
    return v != nullptr ? v->as_dict() : nullptr;
}

const PickleList* as_list_ptr(const PickleValue* v) {
    return v != nullptr ? v->as_list() : nullptr;
}

std::vector<int> sort_int64_hits(std::vector<int64_t> vals) {
    if (vals.empty()) {
        return {};
    }
    int64_t min_v = vals[0];
    int64_t max_v = vals[0];
    for (int64_t x : vals) {
        min_v = std::min(min_v, x);
        max_v = std::max(max_v, x);
    }
    if (min_v >= 0) {
        const std::size_t span = static_cast<std::size_t>(max_v - min_v + 1);
        if (span <= static_cast<std::size_t>(1) << 24) {
            std::vector<int32_t> counts(span, 0);
            for (int64_t x : vals) {
                counts[static_cast<std::size_t>(x - min_v)] += 1;
            }
            std::vector<int> out;
            out.reserve(vals.size());
            for (std::size_t bucket = 0; bucket < span; ++bucket) {
                const int value = static_cast<int>(static_cast<int64_t>(bucket) + min_v);
                for (int32_t c = 0; c < counts[bucket]; ++c) {
                    out.push_back(value);
                }
            }
            return out;
        }
    }
    std::sort(vals.begin(), vals.end());
    std::vector<int> out;
    out.reserve(vals.size());
    for (int64_t x : vals) {
        out.push_back(static_cast<int>(x));
    }
    return out;
}

std::vector<int> sorted_hits(const PickleValue* v) {
    std::vector<int64_t> tmp;
    if (v == nullptr) {
        return {};
    }
    if (const PickleList* lst = as_list_ptr(v)) {
        tmp.reserve(lst->size());
        for (const PicklePtr& item : *lst) {
            if (item && item->kind == PickleValue::kInt) {
                tmp.push_back(item->i);
            }
        }
    } else if (v->kind == PickleValue::kSet) {
        tmp.reserve(v->set_ints.size());
        for (int64_t x : v->set_ints) {
            tmp.push_back(x);
        }
    }
    return sort_int64_hits(std::move(tmp));
}

int oip_len(const PickleValue* oip) {
    if (const PickleDict* d = as_dict_ptr(oip)) {
        return static_cast<int>(d->size());
    }
    if (const PickleList* lst = as_list_ptr(oip)) {
        return static_cast<int>(lst->size());
    }
    return 0;
}

const PickleValue* oip_part(const PickleValue* oip, int i) {
    if (const PickleDict* d = as_dict_ptr(oip)) {
        if (const PicklePtr* p = d->get(std::to_string(i))) {
            return p && *p ? p->get() : nullptr;
        }
        if (const PicklePtr* p = d->get(std::to_string(static_cast<long long>(i)))) {
            return p && *p ? p->get() : nullptr;
        }
    }
    if (const PickleList* lst = as_list_ptr(oip)) {
        if (i >= 0 && i < static_cast<int>(lst->size())) {
            const PicklePtr& ptr = (*lst)[static_cast<std::size_t>(i)];
            return ptr ? ptr.get() : nullptr;
        }
    }
    return nullptr;
}

int path_bit_msb(const FlatRow& row, int L, int j) {
    const int bit_pos = L - 1 - j;
    if (!row.path_bytes.empty()) {
        const std::size_t byte_idx = static_cast<std::size_t>(bit_pos / 8);
        if (byte_idx >= row.path_bytes.size()) {
            return 0;
        }
        return static_cast<int>((row.path_bytes[byte_idx] >> (bit_pos % 8)) & 1u);
    }
    return static_cast<int>((row.path >> bit_pos) & 1u);
}

int popcount_path(const FlatRow& row, int L) {
    int n = 0;
    for (int j = 0; j < L; ++j) {
        n += path_bit_msb(row, L, j);
    }
    return n;
}

uint64_t path_to_bits64(const FlatRow& row, int L) {
    uint64_t out = 0;
    for (int j = 0; j < L; ++j) {
        out = (out << 1) | static_cast<uint64_t>(path_bit_msb(row, L, j));
    }
    return out;
}

unsigned __int128 pickle_uint128(const PickleValue* v) {
    if (v == nullptr || v->kind != PickleValue::kInt) {
        return 0;
    }
    if (v->wide) {
        return v->u;
    }
    return v->i >= 0 ? static_cast<unsigned __int128>(v->i)
                     : static_cast<unsigned __int128>(v->i);  // path ints are non-negative
}

FlatRow normalize_row(const PickleList& row) {
    FlatRow out;
    if (row.size() < 3) {
        return out;
    }
    if (row[0] && row[0]->kind == PickleValue::kInt) {
        out.cid = row[0]->i;
    }
    if (row[1] && row[1]->kind == PickleValue::kInt) {
        if (!row[1]->long_bytes.empty()) {
            out.path_bytes = row[1]->long_bytes;
            out.path = pickle_uint128(row[1].get());
        } else {
            out.path = pickle_uint128(row[1].get());
        }
    } else if (row[1]) {
        if (const PickleList* bits = as_list_ptr(row[1].get())) {
            uint64_t v = 0;
            for (const PicklePtr& b : *bits) {
                v = (v << 1) | (b && b->kind == PickleValue::kInt && b->i ? 1u : 0u);
            }
            out.path = static_cast<unsigned __int128>(v);
        }
    }
    if (row[2] && row[2]->kind == PickleValue::kInt) {
        out.hit = static_cast<int>(row[2]->i);
    }
    return out;
}

int infer_L(const PickleList* rows_raw, const PickleDict* tree_data) {
    if (tree_data != nullptr) {
        const PickleValue* lv = dict_child(tree_data, "L");
        if (lv != nullptr && lv->kind == PickleValue::kInt) {
            return static_cast<int>(lv->i);
        }
    }
    if (rows_raw != nullptr) {
        for (const PicklePtr& row_val : *rows_raw) {
            const PickleList* row = row_val ? as_list_ptr(row_val.get()) : nullptr;
            if (row == nullptr || row->size() < 2 || !(*row)[1]) {
                continue;
            }
            if (const PickleList* path = as_list_ptr((*row)[1].get())) {
                return static_cast<int>(path->size());
            }
        }
    }
    return 0;
}

bool partition_root_to_flat(
    const PickleValue& root_val,
    std::vector<unsigned char>& blob,
    PartBuildStats& stats,
    std::string* err) {
    const PickleDict* root = root_val.as_dict();
    if (root == nullptr) {
        if (err) {
            *err = "partition_root not dict";
        }
        return false;
    }

    std::unordered_map<const PickleDict*, int> idmap;
    std::vector<PartNodeData> parts;
    std::vector<const PickleDict*> part_ptrs;

    auto extract_part = [&](const PickleDict* part) -> PartNodeData {
        PartNodeData node;
        node.one_index = sorted_hits(dict_child(part, "1Index"));
        const PickleValue* oip = dict_child(part, "oneIndex_partition");
        const int n_oip = oip_len(oip);
        node.oip.resize(static_cast<std::size_t>(n_oip));
        for (int i = 0; i < n_oip; ++i) {
            node.oip[static_cast<std::size_t>(i)] = sorted_hits(oip_part(oip, i));
        }
        const PickleValue* route = dict_child(part, "_route");
        if (const PickleDict* route_d = as_dict_ptr(route)) {
            std::vector<std::pair<int, const PickleValue*>> entries;
            entries.reserve(route_d->size());
            for (const auto& kv : route_d->entries) {
                entries.emplace_back(std::stoi(kv.first), kv.second ? kv.second.get() : nullptr);
            }
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            node.route.reserve(entries.size());
            for (const auto& ent : entries) {
                node.route.emplace_back(ent.first, std::vector<int>{});
            }
        }
        node.is_leaf = dict_child(part, "MGNTree") != nullptr;
        node.mgn_key = node.is_leaf ? dict_int(part, "MGNTree", -1) : -1;
        return node;
    };

    std::function<int(const PickleDict*)> visit;

    visit = [&](const PickleDict* part) -> int {
        const auto it = idmap.find(part);
        if (it != idmap.end()) {
            return it->second;
        }
        const int nid = static_cast<int>(parts.size());
        idmap[part] = nid;
        parts.push_back(extract_part(part));
        part_ptrs.push_back(part);

        const PickleValue* next = dict_child(part, "nextPartition");
        if (const PickleDict* next_d = as_dict_ptr(next)) {
            for (const auto& kv : next_d->entries) {
                if (const PickleDict* child = kv.second ? as_dict_ptr(kv.second.get()) : nullptr) {
                    if (dict_child(child, "MGNTree") == nullptr) {
                        visit(child);
                    }
                }
            }
        }
        return nid;
    };

    HpcFlatPart pf;
    pf.root_node = visit(root);

    for (std::size_t pi = 0; pi < parts.size(); ++pi) {
        const PickleDict* part = part_ptrs[pi];
        std::vector<std::pair<int, std::vector<int>>> route_local;
        const PickleValue* route = dict_child(part, "_route");
        if (const PickleDict* route_d = as_dict_ptr(route)) {
            std::vector<std::pair<int, const PickleValue*>> entries;
            entries.reserve(route_d->size());
            for (const auto& kv : route_d->entries) {
                entries.emplace_back(std::stoi(kv.first), kv.second ? kv.second.get() : nullptr);
            }
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            route_local.reserve(entries.size());
            for (const auto& ent : entries) {
                std::vector<int> child_ids;
                const PickleValue* children = ent.second;
                if (const PickleList* cl = as_list_ptr(children)) {
                    for (const PicklePtr& ch : *cl) {
                        if (const PickleDict* cd = ch ? as_dict_ptr(ch.get()) : nullptr) {
                            child_ids.push_back(visit(cd));
                        }
                    }
                } else if (const PickleDict* cd = as_dict_ptr(children)) {
                    child_ids.push_back(visit(cd));
                }
                route_local.emplace_back(ent.first, std::move(child_ids));
            }
        }
        parts[pi].route = std::move(route_local);
    }

    pf.one_off.reserve(parts.size());
    pf.one_len.reserve(parts.size());
    pf.oip_off.reserve(parts.size());
    pf.oip_num_parts.reserve(parts.size());
    pf.route_ent_off.reserve(parts.size());
    pf.route_ent_len.reserve(parts.size());
    pf.node_is_leaf.reserve(parts.size());
    pf.node_mgn_key.reserve(parts.size());

    for (const PartNodeData& part : parts) {
        pf.one_off.push_back(static_cast<int>(pf.one_index_pool.size()));
        pf.one_len.push_back(static_cast<int>(part.one_index.size()));
        pf.one_index_pool.insert(pf.one_index_pool.end(), part.one_index.begin(), part.one_index.end());

        pf.oip_off.push_back(static_cast<int>(pf.oip_pool.size()));
        pf.oip_num_parts.push_back(static_cast<int>(part.oip.size()));
        for (const auto& hits : part.oip) {
            pf.oip_pool.push_back(static_cast<int>(hits.size()));
            pf.oip_pool.insert(pf.oip_pool.end(), hits.begin(), hits.end());
        }

        pf.route_ent_off.push_back(static_cast<int>(pf.route_mask.size()));
        for (const auto& ent : part.route) {
            pf.route_mask.push_back(ent.first);
            pf.route_child_off.push_back(static_cast<int>(pf.route_child_ids.size()));
            pf.route_child_ids.insert(pf.route_child_ids.end(), ent.second.begin(), ent.second.end());
        }
        pf.route_ent_len.push_back(static_cast<int>(part.route.size()));

        pf.node_is_leaf.push_back(part.is_leaf ? 1 : 0);
        pf.node_mgn_key.push_back(part.mgn_key);
    }
    pf.route_child_off.push_back(static_cast<int>(pf.route_child_ids.size()));

    blob = partition_flat_encode(pf);
    stats.partition_nodes = static_cast<int>(parts.size());
    stats.partition_flat_bytes = static_cast<int>(blob.size());
    stats.route_entries = static_cast<int>(pf.route_mask.size());
    stats.index_pool_len = static_cast<int>(pf.one_index_pool.size());
    stats.leaf_nodes_expected = 0;
    for (const PartNodeData& part : parts) {
        if (part.is_leaf) {
            stats.leaf_nodes_expected += 1;
        }
    }
    stats.leaf_nodes = stats.leaf_nodes_expected;
    stats.leaf_match = true;
    return true;
}

int nary_node_bit(const PickleDict* node) {
    const PickleValue* val = dict_child(node, "value");
    if (val == nullptr) {
        return 0;
    }
    if (const PickleList* lst = as_list_ptr(val)) {
        if (!lst->empty() && (*lst)[0]) {
            const PickleValue& v0 = *(*lst)[0];
            if (v0.kind == PickleValue::kStr) {
                return v0.s.find('1') != std::string::npos ? 1 : 0;
            }
            if (v0.kind == PickleValue::kInt) {
                return static_cast<int>(v0.i);
            }
        }
        return 0;
    }
    if (val->kind == PickleValue::kStr) {
        return val->s.find('1') != std::string::npos ? 1 : 0;
    }
    if (val->kind == PickleValue::kInt) {
        return static_cast<int>(val->i);
    }
    return 0;
}

const PickleDict* nary_get_node(const PickleDict* nodes, int key) {
    if (nodes == nullptr) {
        return nullptr;
    }
    if (const PicklePtr* p = nodes->get(std::to_string(key))) {
        return p && *p ? as_dict_ptr(p->get()) : nullptr;
    }
    return nullptr;
}

void nary_walk(
    const PickleDict* nodes,
    int nkey,
    const std::vector<int>& path,
    std::vector<int>& cids,
    std::vector<int>& hits,
    std::vector<int>& path_lens,
    std::vector<int>& path_bits) {
    const PickleDict* node = nary_get_node(nodes, nkey);
    if (node == nullptr) {
        return;
    }
    const PickleValue* leaves = dict_child(node, "leaves");
    if (const PickleList* leaf_list = as_list_ptr(leaves)) {
        for (const PicklePtr& item : *leaf_list) {
            const PickleList* row = item ? as_list_ptr(item.get()) : nullptr;
            if (row == nullptr || row->size() < 2 || !(*row)[0]) {
                continue;
            }
            cids.push_back((*row)[0]->kind == PickleValue::kInt ? static_cast<int>((*row)[0]->i) : 0);
            if (row->size() >= 3 && (*row)[2] && (*row)[2]->kind == PickleValue::kInt) {
                hits.push_back(static_cast<int>((*row)[2]->i));
            } else if (row->size() >= 2 && (*row)[1] && (*row)[1]->kind == PickleValue::kInt) {
                hits.push_back(static_cast<int>((*row)[1]->i));
            } else {
                hits.push_back(0);
            }
            path_lens.push_back(static_cast<int>(path.size()));
            path_bits.insert(path_bits.end(), path.begin(), path.end());
        }
    }
    const PickleValue* children = dict_child(node, "children");
    if (const PickleList* cl = as_list_ptr(children)) {
        for (const PicklePtr& ck : *cl) {
            const int ckey = ck && ck->kind == PickleValue::kInt
                                 ? static_cast<int>(ck->i)
                                 : dict_int(ck ? as_dict_ptr(ck.get()) : nullptr, "key", 0);
            const PickleDict* cnode = nary_get_node(nodes, ckey);
            if (cnode == nullptr) {
                continue;
            }
            std::vector<int> next = path;
            next.push_back(nary_node_bit(cnode));
            nary_walk(nodes, ckey, next, cids, hits, path_lens, path_bits);
        }
    }
}

MgnCompactTree nary_pickle_to_compact(const PickleDict* tree_data) {
    const PickleDict* nodes = as_dict_ptr(dict_child(tree_data, "nodes"));
    const int root_key = dict_int(tree_data, "root", 1);
    std::vector<int> cids;
    std::vector<int> hits;
    std::vector<int> path_lens;
    std::vector<int> path_bits;
    nary_walk(nodes, root_key, {}, cids, hits, path_lens, path_bits);
    return mgn_flat_to_compact(
        cids.data(),
        hits.data(),
        path_lens.data(),
        path_bits.data(),
        static_cast<int>(cids.size()));
}

void fill_matrix_rows(
    std::vector<uint8_t>& mat,
    std::vector<int32_t>& cids,
    std::vector<uint8_t>& spop,
    int& pos,
    const std::vector<FlatRow>& rows,
    int l_val) {
    for (const FlatRow& row : rows) {
        cids[static_cast<std::size_t>(pos)] = static_cast<int32_t>(row.cid);
        spop[static_cast<std::size_t>(pos)] = static_cast<uint8_t>(popcount_path(row, l_val));
        for (int j = 0; j < l_val; ++j) {
            const int bit = path_bit_msb(row, l_val, j);
            mat[static_cast<std::size_t>(pos) * static_cast<std::size_t>(l_val) + static_cast<std::size_t>(j)] =
                static_cast<uint8_t>(bit);
        }
        pos += 1;
    }
}

bool build_mgn_from_pickle(
    const PickleDict& mgn,
    HpcxWriteParts& parts,
    std::string* err) {
    std::vector<FlatItem> packed_items;
    std::map<int, std::vector<std::pair<int, std::vector<FlatRow>>>> matrix_by_l;
    std::vector<std::pair<int, const PickleDict*>> nary_items;

    for (const auto& kv : mgn.entries) {
        const int key = std::stoi(kv.first);
        const PickleDict* td = kv.second ? as_dict_ptr(kv.second.get()) : nullptr;
        if (td == nullptr) {
            continue;
        }
        const PickleValue* flat_flag = dict_child(td, "flat");
        if (flat_flag != nullptr && flat_flag->kind == PickleValue::kBool && flat_flag->b) {
            FlatItem item;
            item.key = key;
            item.L = infer_L(as_list_ptr(dict_child(td, "rows")), td);
            if (const PickleList* rows = as_list_ptr(dict_child(td, "rows"))) {
                for (const PicklePtr& rv : *rows) {
                    if (rv) {
                        if (const PickleList* row = as_list_ptr(rv.get())) {
                            item.rows.push_back(normalize_row(*row));
                        }
                    }
                }
            }
            if (item.rows.empty()) {
                continue;
            }
            if (item.L <= 64) {
                packed_items.push_back(std::move(item));
            } else {
                matrix_by_l[item.L].push_back({key, item.rows});
            }
            continue;
        }
        if (dict_child(td, "root") != nullptr && dict_child(td, "nodes") != nullptr) {
            nary_items.emplace_back(key, td);
        }
    }

    if (!packed_items.empty()) {
        int total = 0;
        for (const FlatItem& item : packed_items) {
            total += static_cast<int>(item.rows.size());
        }
        OwnedPackedPool pool;
        pool.cids.assign(static_cast<std::size_t>(total), 0);
        pool.bits.assign(static_cast<std::size_t>(total), 0);
        pool.spop.assign(static_cast<std::size_t>(total), 0);
        pool.leaf_off.assign(packed_items.size(), 0);
        pool.leaf_n.assign(packed_items.size(), 0);
        int pos = 0;
        for (std::size_t li = 0; li < packed_items.size(); ++li) {
            const FlatItem& item = packed_items[li];
            pool.leaf_off[li] = pos;
            pool.leaf_n[li] = static_cast<int32_t>(item.rows.size());
            for (const FlatRow& row : item.rows) {
                pool.cids[static_cast<std::size_t>(pos)] = static_cast<int32_t>(row.cid);
                pool.bits[static_cast<std::size_t>(pos)] = path_to_bits64(row, item.L);
                pool.spop[static_cast<std::size_t>(pos)] = static_cast<uint8_t>(popcount_path(row, item.L));
                pos += 1;
            }
        }
        const int pool_id = static_cast<int>(parts.packed_pools.size());
        parts.packed_pools.push_back(std::move(pool));

        for (std::size_t li = 0; li < packed_items.size(); ++li) {
            const FlatItem& item = packed_items[li];
            HpcxMgnEntry ent;
            ent.kind = HpcxMgnEntry::PACKED;
            ent.pool_id = pool_id;
            ent.leaf_idx = static_cast<int>(li);
            ent.L = item.L;
            ent.n = static_cast<int>(item.rows.size());
            parts.tree_mgn[item.key] = ent;
        }
    }

    for (const auto& l_kv : matrix_by_l) {
        const int l_val = l_kv.first;
        const auto& items = l_kv.second;
        int total = 0;
        for (const auto& it : items) {
            total += static_cast<int>(it.second.size());
        }
        OwnedMatrixPool pool;
        pool.L = static_cast<int16_t>(l_val);
        pool.cids.assign(static_cast<std::size_t>(total), 0);
        pool.mat.assign(static_cast<std::size_t>(total) * static_cast<std::size_t>(l_val), 0);
        pool.spop.assign(static_cast<std::size_t>(total), 0);
        pool.leaf_off.assign(items.size(), 0);
        pool.leaf_n.assign(items.size(), 0);

        pool.cids.assign(static_cast<std::size_t>(total), 0);
        int pos = 0;
        for (std::size_t li = 0; li < items.size(); ++li) {
            pool.leaf_off[li] = pos;
            pool.leaf_n[li] = static_cast<int32_t>(items[li].second.size());
            fill_matrix_rows(pool.mat, pool.cids, pool.spop, pos, items[li].second, l_val);
        }

        const int pool_id = static_cast<int>(parts.matrix_pools.size());
        parts.matrix_pools.push_back(std::move(pool));
        for (std::size_t li = 0; li < items.size(); ++li) {
            HpcxMgnEntry ent;
            ent.kind = HpcxMgnEntry::MATRIX;
            ent.pool_id = pool_id;
            ent.leaf_idx = static_cast<int>(li);
            ent.L = l_val;
            ent.n = static_cast<int>(items[li].second.size());
            parts.tree_mgn[items[li].first] = ent;
        }
    }

    for (const auto& nv : nary_items) {
        HpcxMgnEntry ent;
        ent.kind = HpcxMgnEntry::COMPACT;
        ent.compact = nary_pickle_to_compact(nv.second);
        parts.tree_mgn[nv.first] = ent;
    }

    if (parts.tree_mgn.empty()) {
        if (err) {
            *err = "no MGN entries built";
        }
        return false;
    }
    return true;
}

std::vector<unsigned char> make_meta_json(
    int band,
    const PartBuildStats& ps,
    int n_views,
    int n_pools,
    int n_compact) {
    std::ostringstream os;
    os << "{"
       << "\"format\":\"HPCX\","
       << "\"version\":" << HPCX_VERSION << ","
       << "\"band\":" << band << ","
       << "\"n_views\":" << n_views << ","
       << "\"n_pools\":" << n_pools << ","
       << "\"n_compact\":" << n_compact << ","
       << "\"partition_nodes\":" << ps.partition_nodes << ","
       << "\"partition_flat_bytes\":" << ps.partition_flat_bytes << ","
       << "\"route_entries\":" << ps.route_entries << ","
       << "\"index_pool_len\":" << ps.index_pool_len << ","
       << "\"leaf_nodes\":" << ps.leaf_nodes << ","
       << "\"leaf_nodes_expected\":" << ps.leaf_nodes_expected << ","
       << "\"leaf_match\":" << (ps.leaf_match ? "true" : "false") << ","
       << "\"build_from\":\"cpp_zip\""
       << "}";
    const std::string text = os.str();
    return std::vector<unsigned char>(text.begin(), text.end());
}

}  // namespace

bool hpcx_build_from_zip(
    const std::string& zip_path,
    const std::string& out_hpcx_path,
    int band,
    HpcxWriteStats* stats,
    std::string* err) {
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<unsigned char> raw;
    if (!hpc_zip_read_stored_member(zip_path, raw, err)) {
        return false;
    }

    PickleValue root;
    if (!pickle_load_data_only(raw.data(), raw.size(), root, err)) {
        return false;
    }
    const PickleDict* top = root.as_dict();
    if (top == nullptr) {
        if (err) {
            *err = "zip pickle root not dict";
        }
        return false;
    }

    const PickleValue* pr = dict_child(top, "partition_root");
    const PickleValue* mgn_v = dict_child(top, "MGN");
    const PickleDict* mgn = as_dict_ptr(mgn_v);
    if (pr == nullptr || mgn == nullptr) {
        if (err) {
            *err = "missing partition_root or MGN";
        }
        return false;
    }

    HpcxWriteParts parts;
    parts.band = static_cast<uint32_t>(band);

    PartBuildStats ps;
    if (!partition_root_to_flat(*pr, parts.partition, ps, err)) {
        return false;
    }
    if (!build_mgn_from_pickle(*mgn, parts, err)) {
        return false;
    }

    int n_compact = 0;
    for (const auto& kv : parts.tree_mgn) {
        if (kv.second.kind == HpcxMgnEntry::COMPACT) {
            n_compact += 1;
        }
    }
    parts.meta = make_meta_json(
        band,
        ps,
        static_cast<int>(parts.tree_mgn.size()),
        static_cast<int>(parts.packed_pools.size() + parts.matrix_pools.size()),
        n_compact);

    HpcxWriteStats wstats;
    if (!hpcx_write_parts(parts, out_hpcx_path, &wstats, err)) {
        return false;
    }

    if (stats != nullptr) {
        *stats = wstats;
        stats->write_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}
