#include "target_json.h"

#include "simdjson.h"

#include <string_view>
#include <unordered_set>

std::vector<std::string> identity_preprocess(const std::vector<std::string>& values) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    out.reserve(values.size());
    for (const std::string& v : values) {
        if (seen.insert(v).second) {
            out.push_back(v);
        }
    }
    return out;
}

bool target_read_json(
    const std::string& path,
    std::vector<TargetColumn>* out,
    std::string* err,
    const TargetReadOptions& opts) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    const simdjson::error_code load_err = parser.load(path).get(doc);
    if (load_err) {
        if (err) {
            *err = std::string("simdjson load failed: ") + simdjson::error_message(load_err);
        }
        return false;
    }

    simdjson::dom::object root;
    const simdjson::error_code obj_err = doc.get(root);
    if (obj_err) {
        if (err) {
            *err = "expected object";
        }
        return false;
    }

    out->clear();

    for (auto field : root) {
        const std::string key(field.key);
        uint64_t col_id = 0;
        try {
            col_id = static_cast<uint64_t>(std::stoull(key));
        } catch (const std::exception&) {
            if (err) {
                *err = "bad column id key: " + key;
            }
            return false;
        }

        simdjson::dom::array arr;
        const simdjson::error_code arr_err = field.value.get_array().get(arr);
        if (arr_err) {
            if (err) {
                *err = "expected token array for key " + key;
            }
            return false;
        }

        TargetColumn col;
        col.col_id = col_id;
        std::vector<std::string> raw;
        raw.reserve(arr.size());
        for (auto elem : arr) {
            std::string_view tok;
            const simdjson::error_code tok_err = elem.get_string().get(tok);
            if (tok_err) {
                if (err) {
                    *err = "bad token string for key " + key;
                }
                return false;
            }
            raw.emplace_back(tok);
        }
        col.tokens = identity_preprocess(raw);
        if (opts.keep_empty_columns || !col.tokens.empty()) {
            out->push_back(std::move(col));
        }
    }
    return true;
}
