#pragma once

#include "hpc_partition_build.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct HpcPartitionV8Context;

struct HpcV8CkptFileHandle {
    std::string path;
    int write_fd = -1;
    int read_fd = -1;
    void* mmap_base = nullptr;
    std::size_t mmap_len = 0;
    uint64_t write_bytes = 0;

    ~HpcV8CkptFileHandle();
    void reset();
    bool open_write();
    bool append_i32(const int32_t* data, std::size_t n);
    bool mmap_read();
    const int32_t* span(uint64_t off, uint32_t len) const;
};

bool hpc_v8_stream_checkpoint_enabled();

void hpc_v8_stream_ckpt_init(HpcPartitionV8Context& ctx, const std::string& run_dir, int band);
void hpc_v8_stream_ckpt_finish_partition(HpcPartitionV8Context& ctx);
void hpc_v8_stream_ckpt_cleanup(HpcPartitionV8Context& ctx);

void hpc_v8_stream_ckpt_spill_oip(HpcPartitionV8Context& ctx, HpcPartitionNode& node);
void hpc_v8_stream_ckpt_spill_leaf_one_index(HpcPartitionV8Context& ctx, HpcPartitionNode& leaf);
void hpc_v8_stream_ckpt_trim_terminal_parent(HpcPartitionV8Context& ctx, HpcPartitionNode& parent);

bool hpc_v8_stream_ckpt_oip_view(
    const HpcPartitionV8Context& ctx,
    const HpcPartitionNode& node,
    std::vector<int32_t>& out);

bool hpc_v8_stream_ckpt_leaf_one_index_view(
    const HpcPartitionV8Context& ctx,
    const HpcPartitionNode& leaf,
    std::vector<int32_t>& out);
