#pragma once

#include "dict_io.h"
#include "target_json.h"

#include <cstdint>
#include <string>
#include <vector>

struct DictBuildResult {
    NativeDict dict;
    std::vector<uint32_t> gram_counts;
};

/* Replicate jing_minhash_opt.Dictionary.insert_dictionary. */
bool dict_build_from_columns(
    const std::vector<TargetColumn>& columns,
    DictBuildResult* out,
    std::string* err);

bool dict_build_from_target_json(
    const std::string& target_path,
    DictBuildResult* out,
    std::string* err);

