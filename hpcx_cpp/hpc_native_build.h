#pragma once

#include <string>

struct HpcxWriteStats;

// Build HPCX directly from native SIGB without the legacy monolithic Python dict load.
// Uses C++ SIGB reader + streaming partition build, then existing zip->hpcx writer.
bool hpcx_build_from_sig_native(
    const std::string& sig_native_path,
    const std::string& run_dir,
    int band,
    const std::string& out_hpcx_path,
    HpcxWriteStats* stats,
    std::string* err);

// CLI alias used by hpcx_build_cli (--from-sig).
inline bool hpcx_build_from_sig(
    const std::string& sig_native_path,
    const std::string& run_dir,
    int band,
    const std::string& out_hpcx_path,
    HpcxWriteStats* stats,
    std::string* err) {
    return hpcx_build_from_sig_native(sig_native_path, run_dir, band, out_hpcx_path, stats, err);
}

