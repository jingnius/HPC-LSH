#pragma once

#include <cstdint>
#include <vector>

// sig_zip_2_indexOne: compressed run-length sig -> hit positions (compactTree semantics).
std::vector<int> hpc_sig_zip_to_index_one(const int32_t* sig, int sig_len);

// cut_sig: keep first num_perm elements (LSH band slice).
std::vector<int32_t> hpc_cut_sig(const int32_t* sig, int sig_len, int num_perm);
