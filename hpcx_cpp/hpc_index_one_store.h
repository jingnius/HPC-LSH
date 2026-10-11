#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HpcIndexOneStore {
    std::vector<int64_t> col_ids;
    std::vector<uint32_t> hit_off;
    std::vector<int32_t> hit_pool;

    void clear();

    std::size_t size() const { return col_ids.size(); }

    bool empty() const { return col_ids.empty(); }

    bool build_from_sigb(const std::string& sig_native_path, std::string* err);

    bool find_hits(int64_t col_id, const int32_t** hits, int* len) const;

    std::vector<int32_t> hits_copy(int64_t col_id) const;

    int32_t nth_hit(int64_t col_id, int nth) const;
};
