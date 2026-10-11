#pragma once

#include <string>

// In-process Python bridge (no subprocess): streaming SIGB HPC zip export.
bool hpc_py_export_hpc_zip_streaming(
    const std::string& sig_native_path,
    const std::string& run_dir,
    int band,
    const std::string& out_zip_path,
    const std::string& index_one_bin,
    const std::string& partition_json,
    std::string* err);
