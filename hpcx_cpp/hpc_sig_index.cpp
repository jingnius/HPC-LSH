#include "hpc_sig_index.h"

#include <cstdlib>

std::vector<int> hpc_sig_zip_to_index_one(const int32_t* sig, int sig_len) {
    std::vector<int> out;
    if (sig == nullptr || sig_len <= 0) {
        return out;
    }
    int64_t count = 0;
    out.reserve(static_cast<std::size_t>(sig_len));
    for (int i = 0; i < sig_len; ++i) {
        const int64_t ele = sig[i];
        if (ele < 0) {
            count += -ele;
            continue;
        }
        count += 1;
        out.push_back(static_cast<int>(count - 1));
    }
    return out;
}

std::vector<int32_t> hpc_cut_sig(const int32_t* sig, int sig_len, int num_perm) {
    if (sig == nullptr || sig_len <= 0 || num_perm <= 0) {
        return {};
    }
    const int n = sig_len < num_perm ? sig_len : num_perm;
    return std::vector<int32_t>(sig, sig + n);
}
