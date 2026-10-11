#include "hpc_index_one_store.h"

#include "hpc_sig_index.h"
#include "hpc_sig_io.h"

#include <algorithm>

void HpcIndexOneStore::clear() {
    col_ids.clear();
    hit_off.clear();
    hit_pool.clear();
}

bool HpcIndexOneStore::build_from_sigb(const std::string& sig_native_path, std::string* err) {
    clear();
    HpcSigBandMeta meta{};
    if (!hpc_sigb_read_header(sig_native_path, &meta, err)) {
        return false;
    }

    col_ids.reserve(static_cast<std::size_t>(meta.n_cols));
    std::vector<uint32_t> hit_lens;
    hit_lens.reserve(static_cast<std::size_t>(meta.n_cols));

    if (!hpc_sigb_for_each_column(
            sig_native_path,
            [&](const HpcColumnSig& col) {
                col_ids.push_back(static_cast<int64_t>(col.col_id));
                hit_lens.push_back(static_cast<uint32_t>(std::max(col.hit_count, 0)));
                return true;
            },
            err)) {
        return false;
    }

    struct RowRef {
        int64_t col_id = 0;
        uint32_t hit_len = 0;
        std::size_t src_index = 0;
    };
    std::vector<RowRef> order(col_ids.size());
    for (std::size_t i = 0; i < col_ids.size(); ++i) {
        order[i] = RowRef{col_ids[i], hit_lens[i], i};
    }
    std::sort(order.begin(), order.end(), [](const RowRef& a, const RowRef& b) {
        return a.col_id < b.col_id;
    });

    hit_off.assign(order.size() + 1, 0);
    std::size_t total_hits = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        total_hits += order[i].hit_len;
        hit_off[i + 1] = static_cast<uint32_t>(total_hits);
    }
    hit_pool.resize(total_hits);

    std::vector<uint32_t> write_pos(order.size(), 0);
    for (std::size_t i = 0; i < order.size(); ++i) {
        write_pos[order[i].src_index] = hit_off[i];
    }

    std::size_t row_i = 0;
    if (!hpc_sigb_for_each_column(
            sig_native_path,
            [&](const HpcColumnSig& col) {
                const std::vector<int> decoded = hpc_sig_zip_to_index_one(
                    col.sig.data(), static_cast<int>(col.sig.size()));
                const uint32_t dst = write_pos[row_i++];
                if (!decoded.empty()) {
                    std::copy(
                        decoded.begin(),
                        decoded.end(),
                        hit_pool.begin() + static_cast<std::ptrdiff_t>(dst));
                }
                return true;
            },
            err)) {
        return false;
    }

    col_ids.resize(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        col_ids[i] = order[i].col_id;
    }
    hit_lens.clear();
    hit_lens.shrink_to_fit();
    order.clear();
    order.shrink_to_fit();
    return true;
}

bool HpcIndexOneStore::find_hits(int64_t col_id, const int32_t** hits, int* len) const {
    if (hits == nullptr || len == nullptr || col_ids.empty()) {
        return false;
    }
    const auto it = std::lower_bound(col_ids.begin(), col_ids.end(), col_id);
    if (it == col_ids.end() || *it != col_id) {
        *hits = nullptr;
        *len = 0;
        return false;
    }
    const std::size_t idx = static_cast<std::size_t>(it - col_ids.begin());
    *hits = hit_pool.data() + hit_off[idx];
    *len = static_cast<int>(hit_off[idx + 1] - hit_off[idx]);
    return true;
}

std::vector<int32_t> HpcIndexOneStore::hits_copy(int64_t col_id) const {
    const int32_t* hits = nullptr;
    int len = 0;
    if (!find_hits(col_id, &hits, &len) || hits == nullptr || len <= 0) {
        return {};
    }
    return std::vector<int32_t>(hits, hits + len);
}

int32_t HpcIndexOneStore::nth_hit(int64_t col_id, int nth) const {
    if (nth <= 0) {
        return 0;
    }
    const int32_t* hits = nullptr;
    int len = 0;
    if (!find_hits(col_id, &hits, &len) || hits == nullptr || len <= 0) {
        return 0;
    }
    if (len >= nth) {
        return hits[nth - 1];
    }
    return hits[len - 1];
}
