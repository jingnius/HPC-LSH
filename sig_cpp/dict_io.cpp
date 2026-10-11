#include "dict_io.h"

#include "native_format.h"

#include <fstream>

static bool read_exact(std::ifstream& in, void* buf, std::size_t n, std::string* err) {
    in.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    if (!in) {
        if (err) {
            *err = "unexpected EOF";
        }
        return false;
    }
    return true;
}

bool dict_read_native(const std::string& path, NativeDict* out, std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open: " + path;
        }
        return false;
    }
    NativeDictHeader hdr{};
    if (!read_exact(in, &hdr, sizeof(hdr), err)) {
        return false;
    }
    if (hdr.magic != NATIVE_DICT_MAGIC || hdr.version != NATIVE_FORMAT_VERSION) {
        if (err) {
            *err = "bad DICT magic/version";
        }
        return false;
    }
    out->id_to_token.resize(hdr.n_tokens);
    for (uint32_t i = 0; i < hdr.n_tokens; ++i) {
        uint32_t len = 0;
        if (!read_exact(in, &len, sizeof(len), err)) {
            return false;
        }
        std::string s(len, '\0');
        if (len > 0 && !read_exact(in, s.data(), len, err)) {
            return false;
        }
        out->id_to_token[i] = std::move(s);
    }
    return true;
}

bool dict_write_native(const std::string& path, const NativeDict& dict, std::string* err) {
    NativeDictHeader hdr{};
    hdr.magic = NATIVE_DICT_MAGIC;
    hdr.version = NATIVE_FORMAT_VERSION;
    hdr.n_tokens = static_cast<uint32_t>(dict.id_to_token.size());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) {
            *err = "cannot write: " + path;
        }
        return false;
    }
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    for (const std::string& tok : dict.id_to_token) {
        const uint32_t len = static_cast<uint32_t>(tok.size());
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len > 0) {
            out.write(tok.data(), static_cast<std::streamsize>(len));
        }
    }
    return static_cast<bool>(out);
}

void dict_build_lookup(const NativeDict& dict, std::unordered_map<std::string, uint32_t>* out) {
    out->clear();
    out->reserve(dict.id_to_token.size());
    for (uint32_t i = 0; i < dict.id_to_token.size(); ++i) {
        (*out)[dict.id_to_token[i]] = i;
    }
}


bool dict_compare(const NativeDict& a, const NativeDict& b, std::string* err) {
    if (a.id_to_token != b.id_to_token) {
        if (err) *err = "dict token mismatch";
        return false;
    }
    return true;
}
