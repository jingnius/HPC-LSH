#include "query_encode.h"

#include "dict_io.h"

#include "native_format.h"

#include <algorithm>
#include <climits>
#include <fstream>
#include <unordered_set>

namespace {

bool read_exact(std::ifstream& in, void* buf, std::size_t n, std::string* err) {
    in.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    if (!in) {
        if (err) {
            *err = "unexpected EOF";
        }
        return false;
    }
    return true;
}

bool sample_order_read_native(const std::string& path, std::vector<int32_t>* out, std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open sample order: " + path;
        }
        return false;
    }
    NativeSampleOrderHeader hdr{};
    if (!read_exact(in, &hdr, sizeof(hdr), err)) {
        return false;
    }
    if (hdr.magic != NATIVE_SORD_MAGIC || hdr.version != NATIVE_FORMAT_VERSION) {
        if (err) {
            *err = "bad SORD magic/version";
        }
        return false;
    }
    out->resize(static_cast<std::size_t>(hdr.n_samples));
    if (hdr.n_samples > 0 && !read_exact(in, out->data(), out->size() * sizeof(int32_t), err)) {
        return false;
    }
    return true;
}

void build_code_lookup(const std::vector<int32_t>& sample_order, BandEncodeMeta* meta) {
    meta->code_keys.clear();
    meta->code_offsets.clear();
    meta->code_indices.clear();
    if (sample_order.empty()) {
        return;
    }

    std::vector<std::pair<int32_t, int32_t>> pairs;
    pairs.reserve(sample_order.size());
    for (int32_t index = 0; index < static_cast<int32_t>(sample_order.size()); ++index) {
        pairs.emplace_back(sample_order[static_cast<std::size_t>(index)], index);
    }
    std::sort(pairs.begin(), pairs.end());

    meta->code_keys.reserve(pairs.size() / 4 + 1);
    meta->code_offsets.reserve(pairs.size() / 4 + 2);
    meta->code_indices.reserve(pairs.size());

    for (const auto& pair : pairs) {
        if (meta->code_keys.empty() || pair.first != meta->code_keys.back()) {
            meta->code_keys.push_back(pair.first);
            meta->code_offsets.push_back(static_cast<int32_t>(meta->code_indices.size()));
        }
        meta->code_indices.push_back(pair.second);
    }
    meta->code_offsets.push_back(static_cast<int32_t>(meta->code_indices.size()));
}

const int32_t* code_indices_for_key(
    const BandEncodeMeta& meta,
    int32_t code,
    int& out_len) {
    out_len = 0;
    if (meta.code_keys.empty()) {
        return nullptr;
    }
    const auto it = std::lower_bound(meta.code_keys.begin(), meta.code_keys.end(), code);
    if (it == meta.code_keys.end() || *it != code) {
        return nullptr;
    }
    const int off = static_cast<int>(it - meta.code_keys.begin());
    out_len = meta.code_offsets[static_cast<std::size_t>(off + 1)] -
              meta.code_offsets[static_cast<std::size_t>(off)];
    return meta.code_indices.data() + meta.code_offsets[static_cast<std::size_t>(off)];
}

bool parse_json_string(const std::string& text, std::size_t* pos, std::string* out) {
    if (*pos >= text.size() || text[*pos] != '"') {
        return false;
    }
    ++(*pos);
    std::string s;
    while (*pos < text.size()) {
        const char c = text[*pos];
        if (c == '"') {
            ++(*pos);
            *out = std::move(s);
            return true;
        }
        if (c == '\\' && *pos + 1 < text.size()) {
            s.push_back(text[*pos + 1]);
            *pos += 2;
            continue;
        }
        s.push_back(c);
        ++(*pos);
    }
    return false;
}

}  // namespace

std::vector<std::string> query_identity_preprocess(const std::vector<std::string>& values) {
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

QueryPreprocessed query_full_preprocess(
    const std::vector<std::string>& tokens,
    const TokenToId& token_to_id) {
    QueryPreprocessed out;
    out.tokens = query_identity_preprocess(tokens);
    out.cids.reserve(out.tokens.size());
    for (const std::string& ele : out.tokens) {
        const auto tok_it = token_to_id.find(ele);
        if (tok_it == token_to_id.end()) {
            continue;
        }
        out.cids.push_back(static_cast<int32_t>(tok_it->second));
    }
    std::sort(out.cids.begin(), out.cids.end());
    out.cids.erase(std::unique(out.cids.begin(), out.cids.end()), out.cids.end());
    return out;
}

BandEncodeMeta band_encode_meta_load(int band, const std::string& refs_dir, std::string* err) {
    BandEncodeMeta meta;
    meta.band = band;
    std::string path = refs_dir;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += "sample_order_band" + std::to_string(band) + ".native.bin";
    if (!sample_order_read_native(path, &meta.sample_order, err)) {
        return meta;
    }
    build_code_lookup(meta.sample_order, &meta);
    return meta;
}

std::vector<int32_t> encode_query_for_band_from_cids(
    const std::vector<int32_t>& query_cids,
    const BandEncodeMeta& meta) {
    static thread_local std::vector<uint32_t> hit_stamp;
    static thread_local uint32_t hit_gen = 1;
    if (++hit_gen == 0) {
        std::fill(hit_stamp.begin(), hit_stamp.end(), 0);
        hit_gen = 1;
    }
    const int so_len = static_cast<int>(meta.sample_order.size());
    if (static_cast<int>(hit_stamp.size()) < so_len) {
        hit_stamp.resize(static_cast<std::size_t>(so_len));
    }

    std::vector<int32_t> out;
    out.reserve(4096);
    for (int32_t code : query_cids) {
        int idx_len = 0;
        const int32_t* idxs = code_indices_for_key(meta, code, idx_len);
        for (int j = 0; j < idx_len; ++j) {
            const int32_t idx = idxs[j];
            if (hit_stamp[static_cast<std::size_t>(idx)] == hit_gen) {
                continue;
            }
            hit_stamp[static_cast<std::size_t>(idx)] = hit_gen;
            out.push_back(idx);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int32_t> encode_query_for_band(
    const std::vector<std::string>& prepped_tokens,
    const TokenToId& token_to_id,
    const BandEncodeMeta& meta) {
    const QueryPreprocessed prepped = query_full_preprocess(prepped_tokens, token_to_id);
    return encode_query_for_band_from_cids(prepped.cids, meta);
}

void merge_sorted_band_candidates(
    const std::vector<std::vector<int32_t>>& per_band,
    std::vector<int32_t>& merged_out) {
    merged_out.clear();
    if (per_band.empty()) {
        return;
    }

    std::vector<std::size_t> pos(per_band.size(), 0);
    std::size_t remaining = 0;
    for (const auto& cids : per_band) {
        remaining += cids.size();
    }
    merged_out.reserve(remaining / 4 + 64);

    while (true) {
        int32_t best = INT32_MAX;
        for (std::size_t bi = 0; bi < per_band.size(); ++bi) {
            if (pos[bi] >= per_band[bi].size()) {
                continue;
            }
            const int32_t v = per_band[bi][pos[bi]];
            if (v < best) {
                best = v;
            }
        }
        if (best == INT32_MAX) {
            break;
        }
        merged_out.push_back(best);
        for (std::size_t bi = 0; bi < per_band.size(); ++bi) {
            while (pos[bi] < per_band[bi].size() && per_band[bi][pos[bi]] == best) {
                ++pos[bi];
            }
        }
    }
}

bool token_to_id_load(const std::string& dict_path, TokenToId* out, std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    NativeDict dict;
    if (!dict_read_native(dict_path, &dict, err)) {
        return false;
    }
    dict_build_lookup(dict, out);
    return true;
}

std::vector<std::string> parse_json_string_array_field(const std::string& json_line, const char* key) {
    std::vector<std::string> tokens;
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t key_pos = json_line.find(needle);
    if (key_pos == std::string::npos) {
        return tokens;
    }
    std::size_t pos = json_line.find('[', key_pos);
    if (pos == std::string::npos) {
        return tokens;
    }
    ++pos;
    while (pos < json_line.size()) {
        while (pos < json_line.size() && (json_line[pos] == ' ' || json_line[pos] == ',')) {
            ++pos;
        }
        if (pos >= json_line.size() || json_line[pos] == ']') {
            break;
        }
        std::string tok;
        if (!parse_json_string(json_line, &pos, &tok)) {
            break;
        }
        tokens.push_back(std::move(tok));
    }
    return tokens;
}
