#include "hpc_ct_ops.h"

#include "hpc_index_one_store.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace {

int32_t map_get(const std::unordered_map<int64_t, int32_t>& m, int64_t key) {
    const auto it = m.find(key);
    return it != m.end() ? it->second : 0;
}

}  // namespace

std::vector<int32_t> hpc_adaptive_first1_boundaries(
    const std::vector<int64_t>& col_ids,
    const std::unordered_map<int64_t, int32_t>& first1_map,
    int k) {
    if (col_ids.empty() || k <= 0) {
        return {};
    }
    std::map<int32_t, int> hist;
    for (const int64_t cid : col_ids) {
        hist[map_get(first1_map, cid)] += 1;
    }
    const double threshold = static_cast<double>(col_ids.size()) / static_cast<double>(k);
    std::vector<int32_t> sorted_pos;
    sorted_pos.reserve(hist.size());
    for (const auto& kv : hist) {
        sorted_pos.push_back(kv.first);
    }
    std::vector<int32_t> boundaries;
    int cum = 0;
    int segments_done = 0;
    for (const int32_t pos : sorted_pos) {
        cum += hist[pos];
        while (segments_done < k - 1 && cum >= static_cast<int>(threshold)) {
            boundaries.push_back(pos);
            cum = 0;
            segments_done += 1;
        }
    }
    if (!sorted_pos.empty()) {
        while (static_cast<int>(boundaries.size()) < k - 1) {
            boundaries.push_back(sorted_pos.back());
        }
    }
    const int keep = std::max(k - 1, 0);
    if (static_cast<int>(boundaries.size()) > keep) {
        boundaries.resize(static_cast<std::size_t>(keep));
    }
    return boundaries;
}

std::vector<int32_t> hpc_adaptive_first1_boundaries_nth(
    const std::vector<int64_t>& col_ids,
    const HpcIndexOneStore& store,
    int nth,
    int k) {
    if (col_ids.empty() || k <= 0 || nth <= 0) {
        return {};
    }
    std::map<int32_t, int> hist;
    for (const int64_t cid : col_ids) {
        hist[store.nth_hit(cid, nth)] += 1;
    }
    const double threshold = static_cast<double>(col_ids.size()) / static_cast<double>(k);
    std::vector<int32_t> sorted_pos;
    sorted_pos.reserve(hist.size());
    for (const auto& kv : hist) {
        sorted_pos.push_back(kv.first);
    }
    std::vector<int32_t> boundaries;
    int cum = 0;
    int segments_done = 0;
    for (const int32_t pos : sorted_pos) {
        cum += hist[pos];
        while (segments_done < k - 1 && cum >= static_cast<int>(threshold)) {
            boundaries.push_back(pos);
            cum = 0;
            segments_done += 1;
        }
    }
    if (!sorted_pos.empty()) {
        while (static_cast<int>(boundaries.size()) < k - 1) {
            boundaries.push_back(sorted_pos.back());
        }
    }
    const int keep = std::max(k - 1, 0);
    if (static_cast<int>(boundaries.size()) > keep) {
        boundaries.resize(static_cast<std::size_t>(keep));
    }
    return boundaries;
}

HpcOneIndexPartition hpc_assign_oneindex_adaptive(
    const std::vector<int32_t>& one_index_list,
    const std::vector<int32_t>& boundaries,
    int k) {
    HpcOneIndexPartition result;
    for (int i = 0; i < k; ++i) {
        result[i] = {};
    }
    for (const int32_t idx : one_index_list) {
        int seg = k - 1;
        for (std::size_t i = 0; i < boundaries.size(); ++i) {
            if (idx <= boundaries[i]) {
                seg = static_cast<int>(i);
                break;
            }
        }
        result[seg].insert(idx);
    }
    return result;
}

std::vector<int32_t> hpc_active_hits_in_part(
    const std::vector<int32_t>& ic_hits,
    const std::unordered_set<int32_t>& part_index_set) {
    std::vector<int32_t> out;
    out.reserve(ic_hits.size());
    for (const int32_t h : ic_hits) {
        if (part_index_set.find(h) != part_index_set.end()) {
            out.push_back(h);
        }
    }
    return out;
}

std::string hpc_format_py_list_key(const std::vector<int>& bits) {
    std::ostringstream os;
    os << "[";
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << bits[i];
    }
    os << "]";
    return os.str();
}

std::vector<int> hpc_csp_cskey_bits_1index_zipfast_gai(
    const std::vector<int32_t>& active,
    const std::vector<int32_t>& one_index,
    const HpcOneIndexPartition& one_index_partition) {
    (void)one_index;
    std::vector<int> bits;
    std::vector<int32_t> sig = active;
    const int n_parts = static_cast<int>(one_index_partition.size());
    bits.reserve(static_cast<std::size_t>(n_parts));
    for (int i = 0; i < n_parts; ++i) {
        const auto pit = one_index_partition.find(i);
        if (pit == one_index_partition.end()) {
            bits.push_back(0);
            continue;
        }
        const auto& part = pit->second;
        bool matched = false;
        for (std::size_t k = 0; k < sig.size(); ++k) {
            const int32_t s = sig[k];
            if (part.find(s) != part.end()) {
                bits.push_back(1);
                matched = true;
                sig.erase(sig.begin(), sig.begin() + static_cast<std::ptrdiff_t>(k + 1));
                break;
            }
        }
        if (!matched) {
            bits.push_back(0);
        }
    }
    return bits;
}

HpcPartCSKey hpc_cskey_pack_bits(const std::vector<int>& bits) {
    HpcPartCSKey key = 0;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) {
            key |= (HpcPartCSKey(1) << i);
        }
    }
    return key;
}

std::string hpc_cskey_format_packed(HpcPartCSKey key, int n_parts) {
    std::vector<int> bits(static_cast<std::size_t>(std::max(n_parts, 0)), 0);
    for (int i = 0; i < n_parts; ++i) {
        if ((key >> i) & 1u) {
            bits[static_cast<std::size_t>(i)] = 1;
        }
    }
    return hpc_format_py_list_key(bits);
}

std::vector<int> hpc_cskey_active_segments(HpcPartCSKey key, int n_parts) {
    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(n_parts));
    for (int i = 0; i < n_parts; ++i) {
        if ((key >> i) & 1u) {
            out.push_back(i);
        }
    }
    return out;
}

std::string hpc_csp_cskey_1index_zipfast_gai(
    const std::vector<int32_t>& active,
    const std::vector<int32_t>& one_index,
    const HpcOneIndexPartition& one_index_partition) {
    const std::vector<int> bits =
        hpc_csp_cskey_bits_1index_zipfast_gai(active, one_index, one_index_partition);
    return hpc_format_py_list_key(bits);
}

HpcPartCSKey hpc_csp_cskey_packed_1index_zipfast_gai(
    const std::vector<int32_t>& active,
    const std::vector<int32_t>& one_index,
    const HpcOneIndexPartition& one_index_partition) {
    const std::vector<int> bits =
        hpc_csp_cskey_bits_1index_zipfast_gai(active, one_index, one_index_partition);
    return hpc_cskey_pack_bits(bits);
}
