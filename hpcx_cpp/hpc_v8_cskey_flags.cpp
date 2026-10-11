#include "hpc_v8_cskey_flags.h"

#include <cstdlib>
#include <string>

namespace hpc_v8_cskey {

namespace {

bool env_enabled(const char* key, bool def_true) {
    const char* v = std::getenv(key);
    if (v == nullptr) {
        return def_true;
    }
    std::string s(v);
    return !(s == "0" || s == "false" || s == "no");
}

}  // namespace

bool packed_cskey_enabled() {
    return env_enabled("HPC_V8_PACKED_CSKEY", false);
}

bool cskey_sorted_buckets() {
    return env_enabled("HPC_V8_CSKEY_SORTED", true);
}

}  // namespace hpc_v8_cskey
