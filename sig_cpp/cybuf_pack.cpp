#include "cybuf_pack.h"
#include "target_json.h"

#include "dict_build.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

bool cybuf_pack_from_columns(
    const NativeDict& dict,
    const std::vector<TargetColumn>& columns,
    const std::vector<uint32_t>& gram_counts,
    uint32_t num_perm,
    CybufAux* out,
    std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    if (dict.id_to_token.empty()) {
        if (err) {
            *err = "empty dictionary";
        }
        return false;
    }

    std::unordered_map<std::string, uint32_t> lookup;
    dict_build_lookup(dict, &lookup);

    out->num_perm = num_perm;
    out->max_gram_id = static_cast<uint32_t>(dict.id_to_token.size() - 1);
    out->col_ids.clear();
    out->col_grams.clear();
    out->col_ids.reserve(columns.size());
    out->col_grams.reserve(columns.size());

    uint32_t max_seen = 0;
    for (const TargetColumn& col : columns) {
        out->col_ids.push_back(col.col_id);
        std::vector<uint32_t> grams;
        const std::vector<std::string> prepped = identity_preprocess(col.tokens);
        grams.reserve(prepped.size());
        for (const std::string& tok : prepped) {
            const auto it = lookup.find(tok);
            if (it == lookup.end()) {
                continue;
            }
            grams.push_back(it->second);
            max_seen = std::max(max_seen, it->second);
        }
        out->col_grams.push_back(std::move(grams));
    }

    if (!columns.empty() && max_seen > out->max_gram_id) {
        out->max_gram_id = max_seen;
    }

    out->gram_counts.assign(static_cast<std::size_t>(out->max_gram_id) + 1u, 0u);
    if (gram_counts.size() == dict.id_to_token.size()) {
        const std::size_t n = std::min(out->gram_counts.size(), gram_counts.size());
        for (std::size_t i = 0; i < n; ++i) {
            out->gram_counts[i] = gram_counts[i];
        }
    } else {
        for (const TargetColumn& col : columns) {
            const std::vector<std::string> prepped = identity_preprocess(col.tokens);
            std::unordered_set<std::string> seen;
            for (const std::string& tok : prepped) {
                const auto it = lookup.find(tok);
                if (it == lookup.end() || !seen.insert(tok).second) {
                    continue;
                }
                if (it->second < out->gram_counts.size()) {
                    out->gram_counts[it->second] += 1u;
                }
            }
        }
    }
    return true;
}

bool cybuf_pack_from_files(
    const std::string& dict_path,
    const std::string& target_path,
    const std::string& out_path,
    uint32_t num_perm,
    std::string* err) {
    NativeDict dict;
    if (!dict_read_native(dict_path, &dict, err)) {
        return false;
    }

    DictBuildResult counts;
    if (!dict_build_from_target_json(target_path, &counts, err)) {
        return false;
    }
    if (!dict_compare(dict, counts.dict, err)) {
        if (err) {
            *err = "dict/token mismatch vs target recount: " + *err;
        }
        return false;
    }

    std::vector<TargetColumn> columns;
    TargetReadOptions opts;
    opts.keep_empty_columns = true;
    if (!target_read_json(target_path, &columns, err, opts)) {
        return false;
    }

    CybufAux aux;
    if (!cybuf_pack_from_columns(dict, columns, counts.gram_counts, num_perm, &aux, err)) {
        return false;
    }
    return cybuf_write_native(out_path, aux, err);
}
