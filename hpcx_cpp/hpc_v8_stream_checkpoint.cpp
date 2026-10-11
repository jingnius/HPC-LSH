#include "hpc_v8_stream_checkpoint.h"

#include "hpc_partition_bfs_v8.h"
#include "hpc_partition_children.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool env_enabled(const char* key) {
    const char* v = std::getenv(key);
    return v != nullptr && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
}

void profile_log(const char* phase, double value) {
    if (std::getenv("HPCX_BUILD_PROFILE") == nullptr) {
        return;
    }
    std::cerr << "[hpcx_v8_ckpt] " << phase << " " << value << "\n";
}

}  // namespace

HpcV8CkptFileHandle::~HpcV8CkptFileHandle() {
    reset();
}

void HpcV8CkptFileHandle::reset() {
    if (mmap_base != nullptr && mmap_base != MAP_FAILED) {
        ::munmap(mmap_base, mmap_len);
    }
    mmap_base = nullptr;
    mmap_len = 0;
    if (write_fd >= 0) {
        ::close(write_fd);
    }
    write_fd = -1;
    if (read_fd >= 0) {
        ::close(read_fd);
    }
    read_fd = -1;
    if (!path.empty()) {
        ::unlink(path.c_str());
        path.clear();
    }
}

bool HpcV8CkptFileHandle::open_write() {
    write_fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    return write_fd >= 0;
}

bool HpcV8CkptFileHandle::append_i32(const int32_t* data, std::size_t n) {
    if (write_fd < 0 || n == 0) {
        return false;
    }
    const std::size_t bytes = n * sizeof(int32_t);
    const ssize_t wrote = ::write(write_fd, data, bytes);
    if (wrote < 0 || static_cast<std::size_t>(wrote) != bytes) {
        return false;
    }
    write_bytes += static_cast<uint64_t>(bytes);
    return true;
}

bool HpcV8CkptFileHandle::mmap_read() {
    if (write_fd >= 0) {
        ::close(write_fd);
        write_fd = -1;
    }
    read_fd = ::open(path.c_str(), O_RDONLY);
    if (read_fd < 0) {
        return false;
    }
    struct stat st {};
    if (::fstat(read_fd, &st) != 0 || st.st_size <= 0) {
        return true;
    }
    mmap_len = static_cast<std::size_t>(st.st_size);
    mmap_base = ::mmap(nullptr, mmap_len, PROT_READ, MAP_PRIVATE, read_fd, 0);
    return mmap_base != MAP_FAILED;
}

const int32_t* HpcV8CkptFileHandle::span(uint64_t off, uint32_t len) const {
    if (mmap_base == nullptr || mmap_base == MAP_FAILED || len == 0) {
        return nullptr;
    }
    const std::size_t byte_off = static_cast<std::size_t>(off);
    const std::size_t bytes = static_cast<std::size_t>(len) * sizeof(int32_t);
    if (byte_off + bytes > mmap_len) {
        return nullptr;
    }
    return reinterpret_cast<const int32_t*>(static_cast<const char*>(mmap_base) + byte_off);
}

bool hpc_v8_stream_checkpoint_enabled() {
    return env_enabled("HPC_V8_STREAM_CHECKPOINT");
}

void hpc_v8_stream_ckpt_init(HpcPartitionV8Context& ctx, const std::string& run_dir, int band) {
    if (!hpc_v8_stream_checkpoint_enabled()) {
        return;
    }
    hpc_v8_stream_ckpt_cleanup(ctx);
    ctx.stream_ckpt_dir = run_dir + "/.hpcx_v8_ckpt_band" + std::to_string(band);
    std::error_code ec;
    std::filesystem::create_directories(ctx.stream_ckpt_dir, ec);

    ctx.stream_oip_ckpt = std::make_unique<HpcV8CkptFileHandle>();
    ctx.stream_oip_ckpt->path = ctx.stream_ckpt_dir + "/oip.bin";
    ctx.stream_leaf_ckpt = std::make_unique<HpcV8CkptFileHandle>();
    ctx.stream_leaf_ckpt->path = ctx.stream_ckpt_dir + "/leaf_one_index.bin";

    if (!ctx.stream_oip_ckpt->open_write() || !ctx.stream_leaf_ckpt->open_write()) {
        hpc_v8_stream_ckpt_cleanup(ctx);
        return;
    }
    ctx.stream_ckpt_active = true;
}

void hpc_v8_stream_ckpt_finish_partition(HpcPartitionV8Context& ctx) {
    if (!ctx.stream_ckpt_active) {
        return;
    }
    if (ctx.stream_oip_ckpt != nullptr) {
        ctx.stream_oip_ckpt->mmap_read();
        profile_log("oip_ckpt_bytes", static_cast<double>(ctx.stream_oip_ckpt->write_bytes));
    }
    if (ctx.stream_leaf_ckpt != nullptr) {
        ctx.stream_leaf_ckpt->mmap_read();
        profile_log("leaf_ckpt_bytes", static_cast<double>(ctx.stream_leaf_ckpt->write_bytes));
    }
    profile_log("oip_ckpt_nodes", static_cast<double>(ctx.stream_oip_ckpt_span.size()));
    profile_log("leaf_ckpt_nodes", static_cast<double>(ctx.stream_leaf_one_ckpt_span.size()));
}

void hpc_v8_stream_ckpt_cleanup(HpcPartitionV8Context& ctx) {
    ctx.stream_oip_ckpt_span.clear();
    ctx.stream_leaf_one_ckpt_span.clear();
    ctx.stream_ckpt_active = false;
    ctx.stream_ckpt_dir.clear();
    if (ctx.stream_oip_ckpt != nullptr) {
        ctx.stream_oip_ckpt->reset();
        ctx.stream_oip_ckpt.reset();
    }
    if (ctx.stream_leaf_ckpt != nullptr) {
        ctx.stream_leaf_ckpt->reset();
        ctx.stream_leaf_ckpt.reset();
    }
}

void hpc_v8_stream_ckpt_spill_oip(HpcPartitionV8Context& ctx, HpcPartitionNode& node) {
    if (!ctx.stream_ckpt_active || ctx.stream_oip_ckpt == nullptr || node.oip_one_index.empty()) {
        return;
    }
    const uint64_t off = ctx.stream_oip_ckpt->write_bytes;
    const uint32_t len = static_cast<uint32_t>(node.oip_one_index.size());
    if (!ctx.stream_oip_ckpt->append_i32(node.oip_one_index.data(), node.oip_one_index.size())) {
        return;
    }
    ctx.stream_oip_ckpt_span[&node] = {off, len};
    node.oip_one_index.clear();
    node.oip_one_index.shrink_to_fit();
}

void hpc_v8_stream_ckpt_spill_leaf_one_index(HpcPartitionV8Context& ctx, HpcPartitionNode& leaf) {
    if (!ctx.stream_ckpt_active || ctx.stream_leaf_ckpt == nullptr || leaf.one_index.empty()) {
        return;
    }
    const uint64_t off = ctx.stream_leaf_ckpt->write_bytes;
    const uint32_t len = static_cast<uint32_t>(leaf.one_index.size());
    if (!ctx.stream_leaf_ckpt->append_i32(leaf.one_index.data(), leaf.one_index.size())) {
        return;
    }
    ctx.stream_leaf_one_ckpt_span[&leaf] = {off, len};
    leaf.one_index.clear();
    leaf.one_index.shrink_to_fit();
}

void hpc_v8_stream_ckpt_trim_terminal_parent(HpcPartitionV8Context& ctx, HpcPartitionNode& parent) {
    if (!ctx.stream_ckpt_active || !hpc_node_has_children(parent)) {
        return;
    }
    bool all_mgn = true;
    hpc_node_for_each_child_ptr(parent, [&](HpcPartitionNode* child) {
        if (child == nullptr || !child->is_mgn_leaf) {
            all_mgn = false;
        }
    });
    if (!all_mgn) {
        return;
    }
    parent.one_index_partition.clear();
    (void)ctx;
}

bool hpc_v8_stream_ckpt_oip_view(
    const HpcPartitionV8Context& ctx,
    const HpcPartitionNode& node,
    std::vector<int32_t>& out) {
    out.clear();
    if (!ctx.stream_ckpt_active || ctx.stream_oip_ckpt == nullptr) {
        return false;
    }
    const auto it = ctx.stream_oip_ckpt_span.find(&node);
    if (it == ctx.stream_oip_ckpt_span.end()) {
        return false;
    }
    const auto [off, len] = it->second;
    const int32_t* ptr = ctx.stream_oip_ckpt->span(off, len);
    if (ptr == nullptr) {
        return false;
    }
    out.assign(ptr, ptr + len);
    return true;
}

bool hpc_v8_stream_ckpt_leaf_one_index_view(
    const HpcPartitionV8Context& ctx,
    const HpcPartitionNode& leaf,
    std::vector<int32_t>& out) {
    out.clear();
    if (!ctx.stream_ckpt_active || ctx.stream_leaf_ckpt == nullptr) {
        return false;
    }
    const auto it = ctx.stream_leaf_one_ckpt_span.find(&leaf);
    if (it == ctx.stream_leaf_one_ckpt_span.end()) {
        return false;
    }
    const auto [off, len] = it->second;
    const int32_t* ptr = ctx.stream_leaf_ckpt->span(off, len);
    if (ptr == nullptr) {
        return false;
    }
    out.assign(ptr, ptr + len);
    return true;
}
