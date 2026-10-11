#pragma once

#include "cybuf_core.h"
#include "dict_io.h"
#include "target_json.h"

#include <cstdint>
#include <string>
#include <vector>

bool cybuf_pack_from_columns(
    const NativeDict& dict,
    const std::vector<TargetColumn>& columns,
    const std::vector<uint32_t>& gram_counts,
    uint32_t num_perm,
    CybufAux* out,
    std::string* err);

bool cybuf_pack_from_files(
    const std::string& dict_path,
    const std::string& target_path,
    const std::string& out_path,
    uint32_t num_perm,
    std::string* err);
