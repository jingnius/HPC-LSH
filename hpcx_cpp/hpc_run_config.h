#pragma once

#include "hpc_partition_build.h"

#include <string>

struct HpcRunBuildConfig {
    HpcPartitionBuildConfig partition;
    int route_r = 12;
};

bool hpc_load_partition_config(const std::string& run_dir, HpcPartitionBuildConfig* cfg, std::string* err);
bool hpc_load_run_build_config(const std::string& run_dir, HpcRunBuildConfig* cfg, std::string* err);
