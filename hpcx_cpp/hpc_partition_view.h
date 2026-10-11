#pragma once

#include <cstddef>
#include <vector>

struct HpcFlatPartView {
    int root_node = 0;
    int n_nodes = 0;
    const int* one_index_pool = nullptr;
    int one_index_pool_len = 0;
    const int* one_off = nullptr;
    const int* one_len = nullptr;
    const int* oip_pool = nullptr;
    int oip_pool_len = 0;
    const int* oip_off = nullptr;
    const int* oip_num_parts = nullptr;
    const int* route_mask = nullptr;
    int route_mask_len = 0;
    const int* route_child_ids = nullptr;
    int route_child_ids_len = 0;
    const int* route_child_off = nullptr;
    int route_child_off_len = 0;
    const int* route_ent_off = nullptr;
    const int* route_ent_len = nullptr;
    const int* node_mgn_key = nullptr;
    const unsigned char* node_is_leaf = nullptr;
};

bool partition_view_attach(const unsigned char* data, std::size_t len, HpcFlatPartView& out);

std::vector<int> partition_view_filter_sig(
    const HpcFlatPartView& p,
    int node,
    const int* sig,
    int sig_len);

void partition_view_filter_sig_into(
    const HpcFlatPartView& p,
    int node,
    const int* sig,
    int sig_len,
    std::vector<int>& out);

int partition_view_route_mask_for_sig(
    const HpcFlatPartView& p,
    int node,
    const int* sig,
    int sig_len);

std::vector<int> partition_view_route_children(
    const HpcFlatPartView& p,
    int node,
    const std::vector<int>& sig_f);

bool partition_view_is_leaf(const HpcFlatPartView& p, int node);
int partition_view_mgn_key(const HpcFlatPartView& p, int node);
