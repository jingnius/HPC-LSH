#include "dict_build.h"

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

unsigned dict_build_threads() {
    if (const char* env = std::getenv("DICT_BUILD_THREADS")) {
        const int n = std::atoi(env);
        if (n > 0) {
            return static_cast<unsigned>(n);
        }
    }
    const unsigned hw = std::thread::hardware_concurrency();
    return hw > 0 ? hw : 4u;
}

void merge_sets(std::unordered_set<std::string>* dst, std::unordered_set<std::string>&& src) {
    dst->insert(std::make_move_iterator(src.begin()), std::make_move_iterator(src.end()));
}

void collect_unique_tokens_parallel(
    const std::vector<TargetColumn>& columns,
    std::unordered_set<std::string>* out) {
    const unsigned n_threads = std::min(dict_build_threads(), static_cast<unsigned>(columns.size()));
    if (n_threads <= 1 || columns.empty()) {
        out->reserve(columns.size() * 4);
        for (const TargetColumn& col : columns) {
            for (const std::string& tok : col.tokens) {
                out->insert(tok);
            }
        }
        return;
    }

    std::vector<std::unordered_set<std::string>> locals(n_threads);
    std::vector<std::thread> workers;
    workers.reserve(n_threads);

    const std::size_t chunk = (columns.size() + n_threads - 1) / n_threads;
    for (unsigned t = 0; t < n_threads; ++t) {
        const std::size_t begin = static_cast<std::size_t>(t) * chunk;
        const std::size_t end = std::min(columns.size(), begin + chunk);
        if (begin >= end) {
            break;
        }
        workers.emplace_back([&columns, &locals, t, begin, end]() {
            std::unordered_set<std::string>& local = locals[t];
            local.reserve((end - begin) * 4);
            for (std::size_t i = begin; i < end; ++i) {
                for (const std::string& tok : columns[i].tokens) {
                    local.insert(tok);
                }
            }
        });
    }
    for (std::thread& th : workers) {
        th.join();
    }

    out->reserve(columns.size() * 4);
    for (auto& local : locals) {
        merge_sets(out, std::move(local));
    }
}

}  // namespace

bool dict_build_from_columns(
    const std::vector<TargetColumn>& columns,
    DictBuildResult* out,
    std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }

    std::unordered_set<std::string> unique;
    collect_unique_tokens_parallel(columns, &unique);

    std::vector<std::string> sorted;
    sorted.reserve(unique.size());
    sorted.insert(sorted.end(), std::make_move_iterator(unique.begin()), std::make_move_iterator(unique.end()));
    std::sort(sorted.begin(), sorted.end());

    out->dict.id_to_token = std::move(sorted);
    out->gram_counts.clear();
    return true;
}

bool dict_build_from_target_json(
    const std::string& target_path,
    DictBuildResult* out,
    std::string* err) {
    std::vector<TargetColumn> columns;
    TargetReadOptions opts;
    opts.keep_empty_columns = true;
    if (!target_read_json(target_path, &columns, err, opts)) {
        return false;
    }
    return dict_build_from_columns(columns, out, err);
}
