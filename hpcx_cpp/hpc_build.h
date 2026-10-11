#pragma once

#include <cstdint>
#include <string>

struct HpcxWriteStats;

// Build HPCX directly from band zip (pickle tree inside STORED zip).
bool hpcx_build_from_zip(
    const std::string& zip_path,
    const std::string& out_hpcx_path,
    int band,
    HpcxWriteStats* stats,
    std::string* err);
