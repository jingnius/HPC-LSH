#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "hpc_hpcx.h"
#include "hpc_rss.h"

static std::vector<int> parse_json_int_array(const std::string& text) {
    std::vector<int> out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '-' || (text[i] >= '0' && text[i] <= '9')) {
            std::size_t j = i + 1;
            while (j < text.size() && (text[j] == '-' || (text[j] >= '0' && text[j] <= '9'))) {
                ++j;
            }
            out.push_back(std::stoi(text.substr(i, j - i)));
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

static std::vector<std::vector<int>> load_encoded_queries(const std::string& path, int n_queries) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open query file");
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::vector<std::vector<int>> out;
    std::size_t pos = 0;
    while (pos < text.size() && static_cast<int>(out.size()) < n_queries) {
        const std::size_t lb = text.find('[', pos);
        if (lb == std::string::npos) {
            break;
        }
        const std::size_t rb = text.find(']', lb);
        if (rb == std::string::npos) {
            break;
        }
        out.push_back(parse_json_int_array(text.substr(lb, rb - lb + 1)));
        pos = rb + 1;
    }
    return out;
}

int main(int argc, char** argv) {
    std::string hpcx_path;
    std::string query_file;
    std::string json_out;
    int n_queries = 5;
    int min_hits = 5;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hpcx-path" && i + 1 < argc) {
            hpcx_path = argv[++i];
        } else if (arg == "--query-file" && i + 1 < argc) {
            query_file = argv[++i];
        } else if (arg == "--n-queries" && i + 1 < argc) {
            n_queries = std::atoi(argv[++i]);
        } else if (arg == "--min-hits" && i + 1 < argc) {
            min_hits = std::atoi(argv[++i]);
        } else if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        }
    }
    if (hpcx_path.empty() || query_file.empty()) {
        std::cerr << "usage: hpcx_bench_cli --hpcx-path PATH --query-file JSON [--n-queries N] [--min-hits R] [--json-out PATH]\n";
        return 2;
    }

    const std::size_t rss_before = vmrss_kb();
    const auto t_load0 = std::chrono::steady_clock::now();
    std::string err;
    std::unique_ptr<HpcxIndex> idx = hpcx_load(hpcx_path, &err);
    if (!idx) {
        std::cerr << "load failed: " << err << "\n";
        return 1;
    }
    const double load_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_load0).count();
    const std::size_t rss_after = vmrss_kb();
    const std::size_t rss_mmap = rss_kb_for_mapped_file(hpcx_path);

    std::vector<std::vector<int>> queries;
    try {
        queries = load_encoded_queries(query_file, n_queries);
    } catch (const std::exception& ex) {
        std::cerr << "query load failed: " << ex.what() << "\n";
        return 1;
    }

    std::vector<int> counts(queries.size(), 0);
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t qi = 0; qi < queries.size(); ++qi) {
        const auto& enc = queries[qi];
        std::vector<int32_t> cids;
        hpcx_search_encoded(*idx, enc.data(), static_cast<int>(enc.size()), min_hits, cids);
        counts[qi] = static_cast<int>(cids.size());
    }
    const double search_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const double rss_before_gb = rss_before / (1024.0 * 1024.0);
    const double rss_after_gb = rss_after / (1024.0 * 1024.0);
    const double rss_delta_gb = (rss_after > rss_before ? rss_after - rss_before : 0) / (1024.0 * 1024.0);
    const double rss_mmap_gb = rss_mmap / (1024.0 * 1024.0);

    std::ostringstream js;
    js << "{\n";
    js << "  \"backend\": \"hpcx_cpp\",\n";
    js << "  \"n_queries\": " << counts.size() << ",\n";
    js << "  \"load_s\": " << load_s << ",\n";
    js << "  \"rss_before_gb\": " << rss_before_gb << ",\n";
    js << "  \"rss_after_load_gb\": " << rss_after_gb << ",\n";
    js << "  \"rss_delta_gb\": " << rss_delta_gb << ",\n";
    js << "  \"rss_mmap_gb\": " << rss_mmap_gb << ",\n";
    js << "  \"search_s\": " << search_s << ",\n";
    js << "  \"per_query_s\": " << (counts.empty() ? 0.0 : search_s / counts.size()) << ",\n";
    js << "  \"candidate_counts\": [";
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (i) {
            js << ", ";
        }
        js << counts[i];
    }
    js << "]\n}\n";

    const std::string rep = js.str();
    if (!json_out.empty()) {
        std::ofstream out(json_out);
        out << rep;
    }
    std::cout << rep;
    return 0;
}
