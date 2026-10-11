#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#define HPC_SIGB_MAGIC 0x42474953u /* 'SIGB' little-endian */
#define HPC_SIGB_VERSION 1u

#pragma pack(push, 1)
struct HpcSigBandHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t band;
    uint32_t num_perm;
    uint32_t n_cols;
    uint64_t n_sig_ints;
};
#pragma pack(pop)

struct HpcColumnSig {
    uint64_t col_id = 0;
    int hit_count = 0;
    int sig_len = 0;
    std::vector<int32_t> sig;
};

struct HpcSigBandMeta {
    uint32_t band = 0;
    uint32_t num_perm = 5;
    uint32_t n_cols = 0;
};

// Read SIGB header only.
bool hpc_sigb_read_header(const std::string& path, HpcSigBandMeta* meta, std::string* err);

// Stream columns without loading the full band into memory.
bool hpc_sigb_for_each_column(
    const std::string& path,
    const std::function<bool(const HpcColumnSig&)>& fn,
    std::string* err);

// Load full band (testing / small N only).
bool hpc_sigb_read_all(const std::string& path, std::vector<HpcColumnSig>* cols, HpcSigBandMeta* meta, std::string* err);
