"""Pinned defaults for the pure C++ HPC pipeline (canonical v6 — build memory compression + fast dict)."""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

PIPELINE_VERSION = "cpp-default-v6"
DEFAULT_DICT_SOURCE = "cpp"
DEFAULT_SCHEME_B_PRF_TOLERANCE = 0.02
DEFAULT_WORKERS = 6
DEFAULT_N_BANDS = 25
DEFAULT_BANDS_SPEC = "0-24"
DEFAULT_MIN_HITS = 5
# Default: always rebuild prep/sig/hpc (overwrite existing artifacts). Use --reuse-existing to skip.
DEFAULT_REBUILD = True

REPO = Path(__file__).resolve().parents[1]
PROJ = REPO
SCAL = REPO
SIG_CPP = SCAL / "sig_cpp"
HPCX_CPP = SCAL / "hpcx_cpp"

# Canonical data run for 60k gate / default dev loop.
DEFAULT_RUN_DIR = PROJ / "work/artifacts/N60000"
DEFAULT_TARGET_FILE = PROJ / "data/target_60000.json"

DEFAULT_QUERY_FILE = REPO / "queries/queries_100_topTokenFreq_N60000.json"
DEFAULT_ENCODED_QUERY_FILE = SCAL / "queries/encoded_queries_5_band0.json"

REPORTS_DIR = SCAL / "final_test_6m"
PIN_MANIFEST = REPORTS_DIR / "cpp_pipeline_default_v6.json"
PIN_MANIFEST_V5 = REPORTS_DIR / "cpp_pipeline_default_v5.json"
PIN_MANIFEST_V4 = REPORTS_DIR / "cpp_pipeline_default_v4.json"
PIN_MANIFEST_V3 = REPORTS_DIR / "cpp_pipeline_default_v3.json"
PIN_MANIFEST_V2 = REPORTS_DIR / "cpp_pipeline_default_v2.json"
PIN_MANIFEST_V1 = REPORTS_DIR / "cpp_pipeline_default_v1.json"


def resolve_target_json(run_dir: Path | None = None, target_file: Path | None = None) -> Path:
    """Locate raw target JSON for C++ prep (no pickle bridge)."""
    if target_file is not None:
        tf = Path(target_file)
        if tf.is_file():
            return tf
        raise FileNotFoundError(f"target file missing: {tf}")

    rd = run_dir or DEFAULT_RUN_DIR
    for candidate in (rd / "target.json", rd / "target_filtered.json"):
        if candidate.is_file():
            return candidate

    n_targets = 60000
    params_path = rd / "hpc_params.json"
    if params_path.is_file():
        params = json.loads(params_path.read_text())
        n_targets = int(params.get("n_targets", n_targets))
        p = params.get("target_file")
        if p:
            tp = Path(p)
            if tp.is_file():
                return tp

    default = PROJ / "data" / f"target_{n_targets}.json"
    if default.is_file():
        return default
    if DEFAULT_TARGET_FILE.is_file():
        return DEFAULT_TARGET_FILE
    raise FileNotFoundError(
        f"no target JSON for run_dir={rd} (tried target.json, hpc_params.target_file, {default})"
    )


def native_artifacts_dir(run_dir: Path | None = None) -> Path:
    rd = run_dir or DEFAULT_RUN_DIR
    return SIG_CPP / "refs" / rd.name


def hpcx_artifacts_dir(run_dir: Path | None = None) -> Path:
    rd = run_dir or DEFAULT_RUN_DIR
    return rd / "hpcx_cpp"


def hpcx_band_path(band: int, run_dir: Path | None = None) -> Path:
    return hpcx_artifacts_dir(run_dir) / f"band{band}.hpcx"


def dict_native_path(run_dir: Path | None = None) -> Path:
    return native_artifacts_dir(run_dir) / "dict.native.bin"


def dictionary_obj_path(run_dir: Path | None = None) -> Path:
    rd = run_dir or DEFAULT_RUN_DIR
    return rd / "dictionary.obj"


def dict_source() -> str:
    """cpp = dict_build_cli from target (default); python = export dictionary.obj (parity)."""
    return os.environ.get("HPCX_DICT_SOURCE", DEFAULT_DICT_SOURCE).strip().lower()


def dict_uses_cpp_build() -> bool:
    return dict_source() not in ("python", "dictionary_obj", "obj", "pkl")


def ensure_dict_native_from_run_dir(
    run_dir: Path | None = None,
    out_dir: Path | None = None,
    *,
    force: bool = False,
) -> Path:
    """Export run_dir/dictionary.obj → dict.native.bin (scheme A / parity with Python baseline)."""
    import sys

    rd = run_dir or DEFAULT_RUN_DIR
    od = out_dir or native_artifacts_dir(rd)
    tools = SIG_CPP / "tools"
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    from export_native_refs import ensure_dict_native_from_run_dir as _ensure  # noqa: WPS433

    return _ensure(rd, od / "dict.native.bin", force=force)


def ensure_dict_native_from_cpp(
    target_file: Path,
    out_dir: Path,
    *,
    force: bool = False,
) -> Path:
    """Build dict.native.bin from target JSON via C++ dict_build_cli (scheme B)."""
    out_path = out_dir / "dict.native.bin"
    out_dir.mkdir(parents=True, exist_ok=True)
    if (
        not force
        and out_path.is_file()
        and out_path.stat().st_size > 0
    ):
        return out_path
    cli = SIG_CPP / "dict_build_cli"
    if not cli.is_file():
        subprocess.run(["make", "-C", str(SIG_CPP), "dict_build_cli"], check=True)
    subprocess.run(
        [str(cli), "--target", str(target_file), "--out", str(out_path)],
        check=True,
    )
    return out_path


def ensure_dict_native(
    run_dir: Path | None = None,
    out_dir: Path | None = None,
    target_file: Path | None = None,
    *,
    force: bool = False,
) -> Path:
    """Resolve dict.native.bin using HPCX_DICT_SOURCE (default: cpp scheme B)."""
    rd = run_dir or DEFAULT_RUN_DIR
    od = out_dir or native_artifacts_dir(rd)
    if dict_uses_cpp_build():
        tf = resolve_target_json(rd, target_file)
        return ensure_dict_native_from_cpp(tf, od, force=force)
    return ensure_dict_native_from_run_dir(rd, od, force=force)


def sample_order_native_path(band: int, run_dir: Path | None = None) -> Path:
    return native_artifacts_dir(run_dir) / f"sample_order_band{band}.native.bin"


def load_sample_order_from_native(path: Path) -> list[int]:
    """Read C++ SORD.bin (sampleOrder from sig_build_cli)."""
    import struct

    data = path.read_bytes()
    magic, _ver, n = struct.unpack_from("<IIQ", data, 0)
    if magic != 0x44524F53:
        raise ValueError(f"bad SORD magic in {path}")
    return list(struct.unpack(f"<{n}i", data[16 : 16 + 4 * n]))


def load_band_meta_from_native(
    band: int,
    lsh,
    *,
    run_dir: Path | None = None,
    num_perm: int = 5,
) -> dict:
    """Band search meta from native artifacts (aligned with C++ sig/HPC build)."""
    order_path = sample_order_native_path(band, run_dir)
    if not order_path.is_file():
        raise FileNotFoundError(f"missing native sample order: {order_path}")
    sample_order = load_sample_order_from_native(order_path)
    return {
        "sampleOrder": sample_order,
        "num_perm": num_perm,
        "code2index": lsh.code2index(data=sample_order),
    }


def load_str2id_from_native_dict(dict_path: Path) -> dict[str, dict]:
    """Build str2id mapping from C++ DICT.bin (compatible with encode_query_for_band)."""
    import struct

    data = dict_path.read_bytes()
    magic, _ver, n = struct.unpack_from("<III", data, 0)
    if magic != 0x54434944:
        raise ValueError(f"bad DICT magic in {dict_path}")
    off = 12
    out: dict[str, dict] = {}
    for i in range(n):
        (ln,) = struct.unpack_from("<I", data, off)
        off += 4
        # Some web-table tokens contain surrogate bytes (mojibake emoji); match C++ byte identity.
        tok = data[off : off + ln].decode("utf-8", errors="surrogateescape")
        off += ln
        out[tok] = {"index": i, "count": 1}
    return out


def write_pin_manifest() -> Path:
    import json

    REPORTS_DIR.mkdir(parents=True, exist_ok=True)
    PIN_MANIFEST.write_text(json.dumps(pin_manifest(), indent=2))
    return PIN_MANIFEST


def pin_manifest() -> dict:
    return {
        "pipeline_version": PIPELINE_VERSION,
        "default_workers": DEFAULT_WORKERS,
        "default_n_bands": DEFAULT_N_BANDS,
        "default_bands_spec": DEFAULT_BANDS_SPEC,
        "default_run_dir": str(DEFAULT_RUN_DIR),
        "default_target_file": str(DEFAULT_TARGET_FILE),
        "default_query_file": str(DEFAULT_QUERY_FILE),
        "native_artifacts_dir": str(native_artifacts_dir()),
        "hpcx_artifacts_dir": str(hpcx_artifacts_dir()),
        "sig_cpp": str(SIG_CPP),
        "hpcx_cpp": str(HPCX_CPP),
        "prep": {
            "language": "C++",
            "dict_source_env": "HPCX_DICT_SOURCE",
            "dict_source_default": "cpp",
            "dict_cpp_token_order": "alphabetical_sorted",
            "dict_python_fallback": "run_dir/dictionary.obj",
            "no_pickle_bridge": True,
            "entrypoint_cpp": "sig_cpp/prep_cli (no --dict)",
            "entrypoint_python": "sig_cpp/prep_cli --dict",
            "rebuild_default": DEFAULT_REBUILD,
            "reuse_existing_flag": "--reuse-existing",
            "dict_build_optimizations": {
                "unordered_set_final_sort": True,
                "drop_gram_counts_second_pass": True,
                "target_json_parser": "simdjson (singleheader v3.9.4)",
                "parallel_token_collect": True,
                "parallel_threads_env": "DICT_BUILD_THREADS",
                "output_invariant": "dict.native.bin byte-identical to pre-opt scheme B",
            },
        },
        "memory_policy": {
            "release_between_phases": True,
            "phases": ["prep", "sig", "hpc", "search"],
            "mechanism": "gc.collect() via cpp_process_utils.release_memory()",
        },
        "build": {
            "execution_model": "per_band_cli_subprocess",
            "per_band_exit": True,
            "parallel_scheduler": "ThreadPoolExecutor (workers=6)",
            "index_one_layout": "csr",
            "index_one_store": "hpcx_cpp/hpc_index_one_store.cpp",
            "dim_maps": "on_demand",
            "build_memory_release": "post_partition",
            "phases": {
                "prep": "single prep_cli subprocess",
                "sig": "25 × sig_build_cli (one process per band, exit after build)",
                "hpc": "25 × hpcx_build_cli --from-sig (one process per band, exit after build)",
            },
            "orchestrator": "tools/run_cpp_hpc_pipeline.py",
            "full_orchestrator": "tools/run_cpp_full_pipeline.py",
        },
        "search": {
            "execution_model": "cpp_preprocess_encode_resident_hpc_pool",
            "encode_backend": "C++ (hpcx_cpp/query_encode.cpp)",
            "encode_layout": "csr+cids",
            "encode_parallel": True,
            "merge": "kway_sorted",
            "thread_pool": "persistent",
            "pool_cli": "hpcx_cpp/hpcx_multiband_pool_cli --serve",
            "pool_wrapper": "tools/hpcx_search_pool.py",
            "orchestrator": "tools/run_cpp_hpcx_multiband_search.py",
            "resident_hpc_bands": DEFAULT_N_BANDS,
            "parallel_band_workers": DEFAULT_WORKERS,
            "input_protocol": {"tokens": ["string", "..."]},
            "timing_included": ["search_parallel_s"],
            "timing_excluded": [
                "load_s_excluded (dict/str2id + sample_orders + 25 HPCX)",
                "preprocess_s (identity dedup + token→cid + 25-band encode)",
                "setup_excluded_s (query JSON load in orchestrator)",
                "query_set_generation (e.g. topTokenFreq top-100)",
            ],
            "per_query_search_s": "search_parallel_s",
            "search_total_s": "sum_q(search_parallel_s)",
            "optimizations": {
                "pool": "WorkerPool task-queue; threads reused across queries",
                "encode": "QueryPreprocessed cids once; CSR code lookup; epoch hit stamp",
                "hpcx_search_encoded": "TLS BFS scratch; route_children_into; flat mgn lookup; matrix pack scan",
                "merge": "25-way k-way merge on sorted per-band candidate lists",
            },
        },
        "components": {
            "prep": "sig_cpp/prep_cli",
            "dict_build": "sig_cpp/dict_build_cli",
            "cybuf_pack": "sig_cpp/cybuf_pack_cli",
            "inver_build": "sig_cpp/inver_build_cli",
            "signature_build": "sig_cpp/sig_build_cli",
            "hpcx_build": "hpcx_cpp/hpcx_build_cli --from-sig",
            "hpc_index_one_store": "hpcx_cpp/hpc_index_one_store.cpp",
            "query_encode": "hpcx_cpp/query_encode.cpp",
            "hpcx_search_pool": "hpcx_cpp/hpcx_multiband_pool_cli",
            "hpcx_search": "tools/run_cpp_hpcx_multiband_search.py",
            "orchestrator": "tools/run_cpp_hpc_pipeline.py",
            "full_pipeline": "tools/run_cpp_full_pipeline.py",
            "scalability": "tools/run_cpp_scalability_test.py",
        },
        "gate_scripts": {
            "dict_build": "sig_cpp/tools/verify_dict_build.py",
            "inver_build": "sig_cpp/tools/verify_inver_build.py",
            "cybuf_pack": "sig_cpp/tools/verify_cybuf_pack.py",
            "prep_e2e": "sig_cpp/tools/verify_prep_e2e.py",
            "hpcx_search_parity": "tools/verify_cpp_hpcx_multiband_search.py",
        },
        "gates_60k": {
            "verify_dict_build": {
                "gate_pass": True,
                "scheme": "B",
                "idempotent_compare": True,
                "vocab_vs_dictionary_obj": True,
                "n_tokens": 375066,
            },
            "verify_inver_build": {"gate_pass": True, "n_entries": 587991},
            "verify_cybuf_pack": {"gate_pass": True, "n_cols": 60000},
            "verify_prep_e2e": {"gate_pass": True, "scheme": "B"},
            "downstream_byte_parity": {
                "dict_vs_gate": True,
                "inver_vs_gate": True,
                "cybuf_vs_gate": True,
                "sig_band0_vs_gate": True,
            },
        },
        "gates_60k_all_pass": True,
        "supersedes": "cpp-default-v5",
        "changes_from_v5": [
            "HpcIndexOneStore: flat CSR col_ids + hit_off + hit_pool (two-pass SIGB load)",
            "partition build: on-demand nth_hit / dim maps (no prebuilt dim_maps)",
            "native build: release index_one after partition BFS",
            "hpc_ct_ops: hpc_adaptive_first1_boundaries_nth for on-demand dim access",
            "dict_build: unordered_set + one final sort (replaces periodic dedupe+sort)",
            "dict_build: drop unused gram_counts second pass",
            "target_json: simdjson parser (shared by dict/inver/cybuf prep)",
            "dict_build: parallel token collection (DICT_BUILD_THREADS)",
        ],
        "changes_from_v4": [
            "search pool: persistent WorkerPool (task-queue) across queries",
            "search: parallel 25-band encode in pool",
            "hpcx_search_encoded: TLS scratch, vector BFS, flat mgn lookup, matrix row pack",
            "encode: QueryPreprocessed (token→cid once) + CSR code lookup + epoch hit stamp",
            "merge: k-way sorted union across 25 bands (replaces hash set + sort)",
        ],
        "changes_from_v3": [
            "memory release after each phase (prep/sig/hpc/search)",
            "build: one CLI subprocess per band, exit after build",
            "default overwrite rebuild; --reuse-existing to skip",
            "search: 25 HPCX resident in pool + parallel band workers",
            "search: C++ query preprocess + 25-band encode (no Python LSH in search path)",
        ],
        "search_benchmark_N1500000_w12_100q": {
            "note": "counts_only, min_hits=5; search_wall_s excludes merge; v6 search path unchanged from v5",
            "v4_reference": {
                "preprocess_s": 1.47,
                "search_wall_s": 11.58,
                "total_s": 13.05,
                "n_candidates": 1099896,
            },
            "v5": {
                "preprocess_s": 0.99,
                "search_wall_s": 12.2,
                "total_s": 13.07,
                "n_candidates": 1099904,
            },
        },
        "build_benchmark_band0_N1500000": {
            "note": "CSR index_one build; search path unchanged from v5",
            "artifact_bytes": 210551759,
            "baseline_artifact_bytes": 210551400,
            "n_views": 111183,
            "n_pools": 40,
            "search_10q_candidate_parity": "10/10 exact",
            "build_rss_gb": {"before": 2.55, "after": 2.44},
            "w12_parallel_rss_gb_estimate": {"before": 28, "after": 26},
        },
        "build_benchmark_N60000_w1_100q": {
            "note": "workers=1; fast dict build pinned in v6; full pipeline after dict opt",
            "result_json": str(
                SCAL / "results/scalability_cpp_HPC-LSH_N60000_opt_dict.json"
            ),
            "prf_json": str(SCAL / "results/scalability_cpp_HPC-LSH_N60000_opt_dict_prf.json"),
            "dict_s": 1.147,
            "prep_total_s": 2.976,
            "build_s": 121.484,
            "search_s": 4.477,
            "dict_rss_gb": 0.159,
            "n_tokens": 375066,
        },
        "scheme_b_eval_60k": {
            "dict_source": DEFAULT_DICT_SOURCE,
            "tau": 0.5,
            "query_file": str(DEFAULT_QUERY_FILE),
            "gt_file": str(REPO / "data/gt_queries_10k_extended.json"),
            "baseline": str(SCAL / "verify_depth_flat/N60000/flat_only/experiment.json"),
            "prf_tolerance": DEFAULT_SCHEME_B_PRF_TOLERANCE,
            "baseline_macro": {
                "precision": 0.919957,
                "recall": 0.809005,
                "f1": 0.860921,
                "raw_cand_total": 40068,
            },
            "candidate_macro": {
                "precision": 0.910178,
                "recall": 0.813690,
                "f1": 0.859234,
                "raw_cand_total": 40901,
            },
            "opt_dict_macro": {
                "precision": 0.910178,
                "recall": 0.813690,
                "f1": 0.859234,
                "raw_cand_total": 40901,
            },
            "delta_vs_baseline": {
                "precision": -0.009778,
                "recall": 0.004685,
                "f1": -0.001687,
            },
            "delta_vs_candidate_ref": {
                "precision": 0.0,
                "recall": 0.0,
                "f1": 0.0,
            },
            "gate_pass": True,
            "search_execution_model": "cpp_preprocess_encode_resident_hpc_pool",
        },
    }


if __name__ == "__main__":
    import argparse
    import json
    import sys

    p = argparse.ArgumentParser(description="C++ pipeline pinned defaults.")
    p.add_argument("--write-manifest", action="store_true")
    p.add_argument("--json", action="store_true")
    args = p.parse_args()
    if args.write_manifest:
        path = write_pin_manifest()
        print(json.dumps({"manifest": str(path), **pin_manifest()}, indent=2))
        raise SystemExit(0)
    if args.json:
        print(json.dumps(pin_manifest(), indent=2))
        raise SystemExit(0)
    print(f"{PIPELINE_VERSION} workers={DEFAULT_WORKERS} run_dir={DEFAULT_RUN_DIR}")
    raise SystemExit(0)
