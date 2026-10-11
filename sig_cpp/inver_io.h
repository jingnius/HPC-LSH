#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct InverMmapRegion {
    int fd = -1;
    size_t size = 0;
    const uint8_t* base = nullptr;
    ~InverMmapRegion();
};

struct DenseInver {
    uint32_t max_gram_id = 0;
    std::vector<uint64_t> offsets; /* size max_gram_id + 2 */
    std::vector<uint32_t> col_ids;
    std::shared_ptr<InverMmapRegion> mmap;
    const uint64_t* offsets_view = nullptr;
    const uint32_t* col_ids_view = nullptr;
};

bool inver_read_native(const std::string& path, DenseInver* out, std::string* err);
/** Map INVR file read-only; offsets/col_ids are views into the mapping (kept in out->mmap). */
bool inver_read_native_mmap(const std::string& path, DenseInver* out, std::string* err);
bool inver_write_native(const std::string& path, const DenseInver& inver, std::string* err);

bool inver_compare(const DenseInver& a, const DenseInver& b, std::string* err);

inline const uint64_t* inver_offsets_cptr(const DenseInver& inv) {
    if (inv.offsets_view != nullptr) {
        return inv.offsets_view;
    }
    return inv.offsets.empty() ? nullptr : inv.offsets.data();
}

inline const uint32_t* inver_col_ids_cptr(const DenseInver& inv) {
    if (inv.col_ids_view != nullptr) {
        return inv.col_ids_view;
    }
    return inv.col_ids.empty() ? nullptr : inv.col_ids.data();
}
