#include "hpcx_write.h"

#include "hpc_band_format.h"
#include "hpc_hpcx.h"
#include "hpc_mgn_compact.h"
#include "hpc_npz.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

void append_u64(std::vector<unsigned char>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
    }
}

NpzArray npz_from_bytes(const std::string& descr, const std::vector<std::size_t>& shape, const void* data, std::size_t nbytes) {
    NpzArray arr;
    arr.descr = descr;
    arr.shape = shape;
    arr.data.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + nbytes);
    return arr;
}

bool append_pool_chunk(
    const char* kind,
    const std::unordered_map<std::string, NpzArray>& arrs,
    std::vector<unsigned char>& pool_blob) {
    std::vector<unsigned char> npz;
    if (!npz_save_compressed(arrs, npz)) {
        return false;
    }
    std::vector<unsigned char> chunk(16, '\0');
    const std::size_t kind_len = std::strlen(kind);
    if (kind_len >= 16) {
        return false;
    }
    std::memcpy(chunk.data(), kind, kind_len);
    chunk.insert(chunk.end(), npz.begin(), npz.end());
    append_u64(pool_blob, static_cast<uint64_t>(chunk.size()));
    pool_blob.insert(pool_blob.end(), chunk.begin(), chunk.end());
    return true;
}

bool packed_pool_to_chunk(const OwnedPackedPool& pool, std::vector<unsigned char>& pool_blob) {
    std::vector<int64_t> cids64(pool.cids.size());
    for (std::size_t i = 0; i < pool.cids.size(); ++i) {
        cids64[i] = pool.cids[i];
    }
    std::unordered_map<std::string, NpzArray> arrs;
    arrs["cids.npy"] = npz_from_bytes("<i8", {pool.cids.size()}, cids64.data(), cids64.size() * sizeof(int64_t));
    arrs["bits.npy"] = npz_from_bytes("<u8", {pool.bits.size()}, pool.bits.data(), pool.bits.size() * sizeof(uint64_t));
    if (!pool.spop.empty()) {
        arrs["spop.npy"] = npz_from_bytes("|u1", {pool.spop.size()}, pool.spop.data(), pool.spop.size());
    }
    arrs["leaf_off.npy"] =
        npz_from_bytes("<i8", {pool.leaf_off.size()}, pool.leaf_off.data(), pool.leaf_off.size() * sizeof(int64_t));
    arrs["leaf_n.npy"] =
        npz_from_bytes("<i4", {pool.leaf_n.size()}, pool.leaf_n.data(), pool.leaf_n.size() * sizeof(int32_t));
    return append_pool_chunk("packed", arrs, pool_blob);
}

bool wide_pool_to_chunk(const OwnedWidePool& pool, std::vector<unsigned char>& pool_blob) {
    std::vector<int64_t> cids64(pool.cids.size());
    for (std::size_t i = 0; i < pool.cids.size(); ++i) {
        cids64[i] = pool.cids[i];
    }
    std::unordered_map<std::string, NpzArray> arrs;
    arrs["cids.npy"] = npz_from_bytes("<i8", {pool.cids.size()}, cids64.data(), cids64.size() * sizeof(int64_t));
    arrs["bits.npy"] = npz_from_bytes("<u8", {pool.bits.size()}, pool.bits.data(), pool.bits.size() * sizeof(uint64_t));
    const int32_t n_words = pool.n_words;
    arrs["n_words.npy"] = npz_from_bytes("<i4", {1}, &n_words, sizeof(int32_t));
    arrs["leaf_off.npy"] =
        npz_from_bytes("<i8", {pool.leaf_off.size()}, pool.leaf_off.data(), pool.leaf_off.size() * sizeof(int64_t));
    arrs["leaf_n.npy"] =
        npz_from_bytes("<i4", {pool.leaf_n.size()}, pool.leaf_n.data(), pool.leaf_n.size() * sizeof(int32_t));
    return append_pool_chunk("wide", arrs, pool_blob);
}

bool matrix_pool_to_chunk(const OwnedMatrixPool& pool, std::vector<unsigned char>& pool_blob) {
    std::vector<int64_t> cids64(pool.cids.size());
    for (std::size_t i = 0; i < pool.cids.size(); ++i) {
        cids64[i] = pool.cids[i];
    }
    std::unordered_map<std::string, NpzArray> arrs;
    arrs["cids.npy"] = npz_from_bytes("<i8", {pool.cids.size()}, cids64.data(), cids64.size() * sizeof(int64_t));
    const std::size_t n_rows = pool.cids.size();
    arrs["mat.npy"] = npz_from_bytes(
        "|u1",
        {n_rows, static_cast<std::size_t>(pool.L)},
        pool.mat.data(),
        pool.mat.size());
    if (!pool.spop.empty()) {
        arrs["spop.npy"] = npz_from_bytes("|u1", {pool.spop.size()}, pool.spop.data(), pool.spop.size());
    }
    arrs["leaf_off.npy"] =
        npz_from_bytes("<i8", {pool.leaf_off.size()}, pool.leaf_off.data(), pool.leaf_off.size() * sizeof(int64_t));
    arrs["leaf_n.npy"] =
        npz_from_bytes("<i4", {pool.leaf_n.size()}, pool.leaf_n.data(), pool.leaf_n.size() * sizeof(int32_t));
    const int16_t L = pool.L;
    arrs["L.npy"] = npz_from_bytes("<i2", {1}, &L, sizeof(int16_t));
    return append_pool_chunk("matrix", arrs, pool_blob);
}

struct ViewRow {
    int node_key = 0;
    int pool_id = 0;
    int leaf_idx = 0;
    int L = 0;
    int n = 0;
};

struct CompactRow {
    int node_key = 0;
    std::vector<unsigned char> blob;
};

}  // namespace

static bool write_parts_impl(
    const HpcxWriteParts& parts,
    const std::string& out_path,
    HpcxWriteStats* stats,
    std::string* err) {
    const auto t0 = std::chrono::steady_clock::now();

    if (parts.partition.empty()) {
        if (err) {
            *err = "partition section missing";
        }
        return false;
    }

    std::vector<unsigned char> pool_blob;
    for (const auto& pool : parts.packed_pools) {
        if (!packed_pool_to_chunk(pool, pool_blob)) {
            if (err) {
                *err = "packed pool encode failed";
            }
            return false;
        }
    }
    for (const auto& pool : parts.wide_pools) {
        if (!wide_pool_to_chunk(pool, pool_blob)) {
            if (err) {
                *err = "wide pool encode failed";
            }
            return false;
        }
    }
    for (const auto& pool : parts.matrix_pools) {
        if (!matrix_pool_to_chunk(pool, pool_blob)) {
            if (err) {
                *err = "matrix pool encode failed";
            }
            return false;
        }
    }

    std::vector<ViewRow> views;
    std::vector<CompactRow> compacts;
    views.reserve(parts.tree_mgn.size());
    compacts.reserve(128);
    const int packed_count = static_cast<int>(parts.packed_pools.size());
    for (const auto& kv : parts.tree_mgn) {
        const int node_key = kv.first;
        const HpcxMgnEntry& ent = kv.second;
        if (ent.kind == HpcxMgnEntry::COMPACT) {
            CompactRow row;
            row.node_key = node_key;
            row.blob = mgn_compact_to_bytes(ent.compact);
            compacts.push_back(std::move(row));
            continue;
        }
        ViewRow row;
        row.node_key = node_key;
        row.leaf_idx = ent.leaf_idx;
        row.L = ent.L;
        row.n = ent.n;
        if (ent.kind == HpcxMgnEntry::PACKED) {
            row.pool_id = ent.pool_id;
        } else if (ent.kind == HpcxMgnEntry::WIDE) {
            row.pool_id = packed_count + ent.pool_id;
        } else {
            row.pool_id = packed_count + static_cast<int>(parts.wide_pools.size()) + ent.pool_id;
        }
        views.push_back(row);
    }
    std::sort(views.begin(), views.end(), [](const ViewRow& a, const ViewRow& b) {
        return a.node_key < b.node_key;
    });
    std::sort(compacts.begin(), compacts.end(), [](const CompactRow& a, const CompactRow& b) {
        return a.node_key < b.node_key;
    });

    std::vector<unsigned char> views_bytes(static_cast<std::size_t>(views.size()) * 5 * sizeof(int64_t));
    for (std::size_t i = 0; i < views.size(); ++i) {
        int64_t* row = reinterpret_cast<int64_t*>(views_bytes.data() + i * 5 * sizeof(int64_t));
        row[0] = views[i].node_key;
        row[1] = views[i].pool_id;
        row[2] = views[i].leaf_idx;
        row[3] = views[i].L;
        row[4] = views[i].n;
    }

    std::vector<unsigned char> compact_blobs;
    std::vector<unsigned char> compact_idx(static_cast<std::size_t>(compacts.size()) * 3 * sizeof(int64_t));
    for (std::size_t i = 0; i < compacts.size(); ++i) {
        const std::size_t off = compact_blobs.size();
        compact_blobs.insert(compact_blobs.end(), compacts[i].blob.begin(), compacts[i].blob.end());
        int64_t* row = reinterpret_cast<int64_t*>(compact_idx.data() + i * 3 * sizeof(int64_t));
        row[0] = compacts[i].node_key;
        row[1] = static_cast<int64_t>(off);
        row[2] = static_cast<int64_t>(compacts[i].blob.size());
    }

    std::vector<unsigned char> sections[HPCX_SEC_COUNT];
    if (!parts.meta.empty()) {
        sections[HPCX_SEC_META_JSON] = parts.meta;
    }
    sections[HPCX_SEC_PARTITION_FLAT] = parts.partition;
    if (!pool_blob.empty()) {
        sections[HPCX_SEC_MGN_PACKED] = std::move(pool_blob);
    }
    if (!views_bytes.empty()) {
        sections[HPCX_SEC_MGN_VIEWS] = std::move(views_bytes);
    }
    if (!compact_idx.empty()) {
        sections[HPCX_SEC_MGN_COMPACT_INDEX] = std::move(compact_idx);
    }
    if (!compact_blobs.empty()) {
        sections[HPCX_SEC_MGN_COMPACT_BLOBS] = std::move(compact_blobs);
    }

    const auto write_u32 = [](std::vector<unsigned char>& buf, std::size_t off, uint32_t v) {
        if (off + 4 > buf.size()) {
            buf.resize(off + 4, 0);
        }
        buf[off + 0] = static_cast<unsigned char>(v & 0xff);
        buf[off + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
        buf[off + 2] = static_cast<unsigned char>((v >> 16) & 0xff);
        buf[off + 3] = static_cast<unsigned char>((v >> 24) & 0xff);
    };
    const auto write_u64 = [](std::vector<unsigned char>& buf, std::size_t off, uint64_t v) {
        if (off + 8 > buf.size()) {
            buf.resize(off + 8, 0);
        }
        for (int i = 0; i < 8; ++i) {
            buf[off + static_cast<std::size_t>(i)] = static_cast<unsigned char>((v >> (8 * i)) & 0xff);
        }
    };

    const std::size_t header_bytes = 64 + static_cast<std::size_t>(HPCX_SEC_COUNT) * 16;
    std::vector<unsigned char> body(header_bytes, 0);
    write_u32(body, 0, HPCX_MAGIC);
    write_u32(body, 4, HPCX_VERSION);
    write_u32(body, 8, parts.band);
    write_u32(body, 12, 0);

    std::size_t off = header_bytes;
    for (int sec = 0; sec < HPCX_SEC_COUNT; ++sec) {
        const std::size_t slen = sections[sec].size();
        write_u64(body, 16 + static_cast<std::size_t>(sec) * 16, off);
        write_u64(body, 24 + static_cast<std::size_t>(sec) * 16, static_cast<uint64_t>(slen));
        off += slen;
    }
    for (int sec = 0; sec < HPCX_SEC_COUNT; ++sec) {
        body.insert(body.end(), sections[sec].begin(), sections[sec].end());
    }

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        if (err) {
            *err = "open out failed";
        }
        return false;
    }
    out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    if (!out) {
        if (err) {
            *err = "write out failed";
        }
        return false;
    }

    if (stats != nullptr) {
        stats->file_bytes = body.size();
        stats->n_views = static_cast<int>(views.size());
        stats->n_pools = static_cast<int>(
            parts.packed_pools.size() + parts.wide_pools.size() + parts.matrix_pools.size());
        stats->n_compact = static_cast<int>(compacts.size());
        stats->write_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}

bool hpcx_write_parts(
    const HpcxWriteParts& parts,
    const std::string& out_path,
    HpcxWriteStats* stats,
    std::string* err) {
    return write_parts_impl(parts, out_path, stats, err);
}

bool hpcx_write_from_index(
    const HpcxIndex& idx,
    const std::string& out_path,
    HpcxWriteStats* stats,
    std::string* err) {
    const unsigned char* meta = nullptr;
    const unsigned char* part = nullptr;
    std::size_t meta_len = 0;
    std::size_t part_len = 0;
    if (!hpcx_get_section(idx, HPCX_SEC_META_JSON, &meta, &meta_len)) {
        if (err) {
            *err = "meta section missing";
        }
        return false;
    }
    if (!hpcx_get_section(idx, HPCX_SEC_PARTITION_FLAT, &part, &part_len) || part == nullptr || part_len == 0) {
        if (err) {
            *err = "partition section missing";
        }
        return false;
    }

    HpcxWriteParts parts;
    parts.band = idx.band;
    if (meta != nullptr && meta_len > 0) {
        parts.meta.assign(meta, meta + meta_len);
    }
    parts.partition.assign(part, part + part_len);
    parts.packed_pools = idx.packed_pools;
    parts.wide_pools = idx.wide_pools;
    parts.matrix_pools = idx.matrix_pools;
    parts.tree_mgn = idx.tree_mgn;
    return write_parts_impl(parts, out_path, stats, err);
}
