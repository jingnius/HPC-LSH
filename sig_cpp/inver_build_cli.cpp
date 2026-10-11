#include "inver_build.h"

#include <cstdlib>
#include <iostream>
#include <string>

static void usage() {
    std::cerr
        << "Usage:\n"
        << "  inver_build_cli --dict DICT.bin --target TARGET.json --out INVR.bin\n"
        << "  inver_build_cli --compare A.bin B.bin\n";
}

int main(int argc, char** argv) {
    std::string dict_path;
    std::string target_path;
    std::string out_path;
    std::string compare_a;
    std::string compare_b;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dict" && i + 1 < argc) {
            dict_path = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
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
        DenseInver a;
        DenseInver b;
        std::string err;
        if (!inver_read_native(compare_a, &a, &err)) {
            std::cerr << "read failed: " << err << "\n";
            return 1;
        }
        if (!inver_read_native(compare_b, &b, &err)) {
            std::cerr << "read failed: " << err << "\n";
            return 1;
        }
        if (!inver_compare(a, b, &err)) {
            std::cerr << "compare failed: " << err << "\n";
            std::cerr << "a lists=" << a.offsets.size() << " entries=" << a.col_ids.size() << "\n";
            std::cerr << "b lists=" << b.offsets.size() << " entries=" << b.col_ids.size() << "\n";
            return 1;
        }
        std::cout << "{\"match\":true,\"n_entries\":" << a.col_ids.size()
                  << ",\"max_gram_id\":" << a.max_gram_id << "}\n";
        return 0;
    }

    if (dict_path.empty() || target_path.empty() || out_path.empty()) {
        usage();
        return 2;
    }

    DenseInver inver;
    std::string err;
    if (!inver_build_from_files(dict_path, target_path, &inver, &err)) {
        std::cerr << "build failed: " << err << "\n";
        return 1;
    }
    if (!inver_write_native(out_path, inver, &err)) {
        std::cerr << "write failed: " << err << "\n";
        return 1;
    }
    std::cout << "{\"path\":\"" << out_path << "\",\"max_gram_id\":" << inver.max_gram_id
              << ",\"n_entries\":" << inver.col_ids.size() << "}\n";
    return 0;
}
