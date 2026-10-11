#include "cybuf_pack.h"
#include "dict_build.h"
#include "inver_build.h"

#include <cstdlib>
#include <iostream>
#include <string>

static void usage() {
    std::cerr
        << "Usage:\n"
        << "  prep_cli --target TARGET.json --out-dir DIR/ [--num-perm N] [--skip-inver]\n";
}

int main(int argc, char** argv) {
    std::string target_path;
    std::string out_dir;
    uint32_t num_perm = 5;
    bool skip_inver = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--target" && i + 1 < argc) {
            target_path = argv[++i];
        } else if (arg == "--out-dir" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--num-perm" && i + 1 < argc) {
            num_perm = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--skip-inver") {
            skip_inver = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            usage();
            return 2;
        }
    }

    if (target_path.empty() || out_dir.empty()) {
        usage();
        return 2;
    }
    if (!out_dir.empty() && out_dir.back() != '/') {
        out_dir.push_back('/');
    }

    const std::string dict_path = out_dir + "dict.native.bin";
    const std::string inver_path = out_dir + "inver.native.bin";
    const std::string cybuf_path = out_dir + "cybuf.native.bin";

    DictBuildResult built;
    std::string err;
    if (!dict_build_from_target_json(target_path, &built, &err)) {
        std::cerr << "dict build failed: " << err << "\n";
        return 1;
    }
    if (!dict_write_native(dict_path, built.dict, &err)) {
        std::cerr << "dict write failed: " << err << "\n";
        return 1;
    }

    if (!skip_inver) {
        DenseInver inver;
        if (!inver_build_from_files(dict_path, target_path, &inver, &err)) {
            std::cerr << "inver build failed: " << err << "\n";
            return 1;
        }
        if (!inver_write_native(inver_path, inver, &err)) {
            std::cerr << "inver write failed: " << err << "\n";
            return 1;
        }
    }

    if (!cybuf_pack_from_files(dict_path, target_path, cybuf_path, num_perm, &err)) {
        std::cerr << "cybuf pack failed: " << err << "\n";
        return 1;
    }

    CybufAux aux;
    if (!cybuf_read_native(cybuf_path, &aux, &err)) {
        std::cerr << "cybuf read failed: " << err << "\n";
        return 1;
    }

    std::cout << "{\"dict\":\"" << dict_path << "\",\"cybuf\":\"" << cybuf_path << "\""
              << ",\"n_tokens\":" << built.dict.id_to_token.size()
              << ",\"n_cols\":" << aux.col_ids.size()
              << ",\"max_gram_id\":" << aux.max_gram_id
              << ",\"skip_inver\":" << (skip_inver ? "true" : "false") << "}\n";
    return 0;
}
