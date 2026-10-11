#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HpcSigHitsMmap {
    static constexpr uint32_t kMagic = 0x48485453u; /* 'SHTH' */

#pragma pack(push, 1)
    struct FileHeader {
        uint32_t magic = kMagic;
        uint32_t n_cols = 0;
        uint64_t total_hits = 0;
    };
#pragma pack(pop)

    int fd = -1;
    void* base = nullptr;
    std::size_t len = 0;
    std::string path;
    std::vector<int64_t> col_ids;
    std::vector<uint32_t> hit_off;

    ~HpcSigHitsMmap();
    HpcSigHitsMmap() = default;
    HpcSigHitsMmap(const HpcSigHitsMmap&) = delete;
    HpcSigHitsMmap& operator=(const HpcSigHitsMmap&) = delete;
    HpcSigHitsMmap(HpcSigHitsMmap&& other) noexcept;
    HpcSigHitsMmap& operator=(HpcSigHitsMmap&& other) noexcept;

    void reset();
    const int32_t* hit_pool() const;
    std::size_t hit_pool_len() const;

    static bool build_from_sigb(const std::string& sig_native_path, HpcSigHitsMmap* out, std::string* err);
};
