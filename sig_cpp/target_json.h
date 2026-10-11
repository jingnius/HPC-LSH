#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/* Target column: col_id + deduped token strings (identity_preprocess). */
struct TargetColumn {
    uint64_t col_id = 0;
    std::vector<std::string> tokens;
};

struct TargetReadOptions {
    bool keep_empty_columns = false;
};

/* Match Python list(dict.fromkeys(values)) — preserve first occurrence order. */
std::vector<std::string> identity_preprocess(const std::vector<std::string>& values);

bool target_read_json(
    const std::string& path,
    std::vector<TargetColumn>* out,
    std::string* err,
    const TargetReadOptions& opts = TargetReadOptions{});
