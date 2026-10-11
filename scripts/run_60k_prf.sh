#!/usr/bin/env bash
# Default HPC-LSH, workers=1.
# For each containment threshold 0.1..0.9, (b, r) comes from MinHash LSH
# with num_perm=128. Rebuild signatures for that (b, r), search with
# min_hits=r, and score macro P/R/F1 on query-100 / query-1k / query-10k.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
source "$REPO/env/source_v8_phase1_d2.sh"
export HPCX_BUILD_CLI="$REPO/hpcx_cpp/hpcx_build_v8.sh"
export HPC_PIPELINE_VERSION="${HPC_PIPELINE_VERSION:-cpp-default-v8-phase1-d2-src}"
export SIG_CYBUF_MMAP=1
export SIG_CYBUF_VECTOR=0
export DICT_BUILD_THREADS=1
export OMP_NUM_THREADS=1
export HPC_REPO="$REPO"
mkdir -p "$TMPDIR" "$REPO/work/results"
exec > >(tee -a "$REPO/work/results/prf_60k_w1.log") 2>&1
echo "=== $(date -Iseconds) 60k w1 P/R/F1 per threshold ==="
python3 - << 'PY'
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from datasketch import MinHashLSH

repo = Path(os.environ["HPC_REPO"])
sys.path.insert(0, str(repo / "tools"))
from cpp_pipeline_defaults import dict_native_path, native_artifacts_dir
from eval_containment_prf import eval_containment_prf
from hpcx_search_pool import HpcxSearchPool
from run_cpp_scalability_test import HPCX_POOL, MemorySampler, release_memory

NUM_PERM = 128
TAUS = [i / 10 for i in range(1, 10)]
WORKERS = 1
QUERY_SETS = [
    ("100", repo / "data/source_queries_100.json", repo / "data/gt_queries_100_extended.json"),
    ("1k", repo / "data/source_queries_1k.json", repo / "data/gt_queries_1k_extended.json"),
    ("10k", repo / "data/source_queries_10k.json", repo / "data/gt_queries_10k_extended.json"),
]
PARAMS = repo / "config/hpc_params_N60000.json"
TARGET = repo / "data/target_60000.json"

def lsh_br(threshold: float, num_perm: int) -> tuple[int, int]:
    lsh = MinHashLSH(threshold=threshold, num_perm=num_perm)
    return int(lsh.b), int(lsh.r)

sets_out = {label: {"query_file": str(q), "gt_file": str(g), "by_tau": {}} for label, q, g in QUERY_SETS}
builds = {}

for tau in TAUS:
    key = f"{tau:.1f}"
    b, r = lsh_br(tau, NUM_PERM)
    print(f"=== THRESHOLD {key} b={b} r={r} min_hits={r} num_perm={NUM_PERM} ===", flush=True)
    run_dir = repo / "work/artifacts" / f"N60000_t{key}"
    native = native_artifacts_dir(run_dir)
    hpcx = run_dir / "hpcx_cpp"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    if native.exists():
        shutil.rmtree(native)
    run_dir.mkdir(parents=True)
    hpcx.mkdir(parents=True)
    shutil.copyfile(PARAMS, run_dir / "hpc_params.json")
    build_json = repo / "work/results" / f"prf_60k_w1_build_t{key}.json"
    subprocess.run(
        [
            sys.executable,
            str(repo / "tools/run_cpp_scalability_test.py"),
            "--run-dir", str(run_dir),
            "--hpcx-dir", str(hpcx),
            "--target-file", str(TARGET),
            "--query-file", str(QUERY_SETS[0][1]),
            "--bands", f"0-{b - 1}",
            "--n-bands", str(b),
            "--num-perm", str(r),
            "--sig-workers", str(WORKERS),
            "--hpc-workers", str(WORKERS),
            "--search-workers", str(WORKERS),
            "--workers", str(WORKERS),
            "--tau", key,
            "--min-hits", str(r),
            "--n-queries", "1000",
            "--skip-search",
            "--json-out", str(build_json),
        ],
        check=True,
    )
    build = json.loads(build_json.read_text())
    sig_m = build.get("signature_build", {}).get("memory", {})
    hpc_m = build.get("hpc_build", {}).get("memory", {})
    prep_m = build.get("prep", {}).get("memory", {})
    build_rss = max(
        float(sig_m.get("peak_tree_rss_gb") or 0),
        float(hpc_m.get("peak_tree_rss_gb") or 0),
        float(prep_m.get("peak_tree_rss_gb") or 0),
    )
    builds[key] = {
        "b": b,
        "r": r,
        "min_hits": r,
        "num_perm": NUM_PERM,
        "build_s": build.get("build_s"),
        "build_memory_peak_tree_rss_gb": round(build_rss, 3),
    }
    bands = list(range(b))
    for label, query_path, gt_path in QUERY_SETS:
        queries = json.loads(query_path.read_text())
        qids = list(queries.keys())
        candidates = {}
        search_parallel_s = 0.0
        print(f"=== SEARCH {label} tau={key} n_queries={len(qids)} ===", flush=True)
        with MemorySampler() as mem_search:
            with HpcxSearchPool(
                hpcx_dir=hpcx,
                refs_dir=native,
                dict_path=dict_native_path(run_dir),
                bands=bands,
                workers=WORKERS,
                min_hits=r,
                pool_cli=HPCX_POOL,
            ) as pool:
                load_s = pool.load_s
                for qid in qids:
                    rep = pool.search_tokens(list(queries[qid]))
                    search_parallel_s += float(rep.get("search_wall_s", 0.0))
                    candidates[qid] = {str(c) for c in rep.get("candidates", [])}
        search_mem = mem_search.summary()
        release_memory()
        prf = eval_containment_prf(candidates, qids, gt_path, tau=tau)
        sets_out[label]["n_queries"] = len(qids)
        sets_out[label]["by_tau"][key] = {
            "b": b,
            "r": r,
            "min_hits": r,
            "precision": prf["precision"],
            "recall": prf["recall"],
            "f1": prf["f1"],
            "n_gt_nonempty": prf["aggregation"]["n_gt_nonempty"],
            "raw_cand_total": prf["raw_cand_total"],
            "search_s": round(search_parallel_s, 6),
            "load_s_excluded": round(load_s, 3),
            "search_memory_peak_tree_rss_gb": round(float(search_mem["peak_tree_rss_gb"]), 3),
        }
        print(
            f"{label} tau={key} b={b} r={r} P={prf['precision']:.6f} R={prf['recall']:.6f} F1={prf['f1']:.6f}",
            flush=True,
        )

summary = {
    "pipeline_version": os.environ.get("HPC_PIPELINE_VERSION"),
    "n_targets": 60000,
    "num_perm": NUM_PERM,
    "parallelism": {
        "signature_workers": WORKERS,
        "hpc_workers": WORKERS,
        "search_workers": WORKERS,
        "dict_build_threads": 1,
    },
    "builds": builds,
    "query_sets": sets_out,
}
out = repo / "work/results/prf_60k_w1_summary.json"
detail = repo / "work/results/prf_60k_w1.json"
out.write_text(json.dumps(summary, indent=2) + "\n")
detail.write_text(json.dumps(summary, indent=2) + "\n")
print("=== SUMMARY ===")
print(json.dumps(summary, indent=2))
PY
echo "=== $(date -Iseconds) 60k P/R/F1 done ==="
