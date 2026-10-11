#include <cstring>
#include "cybuf_pack.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

static void usage() {
    std::cerr
        << "Usage:\n"
        << "  cybuf_pack_cli --dict DICT.bin --target TARGET.json --out CYBF.bin [--num-perm N]\n"
        << "  cybuf_pack_cli --compare A.bin B.bin\n";
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
    std::string dict_path;
    std::string target_path;
    std::string out_path;
    std::string compare_a;
    std::string compare_b;
    uint32_t num_perm = 5;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dict" && i + 1 < argc) {
            dict_path = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            target_path = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--num-perm" && i + 1 < argc) {
            num_perm = static_cast<uint32_t>(std::atoi(argv[++i]));
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
        CybufAux a;
        CybufAux b;
        std::string err;
        if (!cybuf_read_native(compare_a, &a, &err)) {
            std::cerr << "read failed: " << err << "\n";
            return 1;
        }
        if (!cybuf_read_native(compare_b, &b, &err)) {
            std::cerr << "read failed: " << err << "\n";
            return 1;
        }
        if (!cybuf_compare(a, b, &err) && !files_equal(compare_a, compare_b, &err)) {
            std::cerr << "compare failed: " << err << "\n";
            return 1;
        }
        std::cout << "{\"match\":true,\"n_cols\":" << a.col_ids.size()
                  << ",\"max_gram_id\":" << a.max_gram_id << "}\n";
        return 0;
    }

    if (dict_path.empty() || target_path.empty() || out_path.empty()) {
        usage();
        return 2;
    }

    std::string err;
    if (!cybuf_pack_from_files(dict_path, target_path, out_path, num_perm, &err)) {
        std::cerr << "pack failed: " << err << "\n";
        return 1;
    }
    CybufAux aux;
    if (!cybuf_read_native(out_path, &aux, &err)) {
        std::cerr << "read back failed: " << err << "\n";
        return 1;
    }
    std::cout << "{\"path\":\"" << out_path << "\",\"n_cols\":" << aux.col_ids.size()
              << ",\"max_gram_id\":" << aux.max_gram_id << "}\n";
    return 0;
}
