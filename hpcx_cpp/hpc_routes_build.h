#pragma once

#include "hpc_partition_build.h"

// Precompute _route tables on partition tree (compactTree.build_hpc_query_routes).
void hpc_build_query_routes(HpcPartitionNode& root, int route_r);
