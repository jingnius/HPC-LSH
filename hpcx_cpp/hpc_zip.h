#pragma once

#include <cstddef>
#include <string>
#include <vector>

bool hpc_zip_read_stored_member(
    const std::string& path,
    std::vector<unsigned char>& out,
    std::string* err);
