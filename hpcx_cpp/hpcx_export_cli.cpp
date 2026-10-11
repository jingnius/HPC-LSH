#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "hpc_hpcx.h"
#include "hpcx_write.h"

int main(int argc, char** argv) {
    std::string in_path;
    std::string out_path;
    std::string json_out;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--from-hpcx" && i + 1 < argc) {
            in_path = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cerr << "usage: hpcx_export_cli --from-hpcx PATH --out PATH [--json-out PATH]\n";
            return 0;
        }
    }

    if (in_path.empty() || out_path.empty()) {
        std::cerr << "usage: hpcx_export_cli --from-hpcx PATH --out PATH [--json-out PATH]\n";
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    std::unique_ptr<HpcxIndex> idx = hpcx_load(in_path, &err);
    if (!idx) {
        std::cerr << "load failed: " << err << "\n";
        return 1;
    }
    const double load_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    HpcxWriteStats wstats;
    if (!hpcx_write_from_index(*idx, out_path, &wstats, &err)) {
        std::cerr << "write failed: " << err << "\n";
        return 1;
    }

    std::ostringstream js;
    js << "{\n";
    js << "  \"backend\": \"hpcx_export_cpp\",\n";
    js << "  \"from_hpcx\": \"" << in_path << "\",\n";
    js << "  \"out_hpcx\": \"" << out_path << "\",\n";
    js << "  \"band\": " << idx->band << ",\n";
    js << "  \"load_s\": " << load_s << ",\n";
    js << "  \"write_s\": " << wstats.write_s << ",\n";
    js << "  \"file_bytes\": " << wstats.file_bytes << ",\n";
    js << "  \"file_gb\": " << (wstats.file_bytes / (1024.0 * 1024.0 * 1024.0)) << ",\n";
    js << "  \"n_views\": " << wstats.n_views << ",\n";
    js << "  \"n_pools\": " << wstats.n_pools << ",\n";
    js << "  \"n_compact\": " << wstats.n_compact << "\n";
    js << "}\n";

    const std::string rep = js.str();
    if (!json_out.empty()) {
        std::ofstream out(json_out);
        out << rep;
    }
    std::cout << rep;
    return 0;
}
