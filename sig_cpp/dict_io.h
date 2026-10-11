#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct NativeDict {
    std::vector<std::string> id_to_token; /* index = token id */
};

bool dict_read_native(const std::string& path, NativeDict* out, std::string* err);
bool dict_write_native(const std::string& path, const NativeDict& dict, std::string* err);

void dict_build_lookup(const NativeDict& dict, std::unordered_map<std::string, uint32_t>* out);
bool dict_compare(const NativeDict& a, const NativeDict& b, std::string* err);
