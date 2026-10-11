#include "inver_io.h"

#include "native_format.h"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

InverMmapRegion::~InverMmapRegion() {
    if (base != nullptr && base != MAP_FAILED) {
        munmap(const_cast<uint8_t*>(base), size);
    }
    if (fd >= 0) {
        close(fd);
    }
}

static bool read_exact(std::ifstream& in, void* buf, std::size_t n, std::string* err) {
    in.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    if (!in) {
        if (err) {
            *err = "unexpected EOF";
        }
        return false;
    }
    return true;
}

static const uint64_t* inver_offsets_ptr(const DenseInver& inv) {
    if (inv.offsets_view != nullptr) {
        return inv.offsets_view;
    }
    return inv.offsets.data();
}

static const uint32_t* inver_col_ids_ptr(const DenseInver& inv) {
    if (inv.col_ids_view != nullptr) {
        return inv.col_ids_view;
    }
    return inv.col_ids.data();
}

bool inver_read_native(const std::string& path, DenseInver* out, std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    out->mmap.reset();
    out->offsets_view = nullptr;
    out->col_ids_view = nullptr;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open: " + path;
        }
        return false;
    }
    NativeInverHeader hdr{};
    if (!read_exact(in, &hdr, sizeof(hdr), err)) {
        return false;
    }
    if (hdr.magic != NATIVE_INVR_MAGIC || hdr.version != NATIVE_FORMAT_VERSION) {
        if (err) {
            *err = "bad INVR magic/version";
        }
        return false;
    }
    const std::size_t n_off = static_cast<std::size_t>(hdr.n_lists) + 1u;
    out->max_gram_id = hdr.max_gram_id;
    out->offsets.resize(n_off);
    if (!read_exact(in, out->offsets.data(), n_off * sizeof(uint64_t), err)) {
        return false;
    }
    out->col_ids.resize(static_cast<std::size_t>(hdr.n_entries));
    if (hdr.n_entries > 0 &&
        !read_exact(in, out->col_ids.data(), out->col_ids.size() * sizeof(uint32_t), err)) {
        return false;
    }
    return true;
}

bool inver_read_native_mmap(const std::string& path, DenseInver* out, std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    out->mmap.reset();
    out->offsets.clear();
    out->col_ids.clear();
    out->offsets_view = nullptr;
    out->col_ids_view = nullptr;

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (err) {
            *err = "cannot open: " + path;
        }
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(NativeInverHeader))) {
        close(fd);
        if (err) {
            *err = "bad INVR file size";
        }
        return false;
    }
    const size_t file_size = static_cast<size_t>(st.st_size);
    void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        if (err) {
            *err = "mmap inver failed: " + path;
        }
        return false;
    }
    auto region = std::make_shared<InverMmapRegion>();
    region->fd = fd;
    region->size = file_size;
    region->base = static_cast<const uint8_t*>(mapped);

    const auto* base = region->base;
    NativeInverHeader hdr{};
    std::memcpy(&hdr, base, sizeof(hdr));
    if (hdr.magic != NATIVE_INVR_MAGIC || hdr.version != NATIVE_FORMAT_VERSION) {
        if (err) {
            *err = "bad INVR magic/version";
        }
        return false;
    }
    const size_t n_off = static_cast<size_t>(hdr.n_lists) + 1u;
    const size_t off_bytes = n_off * sizeof(uint64_t);
    const size_t col_off = sizeof(hdr) + off_bytes;
    if (col_off + hdr.n_entries * sizeof(uint32_t) > file_size) {
        if (err) {
            *err = "truncated INVR body";
        }
        return false;
    }

    out->mmap = std::move(region);
    out->max_gram_id = hdr.max_gram_id;
    out->offsets_view = reinterpret_cast<const uint64_t*>(base + sizeof(hdr));
    out->col_ids_view = reinterpret_cast<const uint32_t*>(base + col_off);
    return true;
}

bool inver_write_native(const std::string& path, const DenseInver& inver, std::string* err) {
    if (inver.offsets_view != nullptr || inver.col_ids_view != nullptr) {
        if (err) {
            *err = "cannot write mmap INVR view";
        }
        return false;
    }
    if (inver.offsets.size() != static_cast<std::size_t>(inver.max_gram_id) + 2u) {
        if (err) {
            *err = "offsets size mismatch";
        }
        return false;
    }
    NativeInverHeader hdr{};
    hdr.magic = NATIVE_INVR_MAGIC;
    hdr.version = NATIVE_FORMAT_VERSION;
    hdr.max_gram_id = inver.max_gram_id;
    hdr.n_lists = inver.max_gram_id + 1u;
    hdr.n_entries = static_cast<uint64_t>(inver.col_ids.size());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) {
            *err = "cannot write: " + path;
        }
        return false;
    }
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(inver.offsets.data()),
              static_cast<std::streamsize>(inver.offsets.size() * sizeof(uint64_t)));
    if (!inver.col_ids.empty()) {
        out.write(reinterpret_cast<const char*>(inver.col_ids.data()),
                  static_cast<std::streamsize>(inver.col_ids.size() * sizeof(uint32_t)));
    }
    return static_cast<bool>(out);
}

bool inver_compare(const DenseInver& a, const DenseInver& b, std::string* err) {
    if (a.max_gram_id != b.max_gram_id) {
        if (err) {
            *err = "INVR mismatch max_gram_id";
        }
        return false;
    }
    const std::size_t n_off = static_cast<std::size_t>(a.max_gram_id) + 2u;
    const uint64_t* ao = inver_offsets_ptr(a);
    const uint64_t* bo = inver_offsets_ptr(b);
    if (ao == nullptr || bo == nullptr) {
        if (err) {
            *err = "INVR missing offsets";
        }
        return false;
    }
    for (std::size_t i = 0; i < n_off; ++i) {
        if (ao[i] != bo[i]) {
            if (err) {
                *err = "INVR mismatch offsets";
            }
            return false;
        }
    }
    const uint64_t n_ent = ao[n_off - 1];
    const uint32_t* ac = inver_col_ids_ptr(a);
    const uint32_t* bc = inver_col_ids_ptr(b);
    for (uint64_t i = 0; i < n_ent; ++i) {
        if (ac[i] != bc[i]) {
            if (err) {
                *err = "INVR mismatch col_ids";
            }
            return false;
        }
    }
    return true;
}
