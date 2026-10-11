#include "hpc_index_one_io.h"

#include <fstream>

namespace {

bool write_exact(std::ofstream& out, const void* src, std::size_t n, std::string* err) {
    if (n == 0) {
        return true;
    }
    out.write(static_cast<const char*>(src), static_cast<std::streamsize>(n));
    if (!out) {
        if (err) {
            *err = "write failed for index_one";
        }
        return false;
    }
    return true;
}

bool read_exact(std::ifstream& in, void* dst, std::size_t n, std::string* err) {
    if (n == 0) {
        return true;
    }
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (!in) {
        if (err) {
            *err = "unexpected EOF reading index_one";
        }
        return false;
    }
    return true;
}

}  // namespace

bool hpc_index_one_write(
    const std::string& path,
    const std::unordered_map<uint64_t, std::vector<int32_t>>& index_one,
    std::string* err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) {
            *err = "cannot open index_one for write: " + path;
        }
        return false;
    }
    const uint32_t magic = HPC_INDEX_ONE_MAGIC;
    const uint32_t version = 1u;
    const uint32_t n = static_cast<uint32_t>(index_one.size());
    if (!write_exact(out, &magic, sizeof(magic), err)) {
        return false;
    }
    if (!write_exact(out, &version, sizeof(version), err)) {
        return false;
    }
    if (!write_exact(out, &n, sizeof(n), err)) {
        return false;
    }
    for (const auto& kv : index_one) {
        const uint64_t col_id = kv.first;
        const uint32_t n_hits = static_cast<uint32_t>(kv.second.size());
        if (!write_exact(out, &col_id, sizeof(col_id), err)) {
            return false;
        }
        if (!write_exact(out, &n_hits, sizeof(n_hits), err)) {
            return false;
        }
        if (n_hits > 0 && !write_exact(out, kv.second.data(), n_hits * sizeof(int32_t), err)) {
            return false;
        }
    }
    return static_cast<bool>(out);
}

bool hpc_index_one_read(
    const std::string& path,
    std::vector<HpcIndexOneEntry>* out,
    uint32_t* n_cols,
    std::string* err) {
    if (out == nullptr) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    out->clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open index_one: " + path;
        }
        return false;
    }
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t n = 0;
    if (!read_exact(in, &magic, sizeof(magic), err)) {
        return false;
    }
    if (magic != HPC_INDEX_ONE_MAGIC) {
        if (err) {
            *err = "bad index_one magic";
        }
        return false;
    }
    if (!read_exact(in, &version, sizeof(version), err)) {
        return false;
    }
    if (version != 1u) {
        if (err) {
            *err = "unsupported index_one version";
        }
        return false;
    }
    if (!read_exact(in, &n, sizeof(n), err)) {
        return false;
    }
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        HpcIndexOneEntry ent;
        uint32_t n_hits = 0;
        if (!read_exact(in, &ent.col_id, sizeof(ent.col_id), err)) {
            return false;
        }
        if (!read_exact(in, &n_hits, sizeof(n_hits), err)) {
            return false;
        }
        ent.hits.resize(n_hits);
        if (n_hits > 0 && !read_exact(in, ent.hits.data(), n_hits * sizeof(int32_t), err)) {
            return false;
        }
        out->push_back(std::move(ent));
    }
    if (n_cols != nullptr) {
        *n_cols = n;
    }
    return true;
}
