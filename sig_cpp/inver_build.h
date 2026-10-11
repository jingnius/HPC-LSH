#pragma once

#include <string>

#include "dict_io.h"
#include "inver_io.h"
#include "target_json.h"

bool inver_build_from_target(
    const NativeDict& dict,
    const std::vector<TargetColumn>& columns,
    DenseInver* out,
    std::string* err);

bool inver_build_from_files(
    const std::string& dict_path,
    const std::string& target_json_path,
    DenseInver* out,
    std::string* err);
