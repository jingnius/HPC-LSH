#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "hpc_hpcx.h"
#include "hpc_rss.h"
#include "query_encode.h"

namespace {

class WorkerPool {
public:
    explicit WorkerPool(int workers) : workers_(std::max(1, workers)) {
        threads_.reserve(static_cast<std::size_t>(workers_));
        for (int i = 0; i < workers_; ++i) {
            threads_.emplace_back([this]() { thread_main(); });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& th : threads_) {
            if (th.joinable()) {
                th.join();
            }
        }
    }

    int workers() const { return workers_; }

    void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) {
        if (n == 0) {
            return;
        }
        struct Batch {
            std::atomic<std::size_t> remaining{0};
            std::mutex done_mtx;
            std::condition_variable done_cv;
            std::exception_ptr error;
        };
        auto batch = std::make_shared<Batch>();
        batch->remaining.store(n, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (std::size_t i = 0; i < n; ++i) {
                tasks_.push([batch, fn, i]() {
                    try {
                        fn(i);
                    } catch (...) {
                        std::lock_guard<std::mutex> err_lk(batch->done_mtx);
                        if (!batch->error) {
                            batch->error = std::current_exception();
                        }
                    }
                    if (batch->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::lock_guard<std::mutex> done_lk(batch->done_mtx);
                        batch->done_cv.notify_one();
                    }
                });
            }
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> done_lk(batch->done_mtx);
        batch->done_cv.wait(done_lk, [&]() { return batch->remaining.load(std::memory_order_acquire) == 0; });
        if (batch->error) {
            std::rethrow_exception(batch->error);
        }
    }

private:
    void thread_main() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [&]() { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            if (task) {
                task();
            }
        }
    }

    const int workers_;
    std::vector<std::thread> threads_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    bool stop_ = false;
};

std::vector<int> parse_bands_spec(const std::string& spec) {
    std::vector<int> out;
    std::string part;
    for (char c : spec) {
        if (c == ',') {
            if (!part.empty()) {
                if (part.find('-') != std::string::npos) {
                    const std::size_t dash = part.find('-');
                    const int a = std::stoi(part.substr(0, dash));
                    const int b = std::stoi(part.substr(dash + 1));
                    for (int x = a; x <= b; ++x) {
                        out.push_back(x);
                    }
                } else {
                    out.push_back(std::stoi(part));
                }
                part.clear();
            }
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            part.push_back(c);
        }
    }
    if (!part.empty()) {
        if (part.find('-') != std::string::npos) {
            const std::size_t dash = part.find('-');
            const int a = std::stoi(part.substr(0, dash));
            const int b = std::stoi(part.substr(dash + 1));
            for (int x = a; x <= b; ++x) {
                out.push_back(x);
            }
        } else {
            out.push_back(std::stoi(part));
        }
    }
    return out;
}

void encode_query_parallel(
    WorkerPool& pool,
    const QueryPreprocessed& prepped,
    const std::vector<BandEncodeMeta>& band_metas,
    std::vector<std::vector<int32_t>>& enc_by_band) {
    enc_by_band.resize(band_metas.size());
    pool.parallel_for(band_metas.size(), [&](std::size_t bi) {
        enc_by_band[bi] = encode_query_for_band_from_cids(prepped.cids, band_metas[bi]);
    });
}

void search_one_query_parallel(
    WorkerPool& pool,
    const std::vector<std::unique_ptr<HpcxIndex>>& indices,
    const std::vector<std::vector<int32_t>>& enc_by_band,
    int min_hits,
    std::vector<int32_t>& merged_out,
    double& search_wall_s) {
    merged_out.clear();
    if (indices.empty()) {
        search_wall_s = 0.0;
        return;
    }

    std::vector<std::vector<int32_t>> per_band(indices.size());
    const auto t0 = std::chrono::steady_clock::now();
    pool.parallel_for(indices.size(), [&](std::size_t bi) {
        if (bi >= enc_by_band.size()) {
            return;
        }
        const auto& enc = enc_by_band[bi];
        std::vector<int32_t> cids;
        hpcx_search_encoded(
            *indices[bi],
            enc.data(),
            static_cast<int>(enc.size()),
            min_hits,
            cids);
        per_band[bi] = std::move(cids);
    });
    search_wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    merge_sorted_band_candidates(per_band, merged_out);
}

}  // namespace

static int run_serve(
    const std::string& hpcx_dir,
    const std::string& refs_dir,
    const std::string& dict_path,
    const std::vector<int>& bands,
    int workers,
    int min_hits) {
    const auto load_t0 = std::chrono::steady_clock::now();

    TokenToId token_to_id;
    std::string err;
    if (!token_to_id_load(dict_path, &token_to_id, &err)) {
        std::cerr << "dict load failed: " << err << "\n";
        return 1;
    }

    std::vector<BandEncodeMeta> band_metas;
    band_metas.reserve(bands.size());
    for (int band : bands) {
        BandEncodeMeta meta = band_encode_meta_load(band, refs_dir, &err);
        if (meta.sample_order.empty()) {
            std::cerr << "sample order load failed band " << band << ": " << err << "\n";
            return 1;
        }
        band_metas.push_back(std::move(meta));
    }

    std::vector<std::unique_ptr<HpcxIndex>> indices;
    indices.reserve(bands.size());
    std::vector<std::string> hpcx_paths;
    hpcx_paths.reserve(bands.size());
    for (int band : bands) {
        const std::string path = hpcx_dir + "/band" + std::to_string(band) + ".hpcx";
        auto idx = hpcx_load(path, &err);
        if (!idx) {
            std::cerr << "hpcx load failed band " << band << ": " << err << "\n";
            return 1;
        }
        hpcx_paths.push_back(path);
        indices.push_back(std::move(idx));
    }

    // Later bands allocate enough heap to evict earlier bands' file pages.
    // Fault every index again only after the full set is loaded, before ready.
    for (const auto& idx : indices) {
        hpcx_prefault_resident(*idx);
    }
    for (const BandEncodeMeta& meta : band_metas) {
        touch_mapped_pages(
            reinterpret_cast<const unsigned char*>(meta.sample_order.data()),
            meta.sample_order.size() * sizeof(int32_t));
        touch_mapped_pages(
            reinterpret_cast<const unsigned char*>(meta.code_keys.data()),
            meta.code_keys.size() * sizeof(int32_t));
        touch_mapped_pages(
            reinterpret_cast<const unsigned char*>(meta.code_offsets.data()),
            meta.code_offsets.size() * sizeof(int32_t));
        touch_mapped_pages(
            reinterpret_cast<const unsigned char*>(meta.code_indices.data()),
            meta.code_indices.size() * sizeof(int32_t));
    }

    double rss_mmap_gb = 0.0;
    for (const std::string& path : hpcx_paths) {
        rss_mmap_gb += rss_kb_for_mapped_file(path) / (1024.0 * 1024.0);
    }
    const double rss_vm_gb = vmrss_kb() / (1024.0 * 1024.0);

    WorkerPool pool(workers);

    const double load_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - load_t0).count();

    std::cout << "{\"status\":\"ready\",\"bands\":" << bands.size() << ",\"load_s\":" << load_s
              << ",\"rss_mmap_gb\":" << rss_mmap_gb << ",\"rss_vm_gb\":" << rss_vm_gb
              << ",\"prefault\":\"full_mmap+heap\",\"workers\":" << workers
              << ",\"encode_backend\":\"cpp\",\"encode_layout\":\"csr+cids\""
              << ",\"encode_parallel\":true,\"merge\":\"kway_sorted\""
              << ",\"thread_pool\":\"persistent\"}\n"
              << std::flush;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        if (line.find("\"cmd\"") != std::string::npos && line.find("shutdown") != std::string::npos) {
            std::cout << "{\"status\":\"shutdown\"}\n" << std::flush;
            break;
        }
        try {
            const auto tokens = parse_json_string_array_field(line, "tokens");
            if (tokens.empty()) {
                throw std::runtime_error("missing tokens field");
            }

            const auto t_pre0 = std::chrono::steady_clock::now();
            const auto prepped = query_full_preprocess(tokens, token_to_id);
            std::vector<std::vector<int32_t>> enc_by_band;
            encode_query_parallel(pool, prepped, band_metas, enc_by_band);
            const double preprocess_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_pre0).count();

            std::vector<int32_t> merged;
            double search_wall_s = 0.0;
            search_one_query_parallel(pool, indices, enc_by_band, min_hits, merged, search_wall_s);

            const bool counts_only =
                line.find("\"counts_only\"") != std::string::npos &&
                (line.find("true") != std::string::npos || line.find(":1") != std::string::npos);

            std::cout << "{\"status\":\"ok\",\"preprocess_s\":" << preprocess_s
                      << ",\"search_wall_s\":" << search_wall_s << ",\"n_candidates\":" << merged.size();
            if (counts_only) {
                std::cout << "}\n" << std::flush;
            } else {
                std::cout << ",\"candidates\":[";
                for (std::size_t i = 0; i < merged.size(); ++i) {
                    if (i) {
                        std::cout << ',';
                    }
                    std::cout << merged[i];
                }
                std::cout << "]}\n" << std::flush;
            }
        } catch (const std::exception& ex) {
            std::cout << "{\"status\":\"error\",\"error\":\"" << ex.what() << "\"}\n" << std::flush;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string hpcx_dir;
    std::string refs_dir;
    std::string dict_path;
    std::string bands_spec = "0-24";
    int workers = 6;
    int min_hits = 5;
    bool serve = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--serve") {
            serve = true;
        } else if (arg == "--hpcx-dir" && i + 1 < argc) {
            hpcx_dir = argv[++i];
        } else if (arg == "--refs-dir" && i + 1 < argc) {
            refs_dir = argv[++i];
        } else if (arg == "--dict" && i + 1 < argc) {
            dict_path = argv[++i];
        } else if (arg == "--bands" && i + 1 < argc) {
            bands_spec = argv[++i];
        } else if (arg == "--workers" && i + 1 < argc) {
            workers = std::atoi(argv[++i]);
        } else if (arg == "--min-hits" && i + 1 < argc) {
            min_hits = std::atoi(argv[++i]);
        }
    }

    if (!serve || hpcx_dir.empty() || refs_dir.empty() || dict_path.empty()) {
        std::cerr << "usage: hpcx_multiband_pool_cli --serve --hpcx-dir DIR --refs-dir DIR --dict DICT.bin "
                     "[--bands 0-24] [--workers 6] [--min-hits 5]\n";
        return 2;
    }
    if (!hpcx_dir.empty() && hpcx_dir.back() == '/') {
        hpcx_dir.pop_back();
    }
    if (!refs_dir.empty() && refs_dir.back() == '/') {
        refs_dir.pop_back();
    }
    const std::vector<int> bands = parse_bands_spec(bands_spec);
    if (bands.empty()) {
        std::cerr << "no bands parsed from: " << bands_spec << "\n";
        return 2;
    }
    return run_serve(hpcx_dir, refs_dir, dict_path, bands, workers, min_hits);
}
