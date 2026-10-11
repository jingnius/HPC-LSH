#ifndef HPC_BAND_FORMAT_H
#define HPC_BAND_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPCX_MAGIC 0x58504348u /* 'HPCX' little-endian */
#define HPCX_VERSION 1u

enum HpcxSectionId {
    HPCX_SEC_META_JSON = 0,
    HPCX_SEC_PARTITION_FLAT = 1,
    HPCX_SEC_MGN_PACKED = 2,
    HPCX_SEC_MGN_MATRIX = 3,
    HPCX_SEC_MGN_VIEWS = 4,
    HPCX_SEC_MGN_COMPACT_INDEX = 5,
    HPCX_SEC_MGN_COMPACT_BLOBS = 6,
    HPCX_SEC_COUNT = 7,
};

#pragma pack(push, 1)
typedef struct HpcxHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t band;
    uint32_t flags;
    uint64_t section_off[HPCX_SEC_COUNT];
    uint64_t section_len[HPCX_SEC_COUNT];
} HpcxHeader;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* HPC_BAND_FORMAT_H */
