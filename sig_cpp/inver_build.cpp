#include "inver_build.h"

#include <unordered_map>

bool inver_build_from_target(
    const NativeDict& dict,
    const std::vector<TargetColumn>& columns,
    DenseInver* out,
    std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    std::unordered_map<std::string, uint32_t> lookup;
    dict_build_lookup(dict, &lookup);

    const uint32_t max_gram_id =
        dict.id_to_token.empty() ? 0u : static_cast<uint32_t>(dict.id_to_token.size() - 1);

    /* DenseInverMutable stores internal column index (0..n_cols-1), not external col_id. */
    std::vector<std::vector<uint32_t>> lists(static_cast<std::size_t>(max_gram_id) + 1u);
    for (uint32_t col_idx = 0; col_idx < columns.size(); ++col_idx) {
        const TargetColumn& col = columns[col_idx];
        for (const std::string& tok : col.tokens) {
            const auto it = lookup.find(tok);
            if (it == lookup.end()) {
                if (err) {
                    *err = "unknown token in target: " + tok.substr(0, 80);
                }
                return false;
            }
            lists[it->second].push_back(col_idx);
        }
    }

    out->max_gram_id = max_gram_id;
    out->offsets.assign(static_cast<std::size_t>(max_gram_id) + 2u, 0);
    out->col_ids.clear();
    uint64_t off = 0;
    for (uint32_t gid = 0; gid <= max_gram_id; ++gid) {
        out->offsets[gid] = off;
        for (uint32_t c : lists[gid]) {
            out->col_ids.push_back(c);
            ++off;
        }
    }
    out->offsets[max_gram_id + 1u] = off;
    return true;
}

bool inver_build_from_files(
    const std::string& dict_path,
    const std::string& target_json_path,
    DenseInver* out,
    std::string* err) {
    NativeDict dict;
    if (!dict_read_native(dict_path, &dict, err)) {
        return false;
    }
    std::vector<TargetColumn> columns;
    TargetReadOptions opts;
    opts.keep_empty_columns = true;
    if (!target_read_json(target_json_path, &columns, err, opts)) {
        return false;
    }
    return inver_build_from_target(dict, columns, out, err);
}
