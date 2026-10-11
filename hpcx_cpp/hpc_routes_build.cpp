#include "hpc_routes_build.h"

#include "hpc_ct_ops.h"
#include "hpc_partition_build.h"
#include "hpc_partition_children.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <vector>

namespace {

bool single_route_enabled() {
    const char* v = std::getenv("HPC_SINGLE_ROUTE");
    if (v == nullptr || *v == '\0') {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return !(s == "0" || s == "false" || s == "no");
}

std::set<int> parse_route_key_segments(const std::string& key_str) {
    std::set<int> out;
    const std::size_t lb = key_str.find('[');
    const std::size_t rb = key_str.rfind(']');
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb) {
        return out;
    }
    const std::string inner = key_str.substr(lb + 1, rb - lb - 1);
    int idx = 0;
    std::size_t pos = 0;
    while (pos < inner.size()) {
        while (pos < inner.size() && inner[pos] == ' ') {
            ++pos;
        }
        std::size_t end = inner.find(',', pos);
        if (end == std::string::npos) {
            end = inner.size();
        }
        const int val = std::stoi(inner.substr(pos, end - pos));
        if (val == 1) {
            out.insert(idx);
        }
        ++idx;
        pos = end + 1;
    }
    return out;
}

std::vector<std::vector<int>> paper_multikey_from_indexes(const std::vector<int>& base_indexes, int r) {
    std::vector<int> uniq = base_indexes;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    const int m = static_cast<int>(uniq.size());
    if (m == 0) {
        return {{}};
    }
    const int max_k = (r > 0) ? std::min(r, m) : m;
    std::vector<std::vector<int>> variants;
    for (int subset_size = 1; subset_size <= max_k; ++subset_size) {
        std::vector<int> idx(subset_size);
        std::iota(idx.begin(), idx.end(), 0);
        while (true) {
            std::vector<int> subset;
            subset.reserve(static_cast<std::size_t>(subset_size));
            for (int j = 0; j < subset_size; ++j) {
                subset.push_back(uniq[static_cast<std::size_t>(idx[static_cast<std::size_t>(j)])]);
            }
            variants.push_back(std::move(subset));
            int pos = subset_size - 1;
            while (pos >= 0 && idx[static_cast<std::size_t>(pos)] == m - subset_size + pos) {
                --pos;
            }
            if (pos < 0) {
                break;
            }
            ++idx[static_cast<std::size_t>(pos)];
            for (int j = pos + 1; j < subset_size; ++j) {
                idx[static_cast<std::size_t>(j)] = idx[static_cast<std::size_t>(j - 1)] + 1;
            }
        }
    }
    return variants;
}

bool query_reaches_child_segments(
    const std::set<int>& target_segs,
    const std::vector<int>& query_segs,
    int m,
    int r) {
    std::set<int> Q(query_segs.begin(), query_segs.end());
    std::set<int> T = target_segs;
    if (static_cast<int>(T.size()) > r) {
        return false;
    }
    std::set<int> all;
    for (int i = 0; i < m; ++i) {
        all.insert(i);
    }
    std::set<int> zeros;
    for (int z : all) {
        if (Q.find(z) == Q.end()) {
            zeros.insert(z);
        }
    }
    std::vector<std::vector<int>> variants;
    if (single_route_enabled()) {
        // One compressed key: the query's own hit segments. No subset keys.
        variants.push_back(std::vector<int>(Q.begin(), Q.end()));
    } else {
        variants = paper_multikey_from_indexes(std::vector<int>(Q.begin(), Q.end()), r);
    }
    for (const auto& qvar : variants) {
        std::set<int> qset(qvar.begin(), qvar.end());
        bool subset_q = true;
        for (int q : qset) {
            if (Q.find(q) == Q.end()) {
                subset_q = false;
                break;
            }
        }
        if (!subset_q) {
            continue;
        }
        bool subset_t = true;
        for (int q : qset) {
            if (T.find(q) == T.end()) {
                subset_t = false;
                break;
            }
        }
        if (!subset_t) {
            continue;
        }
        bool extra_in_zeros = true;
        for (int t : T) {
            if (qset.find(t) != qset.end()) {
                continue;
            }
            if (zeros.find(t) == zeros.end()) {
                extra_in_zeros = false;
                break;
            }
        }
        if (extra_in_zeros) {
            return true;
        }
    }
    return false;
}

void build_routes_recursive(HpcPartitionNode& part, int route_r) {
    if (part.next_partition.empty()) {
        return;
    }
    const int m = part.split_k > 0
        ? part.split_k
        : static_cast<int>(part.segment_boundaries.size()) + 1;
    if (m <= 0 && !part.one_index_partition.empty()) {
        const int m_oip = static_cast<int>(part.one_index_partition.size());
        if (m_oip > 0) {
            // legacy fallback for debug dumps only
        }
    }
    const int m_eff = m > 0 ? m : static_cast<int>(part.one_index_partition.size());
    const int m_use = m_eff;
    (void)m_use;
    if (m <= 0) {
        for (auto& kv : part.next_partition) {
            if (kv.second->mgn_key < 0) {
                build_routes_recursive(*kv.second, route_r);
            }
        }
        return;
    }

    std::vector<std::string> child_keys;
    child_keys.reserve(part.next_partition.size());
    for (const auto& kv : part.next_partition) {
        child_keys.push_back(kv.first);
    }
    std::map<std::string, std::set<int>> parsed;
    for (const std::string& ck : child_keys) {
        parsed[ck] = parse_route_key_segments(ck);
    }

    const int max_mask = 1 << m;
    for (int mask = 1; mask < max_mask; ++mask) {
        std::vector<int> Q;
        for (int i = 0; i < m; ++i) {
            if ((mask >> i) & 1) {
                Q.push_back(i);
            }
        }
        std::vector<int> hits;
        for (const auto& ck : child_keys) {
            if (query_reaches_child_segments(parsed[ck], Q, m, route_r)) {
                hits.push_back(part.next_partition[ck]->node_id);
            }
        }
        if (!hits.empty()) {
            part.route[mask] = std::move(hits);
        }
    }

    for (auto& kv : part.next_partition) {
        if (kv.second->mgn_key < 0) {
            build_routes_recursive(*kv.second, route_r);
        }
    }
}

}  // namespace

void hpc_build_query_routes(HpcPartitionNode& root, int route_r) {
    if (single_route_enabled()) {
        std::fprintf(
            stderr,
            "[hpc_single_route] each level uses one compressed key; multi-keys disabled route_r=%d\n",
            route_r);
    }
    build_routes_recursive(root, route_r);
}
