#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct CybufMmapRegion {
  int fd = -1;
  size_t size = 0;
  const uint8_t* base = nullptr;
  ~CybufMmapRegion();
};

struct CybufAux {
  uint32_t n_cols = 0;
  uint32_t num_perm = 5;
  uint32_t max_gram_id = 0;
  std::vector<uint64_t> col_ids;
  /** Populated when CYBF is fully loaded into RAM (default read path). */
  std::vector<std::vector<uint32_t>> col_grams;
  std::vector<uint32_t> gram_counts;

  /** mmap CYBF: flat CSR views (col_grams empty). Mapping kept alive via mmap. */
  std::shared_ptr<CybufMmapRegion> mmap;
  std::vector<uint64_t> gram_off;
  const uint32_t* flat_grams = nullptr;

  bool cybuf_is_mmap() const { return static_cast<bool>(mmap); }

  void for_each_col_gram(uint32_t col_idx, const std::function<void(uint32_t gram_id)>& fn) const;
};

/** Drop CYBF/inver-related RAM after sig compute; safe before stream merge from disk parts. */
void cybuf_release_build_artifacts(CybufAux* cy);

struct ColumnSig {
  uint64_t col_id = 0;
  uint32_t hit_count = 0;
  uint32_t sig_len = 0;
  std::vector<int32_t> sig;
};

struct SigBand {
  uint32_t band = 0;
  uint32_t num_perm = 5;
  uint32_t n_cols = 0;
  uint32_t n_samples = 0;
  std::vector<ColumnSig> columns;
};

bool cybuf_read_native(const std::string& path, CybufAux* out, std::string* err);
bool cybuf_write_native(const std::string& path, const CybufAux& in, std::string* err);
bool cybuf_compare(const CybufAux& a, const CybufAux& b, std::string* err);

bool sig_read_native(const std::string& path, SigBand* out, std::string* err);
bool sig_write_native(const std::string& path, const SigBand& in, std::string* err);
bool sig_compare(const SigBand& a, const SigBand& b, std::string* err);

bool sample_order_read_native(const std::string& path, std::vector<int32_t>* out, std::string* err);
bool sample_order_write_native(const std::string& path, const std::vector<int32_t>& order, std::string* err);

struct SigBuildOptions {
  /** If set, load CSR inver from file (lazy lists). Else build from cybuf when needed. */
  const char* inver_path = nullptr;
  /** If set, spill completed columns in batches, then merge into this SIGB path. */
  const char* stream_out_path = nullptr;
};

bool sig_build(CybufAux& cy, int band, uint32_t seed, SigBand* out, std::vector<int32_t>* sample_order_out,
               std::string* err, const SigBuildOptions* opts = nullptr);
bool sig_build_replay(CybufAux& cy, const std::vector<int32_t>& sample_order, SigBand* out, std::string* err);
