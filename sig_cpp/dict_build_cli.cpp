#include "dict_build.h"
#include <cstring>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

static void usage() {
    std::cerr
        << "Usage:\n"
        << "  dict_build_cli --target TARGET.json --out DICT.bin\n"
        << "  dict_build_cli --compare A.bin B.bin\n";
}

static bool files_equal(const std::string& a_path, const std::string& b_path, std::string* err) {
    std::ifstream a(a_path, std::ios::binary);
    std::ifstream b(b_path, std::ios::binary);
    if (!a || !b) {
        if (err) {
            *err = "cannot open compare files";
        }
        return false;
    }
    char ca[8192];
    char cb[8192];
    while (a && b) {
        a.read(ca, sizeof(ca));
        b.read(cb, sizeof(cb));
        const std::streamsize na = a.gcount();
        const std::streamsize nb = b.gcount();
        if (na != nb) {
            if (err) {
                *err = "file size mismatch";
            }
            return false;
        }
        if (na > 0 && std::memcmp(ca, cb, static_cast<std::size_t>(na)) != 0) {
            if (err) {
                *err = "byte mismatch";
            }
            return false;
        }
    }
    return a.eof() && b.eof();
}

int main(int argc, char** argv) {
    std::string target_path;
    std::string out_path;
    std::string compare_a;
    std::string compare_b;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--target" && i + 1 < argc) {
            target_path = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--compare" && i + 2 < argc) {
            compare_a = argv[++i];
            compare_b = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            usage();
            return 2;
        }
    }

    if (!compare_a.empty()) {
        NativeDict a;
        NativeDict b;
        std::string err;
        if (!dict_read_native(compare_a, &a, &err)) {
            std::cerr << "read failed: " << err << "\n";
            return 1;
        }
        if (!dict_read_native(compare_b, &b, &err)) {
            std::cerr << "read failed: " << err << "\n";
            return 1;
        }
        if (!dict_compare(a, b, &err) && !files_equal(compare_a, compare_b, &err)) {
            std::cerr << "compare failed: " << err << "\n";
            return 1;
        }
        std::cout << "{\"match\":true,\"n_tokens\":" << a.id_to_token.size() << "}\n";
        return 0;
    }

    if (target_path.empty() || out_path.empty()) {
        usage();
        return 2;
    }

    DictBuildResult built;
    std::string err;
    if (!dict_build_from_target_json(target_path, &built, &err)) {
        std::cerr << "build failed: " << err << "\n";
        return 1;
    }
    if (!dict_write_native(out_path, built.dict, &err)) {
        std::cerr << "write failed: " << err << "\n";
        return 1;
    }
    std::cout << "{\"path\":\"" << out_path << "\",\"n_tokens\":" << built.dict.id_to_token.size()
              << "}\n";
    return 0;
}
