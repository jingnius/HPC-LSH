#pragma once

#include <cstdint>

// Native interchange formats for the pure C++ signature/HPC pipeline.
// Step 0 baseline — binary, no pickle.

#define NATIVE_DICT_MAGIC 0x54434944u /* 'DICT' little-endian */
#define NATIVE_INVR_MAGIC 0x52565249u /* 'INVR' little-endian */
#define NATIVE_CYBF_MAGIC 0x46425943u /* 'CYBF' little-endian */
#define NATIVE_SIGB_MAGIC 0x42474953u /* 'SIGB' little-endian */
#define NATIVE_SORD_MAGIC 0x44524F53u /* 'SORD' little-endian */
#define NATIVE_FORMAT_VERSION 1u

#pragma pack(push, 1)
struct NativeDictHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t n_tokens; /* token ids are 0 .. n_tokens-1 */
};

struct NativeInverHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t max_gram_id;
    uint32_t n_lists; /* max_gram_id + 1 */
    uint64_t n_entries;
};
struct NativeCybufHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t n_cols;
    uint32_t max_gram_id;
    uint32_t num_perm;
    uint64_t n_gram_entries;
};

struct NativeSigBandHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t band;
    uint32_t num_perm;
    uint32_t n_cols;
    uint64_t n_sig_ints;
};

struct NativeSampleOrderHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t n_samples;
};

#pragma pack(pop)

/* DICT body: n_tokens × (uint32_t str_len, str_bytes[str_len])
 * Strings are UTF-8; token id = array index.
 *
 * INVR body (CSR):
 *   offsets[n_lists + 1]  uint64_t
 *   col_ids[n_entries]    uint32_t
 */
