#include "hpc_v8_compress_flags.h"

#include <cstdlib>
#include <cctype>
#include <string>

namespace {

bool env_enabled(const char* key, bool def_false) {
    const char* v = std::getenv(key);
    if (v == nullptr) {
        return def_false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "1" || s == "true" || s == "yes" || s == "y";
}

}  // namespace

bool hpc_v8_compress::split_union_set_enabled() {
    return env_enabled("HPC_V8_SPLIT_UNION_SET", false);
}
