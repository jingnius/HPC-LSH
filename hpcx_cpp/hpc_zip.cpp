#include "hpc_zip.h"

#include <cstdint>
#include <cstring>
#include <fstream>

namespace {

bool read_u16(const unsigned char* p, uint16_t& out) {
    out = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    return true;
}

bool read_u32(const unsigned char* p, uint32_t& out) {
    out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    return true;
}

}  // namespace

bool hpc_zip_read_stored_member(
    const std::string& path,
    std::vector<unsigned char>& out,
    std::string* err) {
    out.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "open zip failed";
        }
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::size_t file_len = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<unsigned char> data(file_len);
    if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(file_len))) {
        if (err) {
            *err = "read zip failed";
        }
        return false;
    }
    if (file_len < 30 || data[0] != 0x50 || data[1] != 0x4b || data[2] != 0x03 || data[3] != 0x04) {
        if (err) {
            *err = "not a zip local header";
        }
        return false;
    }
    uint16_t method = 0;
    uint16_t name_len = 0;
    uint16_t extra_len = 0;
    uint32_t comp_size = 0;
    read_u16(data.data() + 8, method);
    read_u32(data.data() + 18, comp_size);
    read_u16(data.data() + 26, name_len);
    read_u16(data.data() + 28, extra_len);
    if (method != 0) {
        if (err) {
            *err = "only STORED zip supported";
        }
        return false;
    }
    const std::size_t data_off = 30 + name_len + extra_len;
    if (data_off + comp_size > file_len) {
        if (err) {
            *err = "zip payload out of range";
        }
        return false;
    }
    out.assign(data.begin() + static_cast<std::ptrdiff_t>(data_off),
               data.begin() + static_cast<std::ptrdiff_t>(data_off + comp_size));
    return true;
}
