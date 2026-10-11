#include "hpc_native_build.h"

#include "hpc_build.h"
#include "hpc_index_one_store.h"
#include "hpc_mgn_build.h"
#include "hpc_native_write.h"
#include "hpc_partition_build.h"
#include "hpc_partition_bfs_v8.h"
#include "hpc_py_embed.h"
#include "hpc_routes_build.h"
#include "hpc_run_config.h"
#include "hpc_sig_io.h"
#include "hpc_rss.h"
#include "hpc_v8_stream_checkpoint.h"

#include <chrono>
#include <cstdlib>
#include <string>

namespace {

bool use_python_bridge_fallback() {
    const char* v = std::getenv("HPCX_SIG_LEGACY_PY_TREE");
    return v != nullptr && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
}

bool env_build_v8() {
    const char* v = std::getenv("HPC_BUILD_V8");
    return v != nullptr && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
}

}  // namespace

bool hpcx_build_from_sig_native(
    const std::string& sig_native_path,
    const std::string& run_dir,
    int band,
    const std::string& out_hpcx_path,
    HpcxWriteStats* stats,
    std::string* err) {
    if (use_python_bridge_fallback()) {
        const std::string bridge_zip = out_hpcx_path + ".bridge.zip";
        if (!hpc_py_export_hpc_zip_streaming(sig_native_path, run_dir, band, bridge_zip, "", "", err)) {
            return false;
        }
        const bool ok = hpcx_build_from_zip(bridge_zip, out_hpcx_path, band, stats, err);
        std::remove(bridge_zip.c_str());
        return ok;
    }

    const auto t0 = std::chrono::steady_clock::now();

    HpcSigBandMeta meta{};
    if (!hpc_sigb_read_header(sig_native_path, &meta, err)) {
        return false;
    }
    if (static_cast<int>(meta.band) != band) {
        if (err) {
            *err = "SIGB band mismatch: file=" + std::to_string(meta.band) + " expected=" + std::to_string(band);
        }
        return false;
    }

    HpcRunBuildConfig cfg{};
    if (!hpc_load_run_build_config(run_dir, &cfg, err)) {
        return false;
    }

    HpcPartitionBuildResult part;
    HpcPartitionV8Context v8_ctx;
    const bool v8 = env_build_v8();
    if (v8) {
        part = hpc_run_partition_bfs_v8(sig_native_path, cfg.partition, &v8_ctx, err);
    } else {
        HpcIndexOneStore index_one;
        if (!index_one.build_from_sigb(sig_native_path, err)) {
            return false;
        }
        part = hpc_run_partition_bfs(index_one, cfg.partition);
        index_one.clear();
        index_one.col_ids.shrink_to_fit();
        index_one.hit_off.shrink_to_fit();
        index_one.hit_pool.shrink_to_fit();
    }

    if (std::getenv("HPCX_KEEP_PARTITION_JSON") != nullptr) {
        hpc_partition_write_json(*part.root, part.mgn_leaves, out_hpcx_path + ".partition.json", err);
    }
    if (part.root == nullptr) {
        if (err && err->empty()) {
            *err = "C++ partition build produced empty root";
        }
        return false;
    }

    std::vector<HpcFlatMgnLeaf> mgn_leaves;
    if (v8) {
        if (!hpc_build_mgn_from_sigb_v8(
                sig_native_path,
                static_cast<int>(meta.num_perm),
                part.mgn_leaves,
                v8_ctx,
                &mgn_leaves,
                err)) {
            return false;
        }
    } else if (!hpc_build_mgn_from_sigb(
                   sig_native_path,
                   static_cast<int>(meta.num_perm),
                   part.mgn_leaves,
                   &mgn_leaves,
                   err)) {
        return false;
    }

    hpc_build_query_routes(*part.root, cfg.route_r);

    const auto t_compute_done = std::chrono::steady_clock::now();
    HpcNativeBuildStats native_stats;
    HpcxWriteStats write_stats;
    if (!hpcx_write_from_native_tree(
            *part.root,
            mgn_leaves,
            band,
            out_hpcx_path,
            &write_stats,
            &native_stats,
            err,
            v8 ? &v8_ctx : nullptr)) {
        return false;
    }
    if (stats != nullptr) {
        *stats = write_stats;
        stats->compute_s = std::chrono::duration<double>(t_compute_done - t0).count() + write_stats.compute_s;
    }
    return true;
}
