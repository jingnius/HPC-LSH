#include "hpc_sig_hits_mmap.h"

#include "hpc_sig_index.h"
#include "hpc_sig_io.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include <cstdlib>
#include <string>
#include <vector>

std::string hits_mmap_tmpdir() {
    const char* dir = std::getenv("TMPDIR");
    if (dir == nullptr || dir[0] == '\0') {
        dir = std::getenv("HPCX_TMPDIR");
    }
    std::string base = (dir != nullptr && dir[0] != '\0') ? dir : "work/tmp";
    if (base.back() != '/') {
        base.push_back('/');
    }
    return base;
}


namespace {

struct RowMeta {
    int64_t col_id = 0;
    uint32_t hit_len = 0;
    std::size_t src_index = 0;
};

std::size_t hits_file_bytes(uint32_t n_cols, uint64_t total_hits) {
    return sizeof(HpcSigHitsMmap::FileHeader)
        + static_cast<std::size_t>(n_cols) * sizeof(int64_t)
        + static_cast<std::size_t>(n_cols + 1u) * sizeof(uint32_t)
        + static_cast<std::size_t>(total_hits) * sizeof(int32_t);
}

}  // namespace

HpcSigHitsMmap::~HpcSigHitsMmap() {
    reset();
}

HpcSigHitsMmap::HpcSigHitsMmap(HpcSigHitsMmap&& other) noexcept {
    fd = other.fd;
    base = other.base;
    len = other.len;
    path = std::move(other.path);
    col_ids = std::move(other.col_ids);
    hit_off = std::move(other.hit_off);
    other.fd = -1;
    other.base = nullptr;
    other.len = 0;
}

HpcSigHitsMmap& HpcSigHitsMmap::operator=(HpcSigHitsMmap&& other) noexcept {
    if (this != &other) {
        reset();
        fd = other.fd;
        base = other.base;
        len = other.len;
        path = std::move(other.path);
        col_ids = std::move(other.col_ids);
        hit_off = std::move(other.hit_off);
        other.fd = -1;
        other.base = nullptr;
        other.len = 0;
    }
    return *this;
}

void HpcSigHitsMmap::reset() {
    if (base != nullptr && base != MAP_FAILED) {
        ::munmap(base, len);
    }
    base = nullptr;
    len = 0;
    if (fd >= 0) {
        ::close(fd);
    }
    fd = -1;
    if (!path.empty()) {
        ::unlink(path.c_str());
        path.clear();
    }
    col_ids.clear();
    hit_off.clear();
}

const int32_t* HpcSigHitsMmap::hit_pool() const {
    if (base == nullptr || base == MAP_FAILED || hit_off.empty()) {
        return nullptr;
    }
    const auto* hdr = static_cast<const FileHeader*>(base);
    const auto* bytes = static_cast<const unsigned char*>(base);
    const std::size_t off = sizeof(FileHeader)
        + static_cast<std::size_t>(hdr->n_cols) * sizeof(int64_t)
        + static_cast<std::size_t>(hdr->n_cols + 1u) * sizeof(uint32_t);
    return reinterpret_cast<const int32_t*>(bytes + off);
}

std::size_t HpcSigHitsMmap::hit_pool_len() const {
    if (base == nullptr || base == MAP_FAILED) {
        return 0;
    }
    const auto* hdr = static_cast<const FileHeader*>(base);
    return static_cast<std::size_t>(hdr->total_hits);
}

bool HpcSigHitsMmap::build_from_sigb(const std::string& sig_native_path, HpcSigHitsMmap* out, std::string* err) {
    if (out == nullptr) {
        if (err) {
            *err = "null HpcSigHitsMmap out";
        }
        return false;
    }
    out->reset();

    std::vector<RowMeta> rows;
    if (!hpc_sigb_for_each_column(
            sig_native_path,
            [&](const HpcColumnSig& col) {
                RowMeta row;
                row.col_id = static_cast<int64_t>(col.col_id);
                row.hit_len = static_cast<uint32_t>(std::max(col.hit_count, 0));
                row.src_index = rows.size();
                rows.push_back(row);
                return true;
            },
            err)) {
        return false;
    }

    std::sort(rows.begin(), rows.end(), [](const RowMeta& a, const RowMeta& b) {
        return a.col_id < b.col_id;
    });

    const uint32_t n_cols = static_cast<uint32_t>(rows.size());
    out->col_ids.resize(rows.size());
    out->hit_off.assign(rows.size() + 1, 0);
    uint64_t total_hits = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        out->col_ids[i] = rows[i].col_id;
        total_hits += rows[i].hit_len;
        out->hit_off[i + 1] = static_cast<uint32_t>(total_hits);
    }

    std::string templ = hits_mmap_tmpdir() + "hpc_hits_XXXXXX";
    std::vector<char> tmp_buf(templ.begin(), templ.end());
    tmp_buf.push_back('\0');
    const int fd = ::mkstemp(tmp_buf.data());
    if (fd < 0) {
        if (err) {
            *err = "mkstemp failed for hits mmap";
        }
        return false;
    }
    out->path = std::string(tmp_buf.data());
    out->fd = fd;

    const std::size_t file_bytes = hits_file_bytes(n_cols, total_hits);
    if (::ftruncate(fd, static_cast<off_t>(file_bytes)) != 0) {
        if (err) {
            *err = "ftruncate failed for hits mmap";
        }
        out->reset();
        return false;
    }

    void* mapped = ::mmap(nullptr, file_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        if (err) {
            *err = "mmap failed for hits mmap build";
        }
        out->reset();
        return false;
    }

    auto* hdr = static_cast<FileHeader*>(mapped);
    hdr->magic = kMagic;
    hdr->n_cols = n_cols;
    hdr->total_hits = total_hits;

    auto* bytes = static_cast<unsigned char*>(mapped);
    std::memcpy(bytes + sizeof(FileHeader), out->col_ids.data(), out->col_ids.size() * sizeof(int64_t));
    const std::size_t hit_off_pos = sizeof(FileHeader) + out->col_ids.size() * sizeof(int64_t);
    std::memcpy(bytes + hit_off_pos, out->hit_off.data(), out->hit_off.size() * sizeof(uint32_t));

    int32_t* pool = reinterpret_cast<int32_t*>(bytes + hit_off_pos + out->hit_off.size() * sizeof(uint32_t));
    std::vector<uint32_t> write_pos(rows.size(), 0);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        write_pos[rows[i].src_index] = out->hit_off[i];
    }

    std::size_t row_i = 0;
    if (!hpc_sigb_for_each_column(
            sig_native_path,
            [&](const HpcColumnSig& col) {
                const uint32_t dst = write_pos[row_i++];
                const std::vector<int> decoded = hpc_sig_zip_to_index_one(
                    col.sig.data(), static_cast<int>(col.sig.size()));
                if (!decoded.empty()) {
                    std::copy(
                        decoded.begin(),
                        decoded.end(),
                        pool + static_cast<std::ptrdiff_t>(dst));
                }
                return true;
            },
            err)) {
        ::munmap(mapped, file_bytes);
        out->reset();
        return false;
    }

    ::msync(mapped, file_bytes, MS_SYNC);
    ::munmap(mapped, file_bytes);

    void* read_map = ::mmap(nullptr, file_bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (read_map == MAP_FAILED) {
        if (err) {
            *err = "mmap read-only failed for hits mmap";
        }
        out->reset();
        return false;
    }

    out->base = read_map;
    out->len = file_bytes;
    rows.clear();
    rows.shrink_to_fit();
    return true;
}
