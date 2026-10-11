#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct NpzArray {
    std::string descr;
    std::vector<std::size_t> shape;
    std::vector<unsigned char> data;
};

// Parse one NPZ blob (zip archive) into named arrays.
bool npz_load(const unsigned char* data, std::size_t len, std::unordered_map<std::string, NpzArray>& out);

// Build a compressed NPZ archive (numpy savez_compressed compatible subset).
bool npz_save_compressed(
    const std::unordered_map<std::string, NpzArray>& arrs,
    std::vector<unsigned char>& out);

template <typename T>
const T* npz_as(const std::unordered_map<std::string, NpzArray>& arrs, const char* name, std::size_t& n) {
    const auto it = arrs.find(name);
    if (it == arrs.end()) {
        n = 0;
        return nullptr;
    }
    n = it->second.data.size() / sizeof(T);
    return reinterpret_cast<const T*>(it->second.data.data());
}
