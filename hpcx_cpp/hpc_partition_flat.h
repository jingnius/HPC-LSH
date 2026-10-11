#pragma once

#include <cstdint>
#include <vector>

struct HpcFlatPart {
    int root_node = 0;
    std::vector<int> one_index_pool;
    std::vector<int> one_off;
    std::vector<int> one_len;
    std::vector<int> oip_pool;
    std::vector<int> oip_off;
    std::vector<int> oip_num_parts;
    std::vector<int> route_mask;
    std::vector<int> route_child_ids;
    std::vector<int> route_child_off;
    std::vector<int> route_ent_off;
    std::vector<int> route_ent_len;
    std::vector<int> node_mgn_key;
    std::vector<unsigned char> node_is_leaf;
};

std::vector<unsigned char> partition_flat_encode(const HpcFlatPart& p);
HpcFlatPart partition_flat_decode(const unsigned char* data, std::size_t len);

std::vector<int> partition_flat_filter_sig(
    const HpcFlatPart& p,
    int node,
    const std::vector<int>& sig);

std::vector<int> partition_flat_route_children(
    const HpcFlatPart& p,
    int node,
    const std::vector<int>& sig_f);

bool partition_flat_is_leaf(const HpcFlatPart& p, int node);
int partition_flat_mgn_key(const HpcFlatPart& p, int node);
const int* partition_flat_one_index_ptr(const HpcFlatPart& p, int node, int& out_len);
