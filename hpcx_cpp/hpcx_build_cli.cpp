#include "hpc_build.h"
#include "hpc_native_build.h"
#include "hpcx_write.h"

#include <cstdlib>
#include <iostream>
#include <string>

static void usage() {
    std::cerr
        << "Usage:\n"
        << "  hpcx_build_cli --from-zip IN.zip --out OUT.hpcx [--band N]\n"
        << "  hpcx_build_cli --from-sig SIGB.bin --run-dir RUN_DIR --out OUT.hpcx [--band N]\n";
}

int main(int argc, char** argv) {
    std::string zip_path;
    std::string sig_path;
    std::string run_dir;
    std::string out_path;
    int band = 0;
    bool from_sig = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--from-zip" && i + 1 < argc) {
            zip_path = argv[++i];
        } else if (arg == "--from-sig" && i + 1 < argc) {
            sig_path = argv[++i];
            from_sig = true;
        } else if (arg == "--run-dir" && i + 1 < argc) {
            run_dir = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--band" && i + 1 < argc) {
            band = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            usage();
            return 2;
        }
    }

    if (out_path.empty() || (from_sig ? (sig_path.empty() || run_dir.empty()) : zip_path.empty())) {
        usage();
        return 2;
    }

    HpcxWriteStats stats;
    std::string err;
    const bool ok = from_sig ? hpcx_build_from_sig(sig_path, run_dir, band, out_path, &stats, &err)
                             : hpcx_build_from_zip(zip_path, out_path, band, &stats, &err);
    if (!ok) {
        std::cerr << "build failed: " << err << "\n";
        return 1;
    }

    std::cout << "{"
              << "\"path\":\"" << out_path << "\","
              << "\"source\":\"" << (from_sig ? "sig" : "zip") << "\","
              << "\"file_bytes\":" << stats.file_bytes << ","
              << "\"n_views\":" << stats.n_views << ","
              << "\"n_pools\":" << stats.n_pools << ","
              << "\"n_compact\":" << stats.n_compact << ","
              << "\"build_s\":" << stats.compute_s << ","
              << "\"write_s\":" << stats.write_s
              << "}\n";
    return 0;
}
