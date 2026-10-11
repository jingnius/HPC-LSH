#include "hpc_sig_io.h"

#include <cstring>
#include <fstream>

namespace {

bool read_exact(std::ifstream& in, void* dst, std::size_t n, std::string* err) {
    if (n == 0) {
        return true;
    }
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (!in) {
        if (err) {
            *err = "unexpected EOF reading SIGB";
        }
        return false;
    }
    return true;
}

bool validate_header(const HpcSigBandHeader& hdr, std::string* err) {
    if (hdr.magic != HPC_SIGB_MAGIC) {
        if (err) {
            *err = "bad SIGB magic";
        }
        return false;
    }
    if (hdr.version != HPC_SIGB_VERSION) {
        if (err) {
            *err = "unsupported SIGB version";
        }
        return false;
    }
    return true;
}

}  // namespace

bool hpc_sigb_read_header(const std::string& path, HpcSigBandMeta* meta, std::string* err) {
    if (meta == nullptr) {
        if (err) {
            *err = "null meta";
        }
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open SIGB: " + path;
        }
        return false;
    }
    HpcSigBandHeader hdr{};
    if (!read_exact(in, &hdr, sizeof(hdr), err)) {
        return false;
    }
    if (!validate_header(hdr, err)) {
        return false;
    }
    meta->band = hdr.band;
    meta->num_perm = hdr.num_perm;
    meta->n_cols = hdr.n_cols;
    return true;
}

bool hpc_sigb_for_each_column(
    const std::string& path,
    const std::function<bool(const HpcColumnSig&)>& fn,
    std::string* err) {
    if (!fn) {
        if (err) {
            *err = "null column callback";
        }
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open SIGB: " + path;
        }
        return false;
    }
    HpcSigBandHeader hdr{};
    if (!read_exact(in, &hdr, sizeof(hdr), err)) {
        return false;
    }
    if (!validate_header(hdr, err)) {
        return false;
    }
    for (uint32_t i = 0; i < hdr.n_cols; ++i) {
        HpcColumnSig col;
        uint32_t raw_len = 0;
        if (!read_exact(in, &col.col_id, sizeof(col.col_id), err)) {
            return false;
        }
        if (!read_exact(in, &col.hit_count, sizeof(col.hit_count), err)) {
            return false;
        }
        if (!read_exact(in, &col.sig_len, sizeof(col.sig_len), err)) {
            return false;
        }
        if (!read_exact(in, &raw_len, sizeof(raw_len), err)) {
            return false;
        }
        col.sig.resize(raw_len);
        if (raw_len > 0 && !read_exact(in, col.sig.data(), raw_len * sizeof(int32_t), err)) {
            return false;
        }
        if (!fn(col)) {
            if (err) {
                *err = "column callback returned false";
            }
            return false;
        }
    }
    return true;
}

bool hpc_sigb_read_all(
    const std::string& path,
    std::vector<HpcColumnSig>* cols,
    HpcSigBandMeta* meta,
    std::string* err) {
    if (cols == nullptr) {
        if (err) {
            *err = "null cols";
        }
        return false;
    }
    cols->clear();
    HpcSigBandMeta local_meta{};
    if (!hpc_sigb_read_header(path, &local_meta, err)) {
        return false;
    }
    cols->reserve(local_meta.n_cols);
    const bool ok = hpc_sigb_for_each_column(
        path,
        [&](const HpcColumnSig& col) {
            cols->push_back(col);
            return true;
        },
        err);
    if (ok && meta != nullptr) {
        *meta = local_meta;
    }
    return ok;
}
