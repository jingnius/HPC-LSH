#include "cybuf_core.h"

#include <cstdlib>
#include <iostream>
#include <string>

static void usage() {
    std::cerr
        << "Usage:\n"
        << "  sig_replay_cli --cybuf CYBF.bin --sample-order SORD.bin --out SIGB.bin\n"
        << "  sig_replay_cli --compare REF.bin CAND.bin\n";
}

int main(int argc, char** argv) {
    std::string cybuf_path;
    std::string order_path;
    std::string out_path;
    std::string compare_a;
    std::string compare_b;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cybuf" && i + 1 < argc) {
            cybuf_path = argv[++i];
        } else if (arg == "--sample-order" && i + 1 < argc) {
            order_path = argv[++i];
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
        SigBand a;
        SigBand b;
        std::string err;
        if (!sig_read_native(compare_a, &a, &err)) {
            std::cerr << "read failed: " << err << "\n";
            return 1;
        }
        if (!sig_read_native(compare_b, &b, &err)) {
            std::cerr << "read failed: " << err << "\n";
            return 1;
        }
        if (!sig_compare(a, b, &err)) {
            std::cerr << "compare failed: " << err << "\n";
            return 1;
        }
        std::cout << "{\"match\":true,\"n_cols\":" << a.columns.size() << "}\n";
        return 0;
    }

    if (cybuf_path.empty() || order_path.empty() || out_path.empty()) {
        usage();
        return 2;
    }

    CybufAux aux;
    std::vector<int32_t> order;
    SigBand out;
    std::string err;
    if (!cybuf_read_native(cybuf_path, &aux, &err)) {
        std::cerr << "cybuf read failed: " << err << "\n";
        return 1;
    }
    if (!sample_order_read_native(order_path, &order, &err)) {
        std::cerr << "sample order read failed: " << err << "\n";
        return 1;
    }
    if (!sig_build_replay(aux, order, &out, &err)) {
        std::cerr << "replay failed: " << err << "\n";
        return 1;
    }
    if (!sig_write_native(out_path, out, &err)) {
        std::cerr << "write failed: " << err << "\n";
        return 1;
    }
    std::cout << "{\"path\":\"" << out_path << "\",\"n_cols\":" << out.columns.size()
              << ",\"n_samples\":" << order.size() << "}\n";
    return 0;
}
