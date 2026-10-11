#pragma once

#include "hpc_ct_ops.h"
#include "hpc_partition_build.h"
#include "hpc_v8_cskey_flags.h"

#include <functional>
#include <utility>
#include <vector>

inline bool hpc_node_has_children(const HpcPartitionNode& node) {
    return !node.next_partition.empty() || !node.next_partition_pk.empty();
}

inline void hpc_node_clear_children(HpcPartitionNode& node) {
    node.next_partition.clear();
    node.next_partition_pk.clear();
}

inline void hpc_node_for_each_child(
    HpcPartitionNode& node,
    const std::function<void(HpcPartitionNode&)>& fn) {
    for (auto& kv : node.next_partition) {
        if (kv.second) {
            fn(*kv.second);
        }
    }
    for (auto& kv : node.next_partition_pk) {
        if (kv.second) {
            fn(*kv.second);
        }
    }
}

inline void hpc_node_for_each_child_ptr(
    HpcPartitionNode& node,
    const std::function<void(HpcPartitionNode*)>& fn) {
    for (auto& kv : node.next_partition) {
        fn(kv.second.get());
    }
    for (auto& kv : node.next_partition_pk) {
        fn(kv.second.get());
    }
}

inline HpcPartitionNode* hpc_node_find_child_by_node_id(HpcPartitionNode& node, int node_id) {
    for (auto& kv : node.next_partition) {
        if (kv.second && kv.second->node_id == node_id) {
            return kv.second.get();
        }
    }
    for (auto& kv : node.next_partition_pk) {
        if (kv.second && kv.second->node_id == node_id) {
            return kv.second.get();
        }
    }
    return nullptr;
}

inline std::vector<HpcPartCSKey> hpc_node_packed_child_keys(const HpcPartitionNode& node) {
    std::vector<HpcPartCSKey> keys;
    keys.reserve(node.next_partition_pk.size());
    for (const auto& kv : node.next_partition_pk) {
        keys.push_back(kv.first);
    }
    return keys;
}
