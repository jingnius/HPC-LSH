#include "hpc_hpcx.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>


#include "hpc_band_format.h"
#include "hpc_npz.h"
#include "hpc_rss.h"

FlatPackedPool OwnedPackedPool::view() const {
    FlatPackedPool v;
    v.cids = cids.data();
    v.bits = bits.data();
    v.spop = spop.empty() ? nullptr : spop.data();
    v.leaf_off = leaf_off.data();
    v.leaf_n = leaf_n.data();
    v.n_rows = static_cast<int64_t>(cids.size());
    v.n_leaves = static_cast<int32_t>(leaf_n.size());
    return v;
}

FlatWidePool OwnedWidePool::view() const {
    FlatWidePool v;
    v.cids = cids.data();
    v.bits = bits.data();
    v.leaf_off = leaf_off.data();
    v.leaf_n = leaf_n.data();
    v.n_rows = static_cast<int64_t>(cids.size());
    v.n_leaves = static_cast<int32_t>(leaf_n.size());
    v.n_words = n_words;
    return v;
}

FlatMatrixPool OwnedMatrixPool::view() const {
    FlatMatrixPool v;
    v.cids = cids.data();
    v.mat = mat.data();
    v.spop = spop.empty() ? nullptr : spop.data();
    v.leaf_off = leaf_off.data();
    v.leaf_n = leaf_n.data();
    v.n_rows = static_cast<int64_t>(cids.size());
    v.n_leaves = static_cast<int32_t>(leaf_n.size());
    v.L = L;
    return v;
}

static bool load_packed_npz(const std::unordered_map<std::string, NpzArray>& arrs, OwnedPackedPool& out) {
    std::size_t n = 0;
    const int64_t* cids64 = npz_as<int64_t>(arrs, "cids.npy", n);
    if (cids64 == nullptr || n == 0) {
        return false;
    }
    out.cids.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.cids[i] = static_cast<int32_t>(cids64[i]);
    }
    const uint64_t* bits = npz_as<uint64_t>(arrs, "bits.npy", n);
    if (bits == nullptr) {
        return false;
    }
    out.bits.assign(bits, bits + n);
    const uint8_t* spop = npz_as<uint8_t>(arrs, "spop.npy", n);
    if (spop != nullptr) {
        out.spop.assign(spop, spop + n);
    }
    std::size_t nl = 0;
    const int64_t* leaf_off = npz_as<int64_t>(arrs, "leaf_off.npy", nl);
    const int32_t* leaf_n = npz_as<int32_t>(arrs, "leaf_n.npy", nl);
    if (leaf_off == nullptr || leaf_n == nullptr) {
        return false;
    }
    out.leaf_off.assign(leaf_off, leaf_off + nl);
    out.leaf_n.assign(leaf_n, leaf_n + nl);
    return true;
}

static bool load_wide_npz(const std::unordered_map<std::string, NpzArray>& arrs, OwnedWidePool& out) {
    std::size_t n = 0;
    const int64_t* cids64 = npz_as<int64_t>(arrs, "cids.npy", n);
    if (cids64 == nullptr || n == 0) {
        return false;
    }
    out.cids.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.cids[i] = static_cast<int32_t>(cids64[i]);
    }
    std::size_t n_bits = 0;
    const uint64_t* bits = npz_as<uint64_t>(arrs, "bits.npy", n_bits);
    if (bits == nullptr || n_bits == 0) {
        return false;
    }
    out.bits.assign(bits, bits + n_bits);
    std::size_t n_w = 0;
    const int32_t* words = npz_as<int32_t>(arrs, "n_words.npy", n_w);
    if (words == nullptr || n_w == 0 || words[0] <= 0) {
        return false;
    }
    out.n_words = words[0];
    if (out.bits.size() != out.cids.size() * static_cast<std::size_t>(out.n_words)) {
        return false;
    }
    std::size_t nl = 0;
    const int64_t* leaf_off = npz_as<int64_t>(arrs, "leaf_off.npy", nl);
    const int32_t* leaf_n = npz_as<int32_t>(arrs, "leaf_n.npy", nl);
    if (leaf_off == nullptr || leaf_n == nullptr) {
        return false;
    }
    out.leaf_off.assign(leaf_off, leaf_off + nl);
    out.leaf_n.assign(leaf_n, leaf_n + nl);
    return true;
}

static bool load_matrix_npz(const std::unordered_map<std::string, NpzArray>& arrs, OwnedMatrixPool& out) {
    std::size_t n = 0;
    const int64_t* cids64 = npz_as<int64_t>(arrs, "cids.npy", n);
    if (cids64 == nullptr || n == 0) {
        return false;
    }
    out.cids.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.cids[i] = static_cast<int32_t>(cids64[i]);
    }
    const uint8_t* mat = npz_as<uint8_t>(arrs, "mat.npy", n);
    if (mat == nullptr) {
        return false;
    }
    out.mat.assign(mat, mat + n);
    const uint8_t* spop = npz_as<uint8_t>(arrs, "spop.npy", n);
    if (spop != nullptr) {
        std::size_t ns = 0;
        npz_as<uint8_t>(arrs, "spop.npy", ns);
        out.spop.assign(spop, spop + ns);
    }
    std::size_t nl = 0;
    const int64_t* leaf_off = npz_as<int64_t>(arrs, "leaf_off.npy", nl);
    const int32_t* leaf_n = npz_as<int32_t>(arrs, "leaf_n.npy", nl);
    if (leaf_off == nullptr || leaf_n == nullptr) {
        return false;
    }
    out.leaf_off.assign(leaf_off, leaf_off + nl);
    out.leaf_n.assign(leaf_n, leaf_n + nl);
    std::size_t l1 = 0;
    const int16_t* L16 = npz_as<int16_t>(arrs, "L.npy", l1);
    if (L16 != nullptr && l1 > 0) {
        out.L = L16[0];
    } else {
        const int32_t* L32 = npz_as<int32_t>(arrs, "L.npy", l1);
        if (L32 != nullptr && l1 > 0) {
            out.L = static_cast<int16_t>(L32[0]);
        } else {
            const int64_t* L64 = npz_as<int64_t>(arrs, "L.npy", l1);
            if (L64 != nullptr && l1 > 0) {
                out.L = static_cast<int16_t>(L64[0]);
            }
        }
    }
    if (out.L <= 0 && !out.cids.empty() && !out.leaf_n.empty()) {
        out.L = static_cast<int16_t>(out.mat.size() / out.cids.size());
    }
    return true;
}

static bool parse_pool_chunk(
    const unsigned char* chunk,
    std::size_t chunk_len,
    HpcxIndex& idx,
    std::string& err) {
    if (chunk_len < 16) {
        err = "pool chunk too small";
        return false;
    }
    const std::string kind(reinterpret_cast<const char*>(chunk), strnlen(reinterpret_cast<const char*>(chunk), 16));
    std::unordered_map<std::string, NpzArray> arrs;
    if (!npz_load(chunk + 16, chunk_len - 16, arrs)) {
        err = "npz_load failed for kind " + kind;
        return false;
    }
    if (kind == "packed") {
        OwnedPackedPool pool;
        if (!load_packed_npz(arrs, pool)) {
            err = "packed pool parse failed";
            return false;
        }
        idx.packed_pools.push_back(std::move(pool));
    } else if (kind == "wide") {
        OwnedWidePool pool;
        if (!load_wide_npz(arrs, pool)) {
            err = "wide pool parse failed";
            return false;
        }
        idx.wide_pools.push_back(std::move(pool));
    } else if (kind == "matrix") {
        OwnedMatrixPool pool;
        if (!load_matrix_npz(arrs, pool)) {
            err = "matrix pool parse failed";
            return false;
        }
        idx.matrix_pools.push_back(std::move(pool));
    } else {
        err = "unknown pool kind: " + kind;
        return false;
    }
    return true;
}

std::unique_ptr<HpcxIndex> hpcx_load(const std::string& path, std::string* err) {
    auto idx = std::make_unique<HpcxIndex>();
    idx->path = path;
    idx->fd = ::open(path.c_str(), O_RDONLY);
    if (idx->fd < 0) {
        if (err) {
            *err = "open failed";
        }
        return nullptr;
    }
    idx->mmap_len = static_cast<std::size_t>(::lseek(idx->fd, 0, SEEK_END));
    idx->mmap_base = static_cast<unsigned char*>(
        ::mmap(nullptr, idx->mmap_len, PROT_READ, MAP_PRIVATE, idx->fd, 0));
    if (idx->mmap_base == MAP_FAILED) {
        if (err) {
            *err = "mmap failed";
        }
        ::close(idx->fd);
        return nullptr;
    }

    if (idx->mmap_len < sizeof(HpcxHeader)) {
        if (err) {
            *err = "file too small";
        }
        return nullptr;
    }
    const auto* hdr = reinterpret_cast<const HpcxHeader*>(idx->mmap_base);
    if (hdr->magic != HPCX_MAGIC) {
        if (err) {
            *err = "bad magic";
        }
        return nullptr;
    }
    idx->band = hdr->band;

    uint64_t sec_off[HPCX_SEC_COUNT] = {};
    uint64_t sec_len[HPCX_SEC_COUNT] = {};
    for (int sec = 0; sec < HPCX_SEC_COUNT; ++sec) {
        const unsigned char* sp = idx->mmap_base + 16 + sec * 16;
        std::memcpy(&sec_off[sec], sp, 8);
        std::memcpy(&sec_len[sec], sp + 8, 8);
    }

    const unsigned char* part = nullptr;
    std::size_t part_len = 0;
    const unsigned char* pool_sec = nullptr;
    std::size_t pool_len = 0;
    const unsigned char* views = nullptr;
    std::size_t views_len = 0;
    const unsigned char* cidx = nullptr;
    std::size_t cidx_len = 0;
    const unsigned char* cblobs = nullptr;
    std::size_t cblobs_len = 0;

    if (sec_len[HPCX_SEC_PARTITION_FLAT]) {
        part = idx->mmap_base + sec_off[HPCX_SEC_PARTITION_FLAT];
        part_len = static_cast<std::size_t>(sec_len[HPCX_SEC_PARTITION_FLAT]);
    }
    if (sec_len[HPCX_SEC_MGN_PACKED]) {
        pool_sec = idx->mmap_base + sec_off[HPCX_SEC_MGN_PACKED];
        pool_len = static_cast<std::size_t>(sec_len[HPCX_SEC_MGN_PACKED]);
    } else if (sec_len[HPCX_SEC_MGN_MATRIX]) {
        pool_sec = idx->mmap_base + sec_off[HPCX_SEC_MGN_MATRIX];
        pool_len = static_cast<std::size_t>(sec_len[HPCX_SEC_MGN_MATRIX]);
    }
    if (sec_len[HPCX_SEC_MGN_VIEWS]) {
        views = idx->mmap_base + sec_off[HPCX_SEC_MGN_VIEWS];
        views_len = static_cast<std::size_t>(sec_len[HPCX_SEC_MGN_VIEWS]);
    }
    if (sec_len[HPCX_SEC_MGN_COMPACT_INDEX]) {
        cidx = idx->mmap_base + sec_off[HPCX_SEC_MGN_COMPACT_INDEX];
        cidx_len = static_cast<std::size_t>(sec_len[HPCX_SEC_MGN_COMPACT_INDEX]);
    }
    if (sec_len[HPCX_SEC_MGN_COMPACT_BLOBS]) {
        cblobs = idx->mmap_base + sec_off[HPCX_SEC_MGN_COMPACT_BLOBS];
        cblobs_len = static_cast<std::size_t>(sec_len[HPCX_SEC_MGN_COMPACT_BLOBS]);
    }

    if (part == nullptr || !partition_view_attach(part, part_len, idx->partition)) {
        if (err) {
            *err = "partition attach failed";
        }
        return nullptr;
    }

    if (pool_sec != nullptr && pool_len > 0) {
        std::size_t pos = 0;
        while (pos + 8 <= pool_len) {
            uint64_t chunk_len = 0;
            std::memcpy(&chunk_len, pool_sec + pos, 8);
            pos += 8;
            if (chunk_len == 0 || pos + chunk_len > pool_len) {
                break;
            }
            std::string perr;
            if (!parse_pool_chunk(pool_sec + pos, static_cast<std::size_t>(chunk_len), *idx, perr)) {
                if (err) {
                    *err = perr;
                }
                return nullptr;
            }
            pos += static_cast<std::size_t>(chunk_len);
        }
        // Drop compressed pool pages from RSS; decompressed pools live on heap.
        ::madvise(const_cast<unsigned char*>(pool_sec), pool_len, MADV_DONTNEED);
    }

    if (views != nullptr && views_len >= 40) {
        const std::size_t nrows = views_len / (5 * sizeof(int64_t));
        const auto* rows = reinterpret_cast<const int64_t*>(views);
        for (std::size_t i = 0; i < nrows; ++i) {
            const int64_t* row = rows + i * 5;
            const int node_key = static_cast<int>(row[0]);
            const int pool_id = static_cast<int>(row[1]);
            HpcxMgnEntry ent;
            ent.leaf_idx = static_cast<int>(row[2]);
            ent.L = static_cast<int>(row[3]);
            ent.n = static_cast<int>(row[4]);
            ent.pool_id = pool_id;
            const int n_packed = static_cast<int>(idx->packed_pools.size());
            const int n_wide = static_cast<int>(idx->wide_pools.size());
            if (pool_id < n_packed) {
                ent.kind = HpcxMgnEntry::PACKED;
            } else if (pool_id < n_packed + n_wide) {
                ent.kind = HpcxMgnEntry::WIDE;
                ent.pool_id = pool_id - n_packed;
            } else {
                ent.kind = HpcxMgnEntry::MATRIX;
                ent.pool_id = pool_id - n_packed - n_wide;
            }
            idx->tree_mgn[node_key] = ent;
        }
    }

    if (cidx != nullptr && cidx_len >= 24 && cblobs != nullptr) {
        const std::size_t nrows = cidx_len / (3 * sizeof(int64_t));
        const auto* rows = reinterpret_cast<const int64_t*>(cidx);
        for (std::size_t i = 0; i < nrows; ++i) {
            const int64_t* row = rows + i * 3;
            const int node_key = static_cast<int>(row[0]);
            const std::size_t off = static_cast<std::size_t>(row[1]);
            const std::size_t ln = static_cast<std::size_t>(row[2]);
            if (off + ln > cblobs_len) {
                if (err) {
                    *err = "compact blob out of range";
                }
                return nullptr;
            }
            HpcxMgnEntry ent;
            ent.kind = HpcxMgnEntry::COMPACT;
            ent.compact = mgn_compact_from_bytes(cblobs + off, ln);
            ent.compact_index = compact_build_index(ent.compact);
            idx->tree_mgn[node_key] = std::move(ent);
        }
    }

    return idx;
}

template <typename T>
static void touch_vec(const std::vector<T>& v) {
    if (v.empty()) {
        return;
    }
    touch_mapped_pages(reinterpret_cast<const unsigned char*>(v.data()), v.size() * sizeof(T));
}

void hpcx_prefault_resident(const HpcxIndex& idx) {
    if (idx.mmap_base != nullptr && idx.mmap_base != MAP_FAILED && idx.mmap_len > 0) {
        touch_mapped_pages(idx.mmap_base, idx.mmap_len);
    }
    for (const OwnedPackedPool& pool : idx.packed_pools) {
        touch_vec(pool.cids);
        touch_vec(pool.bits);
        touch_vec(pool.spop);
        touch_vec(pool.leaf_off);
        touch_vec(pool.leaf_n);
    }
    for (const OwnedWidePool& pool : idx.wide_pools) {
        touch_vec(pool.cids);
        touch_vec(pool.bits);
        touch_vec(pool.leaf_off);
        touch_vec(pool.leaf_n);
    }
    for (const OwnedMatrixPool& pool : idx.matrix_pools) {
        touch_vec(pool.cids);
        touch_vec(pool.mat);
        touch_vec(pool.spop);
        touch_vec(pool.leaf_off);
        touch_vec(pool.leaf_n);
    }
    for (const auto& kv : idx.tree_mgn) {
        const MgnCompactTree& tree = kv.second.compact;
        touch_vec(tree.node_keys);
        touch_vec(tree.parent_keys);
        touch_vec(tree.node_bits);
        touch_vec(tree.child_off);
        touch_vec(tree.child_keys);
        touch_vec(tree.leaf_node_keys);
        touch_vec(tree.leaf_cids);
        touch_vec(tree.leaf_hits);
        for (const auto& leaves : kv.second.compact_index.leaves_by_key) {
            touch_vec(leaves.second);
        }
    }
}

bool hpcx_get_section(
    const HpcxIndex& idx,
    int sec,
    const unsigned char** data,
    std::size_t* len) {
    if (data == nullptr || len == nullptr || idx.mmap_base == nullptr || sec < 0 ||
        sec >= HPCX_SEC_COUNT) {
        return false;
    }
    if (idx.mmap_len < 16 + static_cast<std::size_t>(sec + 1) * 16) {
        return false;
    }
    const unsigned char* sp = idx.mmap_base + 16 + sec * 16;
    uint64_t off = 0;
    uint64_t slen = 0;
    std::memcpy(&off, sp, 8);
    std::memcpy(&slen, sp + 8, 8);
    if (slen == 0) {
        *data = nullptr;
        *len = 0;
        return true;
    }
    if (off + slen > idx.mmap_len) {
        return false;
    }
    *data = idx.mmap_base + off;
    *len = static_cast<std::size_t>(slen);
    return true;
}

void hpcx_search_encoded(
    const HpcxIndex& idx,
    const int* encoded_query,
    int query_len,
    int min_hits,
    std::vector<int32_t>& out_cids) {
    out_cids.clear();

    const bool profile = std::getenv("HPCX_PROFILE_SEARCH") != nullptr;
    int prof_leaf_tasks = 0;
    std::vector<int32_t> out_raw;

    const HpcFlatPartView& flat = idx.partition;

    struct TreeTask {
        int tree_key = 0;
        int node_id = 0;
        std::vector<int> seq;     // sig at leaf entry (compact zip_0_path_)
        std::vector<int> seq_mgn; // filtered sig at leaf (flat/matrix q)
        int q_hits = 0;
        uint64_t q_cached = 0;
    };

    const auto one_index_for_node = [&](int node, int& out_len) -> const int* {
        out_len = 0;
        if (node < 0 || node >= flat.n_nodes) {
            return nullptr;
        }
        out_len = flat.one_len[node];
        if (out_len <= 0) {
            return flat.one_index_pool;
        }
        return flat.one_index_pool + flat.one_off[node];
    };

    std::deque<std::pair<int, std::vector<int>>> quene;
    std::vector<TreeTask> quene_tree;
    quene_tree.reserve(4096);

    const int root_id = flat.root_node;
    std::vector<int> sig_root = partition_view_filter_sig(flat, root_id, encoded_query, query_len);
    if (min_hits > 0 && static_cast<int>(sig_root.size()) < min_hits) {
        return;
    }
    const auto root_children = partition_view_route_children(flat, root_id, sig_root);
    if (partition_view_is_leaf(flat, root_id)) {
        quene.emplace_back(root_id, std::move(sig_root));
    } else if (!root_children.empty()) {
        for (std::size_t i = 0; i + 1 < root_children.size(); ++i) {
            quene.emplace_back(root_children[i], sig_root);
        }
        quene.emplace_back(root_children.back(), std::move(sig_root));
    }

    const auto loop_t0 = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    while (!quene.empty()) {
        const auto bfs_t0 = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto [node_id, seq] = quene.front();
        quene.pop_front();
        if (partition_view_is_leaf(flat, node_id)) {
            ++prof_leaf_tasks;
            int one_len = 0;
            const int* one_ptr = flat.one_index_pool;
            if (node_id >= 0 && node_id < flat.n_nodes) {
                one_len = flat.one_len[node_id];
                if (one_len > 0) {
                    one_ptr = flat.one_index_pool + flat.one_off[node_id];
                }
            }
            const int mgn_key = partition_view_mgn_key(flat, node_id);
            std::vector<int> seq_mgn = partition_view_filter_sig(flat, node_id, seq.data(), static_cast<int>(seq.size()));
            const ZipPathQ qp = zip_0_path_q(
                seq_mgn.data(),
                static_cast<int>(seq_mgn.size()),
                one_ptr,
                one_len);
            if (min_hits > 0 && qp.popcount < min_hits) {
                continue;
            }
            const int q_hits = qp.popcount;
            const uint64_t q_cached = qp.q;
            TreeTask task;
            task.tree_key = mgn_key;
            task.node_id = node_id;
            task.seq = std::move(seq);
            task.seq_mgn = std::move(seq_mgn);
            task.q_hits = q_hits;
            task.q_cached = q_cached;
            quene_tree.push_back(std::move(task));
            continue;
        }
        std::vector<int> sig_f = partition_view_filter_sig(flat, node_id, seq.data(), static_cast<int>(seq.size()));
        if (min_hits > 0 && static_cast<int>(sig_f.size()) < min_hits) {
            continue;
        }
        const auto kids = partition_view_route_children(flat, node_id, sig_f);
        if (kids.empty()) {
            continue;
        }
        for (std::size_t i = 0; i + 1 < kids.size(); ++i) {
            quene.emplace_back(kids[i], sig_f);
        }
        quene.emplace_back(kids.back(), std::move(sig_f));
    }

    std::vector<int32_t> leaves_tmp;
    leaves_tmp.reserve(256);
    for (const auto& task : quene_tree) {
        const auto it = idx.tree_mgn.find(task.tree_key);
        if (it == idx.tree_mgn.end()) {
            continue;
        }
        const HpcxMgnEntry& ent = it->second;
        leaves_tmp.clear();
        if (ent.kind == HpcxMgnEntry::PACKED) {
            if (ent.pool_id < 0 || ent.pool_id >= static_cast<int>(idx.packed_pools.size())) {
                continue;
            }
            const FlatPackedPool pool = idx.packed_pools[static_cast<std::size_t>(ent.pool_id)].view();
            flat_packed_find_leaves_q(
                pool,
                ent.leaf_idx,
                task.q_cached,
                task.q_hits,
                min_hits,
                leaves_tmp);
        } else if (ent.kind == HpcxMgnEntry::WIDE) {
            if (ent.pool_id < 0 || ent.pool_id >= static_cast<int>(idx.wide_pools.size())) {
                continue;
            }
            const FlatWidePool pool = idx.wide_pools[static_cast<std::size_t>(ent.pool_id)].view();
            int one_len = 0;
            const int* one_ptr = one_index_for_node(task.node_id, one_len);
            std::vector<uint64_t> q_words;
            int qpc = 0;
            zip_0_path_words(
                task.seq_mgn.data(),
                static_cast<int>(task.seq_mgn.size()),
                one_ptr,
                one_len > 0 ? one_len : ent.L,
                q_words,
                qpc);
            flat_wide_find_leaves_q(
                pool,
                ent.leaf_idx,
                q_words.data(),
                static_cast<int>(q_words.size()),
                qpc,
                min_hits,
                leaves_tmp);
        } else if (ent.kind == HpcxMgnEntry::MATRIX) {
            if (ent.pool_id < 0 || ent.pool_id >= static_cast<int>(idx.matrix_pools.size())) {
                continue;
            }
            const FlatMatrixPool pool = idx.matrix_pools[static_cast<std::size_t>(ent.pool_id)].view();
            int one_len = 0;
            const int* one_ptr = one_index_for_node(task.node_id, one_len);
            flat_matrix_find_leaves_q(
                pool,
                ent.leaf_idx,
                task.seq_mgn.data(),
                static_cast<int>(task.seq_mgn.size()),
                one_ptr,
                one_len > 0 ? one_len : ent.L,
                min_hits,
                leaves_tmp);
        } else if (ent.compact.position_trie) {
            global_ohs_find_leaves(ent.compact, ent.compact_index, task.seq, leaves_tmp);
        } else {
            int one_len = 0;
            const int* one_ptr = one_index_for_node(task.node_id, one_len);
            std::vector<int> value_mgn;
            value_mgn.reserve(static_cast<std::size_t>(one_len));
            zip_0_path_list(
                task.seq.data(),
                static_cast<int>(task.seq.size()),
                one_ptr,
                one_len,
                value_mgn);
            compact_find_leaves(ent.compact, ent.compact_index, value_mgn, leaves_tmp);
        }
        for (int32_t cid : leaves_tmp) {
            out_raw.push_back(cid);
        }
    }

    if (profile) {
        std::fprintf(stderr, "[hpcx_profile] leaf_tasks=%d quene_tree=%zu\n", prof_leaf_tasks, quene_tree.size());
        std::fprintf(stderr, "[hpcx_profile] out_raw_before_dedup=%zu\n", out_raw.size());
    }
    const auto dedup_t0 = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::sort(out_raw.begin(), out_raw.end());
    out_raw.erase(std::unique(out_raw.begin(), out_raw.end()), out_raw.end());
    if (profile) {
        const double dedup_s = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - dedup_t0)
                                   .count();
        std::fprintf(
            stderr,
            "[hpcx_profile] out_raw_after_dedup=%zu dedup_s=%.3f\n",
            out_raw.size(),
            dedup_s);
    }
    out_cids.swap(out_raw);
}
