#!/usr/bin/env python3
"""C++ pipeline scalability benchmark (prep -> sig -> HPC -> search).

Build time INCLUDES: dict, invert, cybuf prep, parallel signature compute, parallel HPC compute.
Build time EXCLUDES: writing signature / sample_order / HPC zip / HPCX files.

Search time INCLUDES: parallel HPC band search only (search_parallel_s; includes k-way merge).
Search time EXCLUDES: dict/str2id/HPC load, query JSON load, query-set generation, query preprocess+encode.

Parallelism: workers=6 for signature bands, HPC bands, and search band pool.
"""

from __future__ import annotations

import argparse
import gc
import json
import os
import resource
import subprocess
import sys
import threading
import time

from pathlib import Path
from typing import Any

SCAL = Path(__file__).resolve().parents[1]
TOOLS = SCAL / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
from cpp_pipeline_defaults import (  # noqa: E402
    DEFAULT_REBUILD,
    PIPELINE_VERSION,
    dict_native_path,
    ensure_dict_native_from_cpp,
    hpcx_artifacts_dir,
    native_artifacts_dir,
)

SIG_CPP = SCAL / "sig_cpp"
HPCX_CPP = SCAL / "hpcx_cpp"

from cpp_process_utils import parallel_cli_subprocess, release_memory  # noqa: E402
from hpcx_search_pool import HpcxSearchPool  # noqa: E402

PREP_CLI = SIG_CPP / "prep_cli"
DICT_CLI = SIG_CPP / "dict_build_cli"
INVER_CLI = SIG_CPP / "inver_build_cli"
CYBUF_CLI = SIG_CPP / "cybuf_pack_cli"
SIG_CLI = SIG_CPP / "sig_build_cli"
HPCX_CLI = HPCX_CPP / "hpcx_build_cli"
HPCX_BENCH = HPCX_CPP / "hpcx_bench_cli"
HPCX_POOL = HPCX_CPP / "hpcx_multiband_pool_cli"
HPCX_BUILD_V8_WRAPPER = HPCX_CPP / "hpcx_build_v8.sh"

PROJ = SCAL


def _rss_gb() -> float:
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / (1024 * 1024)


def _collect_process_tree(root_pid: int) -> set[int]:
    pids = {root_pid}
    changed = True
    while changed:
        changed = False
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            pid = int(entry)
            if pid in pids:
                continue
            try:
                with open(f"/proc/{pid}/status") as f:
                    for line in f:
                        if line.startswith("PPid:"):
                            ppid = int(line.split()[1])
                            if ppid in pids:
                                pids.add(pid)
                                changed = True
                            break
            except OSError:
                continue
    return pids


def _tree_rss_gb(root_pid: int | None = None) -> float:
    root = root_pid or os.getpid()
    total_kb = 0
    for pid in _collect_process_tree(root):
        try:
            with open(f"/proc/{pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        total_kb += int(line.split()[1])
                        break
        except OSError:
            continue
    return total_kb / (1024 * 1024)


def _system_mem_gb() -> dict[str, float]:
    info: dict[str, float] = {}
    try:
        with open("/proc/meminfo") as f:
            kv = {}
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    kv[parts[0].rstrip(":")] = int(parts[1])
        info["mem_total_gb"] = kv.get("MemTotal", 0) / (1024 * 1024)
        info["mem_avail_gb"] = kv.get("MemAvailable", 0) / (1024 * 1024)
        info["mem_used_gb"] = info["mem_total_gb"] - info["mem_avail_gb"]
    except OSError:
        pass
    return info


class MemorySampler:
    """Poll process-tree RSS + system used memory during a phase."""

    def __init__(self, interval_s: float = 0.5) -> None:
        self.interval_s = interval_s
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.peak_self_rss_gb = 0.0
        self.peak_sys_used_gb = 0.0
        self.samples: list[dict[str, float]] = []

    def _poll_once(self) -> None:
        tree_rss = _tree_rss_gb()
        self.peak_self_rss_gb = max(self.peak_self_rss_gb, tree_rss)
        sysm = _system_mem_gb()
        used = sysm.get("mem_used_gb", 0.0)
        self.peak_sys_used_gb = max(self.peak_sys_used_gb, used)
        self.samples.append(
            {
                "t": time.time(),
                "tree_rss_gb": tree_rss,
                "self_rss_gb": _rss_gb(),
                "sys_used_gb": used,
                "sys_avail_gb": sysm.get("mem_avail_gb", 0.0),
            }
        )

    def _run(self) -> None:
        while not self._stop.wait(self.interval_s):
            self._poll_once()

    def __enter__(self) -> MemorySampler:
        self._poll_once()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *args: object) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)
        self._poll_once()

    def summary(self) -> dict[str, float | int | str]:
        peak_self = max((s.get("self_rss_gb", 0.0) for s in self.samples), default=_rss_gb())
        return {
            "peak_tree_rss_gb": round(self.peak_self_rss_gb, 3),
            "peak_self_rss_gb": round(peak_self, 3),
            "peak_sys_used_gb": round(self.peak_sys_used_gb, 3),
            "n_samples": len(self.samples),
            "note": (
                "peak_tree_rss_gb = max over samples of sum(VmRSS kB) for orchestrator+CLI children, "
                "GiB = kB/1024/1024; peak_self_rss_gb = max orchestrator ru_maxrss (Linux kB→GiB); "
                "peak_sys_used_gb from MemTotal-MemAvailable (kB→GiB)"
            ),
        }


def _ensure_binaries() -> None:
    if not SIG_CLI.is_file() or not DICT_CLI.is_file():
        subprocess.run(["make", "-C", str(SIG_CPP), "prep_cli", "sig_build_cli", "dict_build_cli", "inver_build_cli", "cybuf_pack_cli"], check=True)
    hpcx_bin = HPCX_CPP / "hpcx_build_cli"
    if not hpcx_bin.is_file() or not HPCX_BENCH.is_file() or not HPCX_POOL.is_file():
        subprocess.run(
            ["make", "-C", str(HPCX_CPP), "hpcx_build_cli", "hpcx_bench_cli", "hpcx_multiband_pool_cli"],
            check=True,
        )


def apply_v8_phase1_d2_env() -> None:
    env_file = SCAL / "env/hpc_v8_phase1_d2.env"
    if not env_file.is_file():
        return
    for raw in env_file.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :].strip()
        if line.startswith("unset "):
            os.environ.pop(line[len("unset ") :].strip(), None)
            continue
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        key, val = key.strip(), val.strip().strip('"').strip("'")
        if val.startswith("${") and ":-" in val:
            var, default = val[2:-1].split(":-", 1)
            val = os.environ.get(var, default)
        os.environ.setdefault(key, val)


def _parse_bands(spec: str, n_bands: int) -> list[int]:
    spec = spec.strip()
    if not spec or spec.lower() == "all":
        return list(range(n_bands))
    out: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return sorted(set(out))


def _load_existing_prep(out_dir: Path) -> dict[str, Any]:
    """Use prep artifacts already written under this repo. Do not rebuild them."""
    dict_path = out_dir / "dict.native.bin"
    inver_path = out_dir / "inver.native.bin"
    cybuf_path = out_dir / "cybuf.native.bin"
    missing = [p for p in (dict_path, inver_path, cybuf_path) if not p.is_file()]
    if missing:
        raise FileNotFoundError("skip-prep missing native files: " + ", ".join(str(p) for p in missing))
    timing = {"dict_s": 0.0, "inver_s": 0.0, "cybuf_s": 0.0, "prep_total_s": 0.0}
    meta: dict[str, Any] = {}
    meta_path = out_dir / "prep_timing.json"
    if meta_path.is_file():
        meta = json.loads(meta_path.read_text())
        timing.update(meta.get("timing") or {})
        timing["prep_total_s"] = round(
            float(timing.get("dict_s", 0.0)) + float(timing.get("inver_s", 0.0)) + float(timing.get("cybuf_s", 0.0)),
            3,
        )
    return {
        "dict": str(meta.get("dict") or dict_path),
        "inver": str(meta.get("inver") or inver_path),
        "cybuf": str(meta.get("cybuf") or cybuf_path),
        "timing": timing,
        "n_columns": meta.get("n_columns"),
        "n_tokens": meta.get("n_tokens"),
        "prep_backend": meta.get("prep_backend", "existing_native"),
    }


def run_prep(
    *,
    run_dir: Path,
    target_file: Path,
    out_dir: Path,
    force: bool,
    num_perm: int = 5,
) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    dict_path = out_dir / "dict.native.bin"
    inver_path = out_dir / "inver.native.bin"
    cybuf_path = out_dir / "cybuf.native.bin"

    timing: dict[str, float] = {}

    if force or not dict_path.is_file():
        t0 = time.perf_counter()
        ensure_dict_native_from_cpp(target_file, out_dir, force=True)
        timing["dict_s"] = round(time.perf_counter() - t0, 3)
    else:
        timing["dict_s"] = 0.0

    if force or not inver_path.is_file():
        t0 = time.perf_counter()
        subprocess.run(
            [str(INVER_CLI), "--dict", str(dict_path), "--target", str(target_file), "--out", str(inver_path)],
            check=True,
        )
        timing["inver_s"] = round(time.perf_counter() - t0, 3)
    else:
        timing["inver_s"] = 0.0

    if force or not cybuf_path.is_file():
        t0 = time.perf_counter()
        subprocess.run(
            [
                str(CYBUF_CLI),
                "--dict",
                str(dict_path),
                "--target",
                str(target_file),
                "--out",
                str(cybuf_path),
                "--num-perm",
                str(num_perm),
            ],
            check=True,
        )
        timing["cybuf_s"] = round(time.perf_counter() - t0, 3)
    else:
        timing["cybuf_s"] = 0.0

    timing["prep_total_s"] = round(timing["dict_s"] + timing["inver_s"] + timing["cybuf_s"], 3)
    release_memory()
    return {
        "dict": str(dict_path),
        "inver": str(inver_path),
        "cybuf": str(cybuf_path),
        "timing": timing,
        "prep_backend": "sig_cpp_source",
        "num_perm": num_perm,
    }


def run_prep_large_target(
    *,
    target_file: Path,
    out_dir: Path,
) -> dict[str, Any]:
    """Python json.loads prep when target JSON exceeds C++ simdjson DOM limit (~48M)."""
    script = TOOLS / "prep_large_target_native.py"
    if not script.is_file():
        raise FileNotFoundError(f"missing large-target prep script: {script}")
    subprocess.run(
        [sys.executable, str(script), str(target_file), str(out_dir)],
        check=True,
    )
    meta_path = out_dir / "prep_timing.json"
    meta = json.loads(meta_path.read_text())
    release_memory()
    return {
        "dict": meta["dict"],
        "inver": meta["inver"],
        "cybuf": meta["cybuf"],
        "timing": meta["timing"],
        "n_columns": meta.get("n_columns"),
        "n_tokens": meta.get("n_tokens"),
        "prep_backend": meta.get("prep_backend", "python_json_loads_large_target"),
    }


def _parse_cli_json(stdout: str) -> dict:
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            return json.loads(line)
    return {}


def run_parallel_signatures(
    *,
    bands: list[int],
    cybuf: Path,
    out_dir: Path,
    workers: int,
    force: bool,
) -> dict[str, Any]:
    tasks: list[dict] = []
    for band in bands:
        sig_out = out_dir / f"sig_band{band}.native.bin"
        order_out = out_dir / f"sample_order_band{band}.native.bin"
        if sig_out.is_file() and order_out.is_file() and not force:
            tasks.append({"band": band, "status": "skipped", "build_s": 0.0})
            continue
        tasks.append(
            {
                "band": band,
                "sig_out": str(sig_out),
                "order_out": str(order_out),
            }
        )

    build_tasks = [t for t in tasks if "sig_out" in t]
    t0 = time.perf_counter()

    def _cmd(task: dict) -> list[str]:
        return [
            str(SIG_CLI),
            "--cybuf",
            str(cybuf),
            "--seed",
            str(task["band"]),
            "--band",
            str(task["band"]),
            "--out",
            task["sig_out"],
            "--sample-order-out",
            task["order_out"],
        ]

    def _parse(stdout: str, task: dict) -> dict:
        meta = _parse_cli_json(stdout)
        return {
            "status": "built",
            "build_s": round(float(meta.get("build_s", meta.get("seconds", 0.0))), 3),
            "write_s_excluded": round(float(meta.get("write_s", 0.0)), 3),
            "n_samples": meta.get("n_samples"),
            "sig": task["sig_out"],
            "sample_order": task["order_out"],
        }

    built = parallel_cli_subprocess(build_tasks, _cmd, workers=workers, parse_stdout=_parse)
    skipped = [t for t in tasks if t.get("status") == "skipped"]
    results = built + skipped
    results.sort(key=lambda r: int(r["band"]))
    built_ok = [r for r in results if r.get("status") == "built"]
    compute_s = round(sum(float(r.get("build_s", 0.0)) for r in built_ok), 3)
    release_memory()
    return {
        "execution_model": "per_band_cli_subprocess",
        "wall_s": round(time.perf_counter() - t0, 3),
        "compute_s": compute_s,
        "sum_band_build_s": compute_s,
        "sum_band_write_s_excluded": round(sum(float(r.get("write_s_excluded", 0.0)) for r in built_ok), 3),
        "bands": results,
        "built": len(built_ok),
        "skipped": sum(1 for r in results if r.get("status") == "skipped"),
        "errors": [r for r in results if r.get("status") not in ("built", "skipped")],
    }


def run_parallel_hpc(
    *,
    bands: list[int],
    run_dir: Path,
    native_dir: Path,
    hpcx_dir: Path,
    workers: int,
    force: bool,
) -> dict[str, Any]:
    hpcx_dir.mkdir(parents=True, exist_ok=True)
    tasks: list[dict] = []
    for band in bands:
        sig_path = native_dir / f"sig_band{band}.native.bin"
        hpcx_out = hpcx_dir / f"band{band}.hpcx"
        if hpcx_out.is_file() and not force:
            tasks.append({"band": band, "status": "skipped", "build_s": 0.0})
            continue
        tasks.append(
            {
                "band": band,
                "sig_path": str(sig_path),
                "hpcx_out": str(hpcx_out),
            }
        )

    build_tasks = [t for t in tasks if "hpcx_out" in t]
    t0 = time.perf_counter()

    def _cmd(task: dict) -> list[str]:
        return [
            str(HPCX_CLI),
            "--from-sig",
            task["sig_path"],
            "--run-dir",
            str(run_dir),
            "--band",
            str(task["band"]),
            "--out",
            task["hpcx_out"],
        ]

    def _parse(stdout: str, task: dict) -> dict:
        meta = _parse_cli_json(stdout)
        build_s = float(meta.get("build_s", meta.get("seconds", 0.0)))
        return {
            "status": "built",
            "build_s": round(build_s, 3),
            "write_s_excluded": round(float(meta.get("write_s", 0.0)), 3),
            "hpcx": task["hpcx_out"],
        }

    built = parallel_cli_subprocess(build_tasks, _cmd, workers=workers, parse_stdout=_parse)
    skipped = [t for t in tasks if t.get("status") == "skipped"]
    results = built + skipped
    results.sort(key=lambda r: int(r["band"]))
    built_ok = [r for r in results if r.get("status") == "built"]
    compute_s = round(sum(float(r.get("build_s", 0.0)) for r in built_ok), 3)
    release_memory()
    return {
        "execution_model": "per_band_cli_subprocess",
        "wall_s": round(time.perf_counter() - t0, 3),
        "compute_s": compute_s,
        "sum_band_build_s": compute_s,
        "sum_band_write_s_excluded": round(sum(float(r.get("write_s_excluded", 0.0)) for r in built_ok), 3),
        "bands": results,
        "built": len(built_ok),
        "skipped": sum(1 for r in results if r.get("status") == "skipped"),
        "errors": [r for r in results if r.get("status") not in ("built", "skipped")],
    }


def run_search(
    *,
    run_dir: Path,
    hpcx_dir: Path,
    query_file: Path,
    bands: list[int],
    workers: int,
    n_queries: int,
    min_hits: int,
) -> dict[str, Any]:
    # --- Excluded setup (not in search timer) ---
    t_setup0 = time.perf_counter()
    dict_path = dict_native_path(run_dir)
    refs_dir = native_artifacts_dir(run_dir)
    if not dict_path.is_file():
        raise FileNotFoundError(f"missing dict: {dict_path}")
    queries = json.loads(query_file.read_text())
    qids = list(queries.keys())[: max(1, n_queries)]

    for band in bands:
        hpcx = hpcx_dir / f"band{band}.hpcx"
        if not hpcx.is_file():
            raise FileNotFoundError(f"missing {hpcx}")

    setup_s = round(time.perf_counter() - t_setup0, 3)

    subprocess.run(["make", "-C", str(HPCX_CPP), "hpcx_multiband_pool_cli"], check=True)

    preprocess_s = 0.0
    search_parallel_s = 0.0
    raw_cand_total = 0
    per_query: list[dict] = []
    pool_ready: dict[str, Any] = {}

    with HpcxSearchPool(
        hpcx_dir=hpcx_dir,
        refs_dir=refs_dir,
        dict_path=dict_path,
        bands=bands,
        workers=workers,
        min_hits=min_hits,
        pool_cli=HPCX_POOL,
    ) as pool:
        load_s = pool.load_s
        pool_ready = dict(pool.load_info)

        t_search0 = time.perf_counter()
        for qid in qids:
            rep = pool.search_tokens(list(queries[qid]))
            q_preprocess_s = float(rep.get("preprocess_s", 0.0))
            q_parallel_s = float(rep.get("search_wall_s", 0.0))
            preprocess_s += q_preprocess_s
            search_parallel_s += q_parallel_s
            q_cands = int(rep.get("n_candidates", len(rep.get("candidates", []))))
            raw_cand_total += q_cands
            per_query.append(
                {
                    "qid": qid,
                    "preprocess_s": round(q_preprocess_s, 6),
                    "search_parallel_s": round(q_parallel_s, 6),
                    "search_s": round(q_parallel_s, 6),
                    "raw_cands": q_cands,
                }
            )

        search_wall_s = round(time.perf_counter() - t_search0, 3)

    release_memory()

    return {
        "execution_model": "cpp_preprocess_encode_resident_hpc_pool",
        "setup_excluded_s": setup_s,
        "load_s_excluded": round(load_s, 3),
        "preprocess_s": round(preprocess_s, 6),
        "search_parallel_s": round(search_parallel_s, 6),
        "search_total_s": round(search_parallel_s, 6),
        "search_wall_s": search_wall_s,
        "n_queries": len(qids),
        "raw_cand_total": raw_cand_total,
        "pool_ready": pool_ready,
        "per_query": per_query,
        "per_query_sample": per_query[:5],
        "note": (
            "search_total_s = sum_q(search_parallel_s); parallel HPC search + k-way merge; "
            "excluded: load_s_excluded, preprocess_s, query JSON load, query-set generation"
        ),
    }


def main() -> int:
    p = argparse.ArgumentParser(description="C++ pipeline scalability test.")
    p.add_argument("--run-dir", type=Path, default=SCAL / "work/artifacts/N60000")
    p.add_argument("--hpcx-dir", type=Path, default=None)
    p.add_argument("--target-file", type=Path, default=PROJ / "data/target_60000.json")
    p.add_argument(
        "--query-file",
        type=Path,
        default=SCAL / "queries/queries_100_topTokenFreq_N60000.json",
    )
    p.add_argument("--bands", type=str, default="0-24")
    p.add_argument("--n-bands", type=int, default=25)
    p.add_argument("--workers", type=int, default=6, help="default parallelism for signature, HPC, and search")
    p.add_argument("--sig-workers", type=int, default=None, help="signature build workers (default: --workers)")
    p.add_argument(
        "--hpc-workers",
        type=int,
        default=None,
        help="parallelism for HPC build (default: same as --workers)",
    )
    p.add_argument("--search-workers", type=int, default=None, help="search workers (default: --workers)")
    p.add_argument("--n-queries", type=int, default=100)
    p.add_argument("--tau", type=float, default=0.5)
    p.add_argument("--min-hits", type=int, default=5)
    p.add_argument("--num-perm", type=int, default=5, help="LSH rows per band (r); stored in cybuf")
    p.add_argument(
        "--reuse-existing",
        action="store_true",
        help="skip prep/sig/hpc when outputs already exist (default: rebuild and overwrite)",
    )
    p.add_argument("--force", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--skip-build", action="store_true")
    p.add_argument("--skip-prep", action="store_true", help="reuse native prep under sig_cpp/refs; still count its timing")
    p.add_argument("--skip-search", action="store_true")
    p.add_argument(
        "--large-target-prep",
        action="store_true",
        help="prep via tools/prep_large_target_native.py (48M+ target JSON; sig/hpc still C++)",
    )
    p.add_argument(
        "--json-out",
        type=Path,
        default=SCAL / "work/results/scalability_cpp_HPC-LSH_N60000.json",
    )
    args = p.parse_args()
    force = not args.reuse_existing

    apply_v8_phase1_d2_env()
    global HPCX_CLI
    HPCX_CLI = Path(os.environ.get("HPCX_BUILD_CLI", str(HPCX_BUILD_V8_WRAPPER)))

    _ensure_binaries()
    bands = _parse_bands(args.bands, args.n_bands)
    native_dir = native_artifacts_dir(args.run_dir)
    hpcx_dir = args.hpcx_dir if args.hpcx_dir is not None else hpcx_artifacts_dir(args.run_dir)
    native_dir.mkdir(parents=True, exist_ok=True)
    hpcx_dir.mkdir(parents=True, exist_ok=True)

    params_path = args.run_dir / "hpc_params.json"
    n_targets = 1500000
    if params_path.is_file():
        n_targets = int(json.loads(params_path.read_text()).get("n_targets", n_targets))
    sig_workers = args.sig_workers if args.sig_workers is not None else args.workers
    search_workers = args.search_workers if args.search_workers is not None else args.workers
    hpc_workers = args.hpc_workers if args.hpc_workers is not None else args.workers

    pipeline_version = os.environ.get("HPC_PIPELINE_VERSION", PIPELINE_VERSION)

    report: dict[str, Any] = {
        "pipeline_version": pipeline_version,
        "method": "HPC-LSH-cpp",
        "n_targets": n_targets,
        "tau": args.tau,
        "workers": args.workers,
        "sig_workers": sig_workers,
        "hpc_workers": hpc_workers,
        "search_workers": search_workers,
        "query_file": str(args.query_file),
        "run_dir": str(args.run_dir),
        "target_file": str(args.target_file),
        "native_dir": str(native_dir),
        "hpcx_dir": str(hpcx_dir),
        "bands": bands,
        "n_bands": len(bands),
        "num_perm": args.num_perm,
        "timing_policy": {
            "build_includes": ["dict_s", "inver_s", "cybuf_s", "signature_compute_s", "hpc_compute_s"],
            "build_excludes": ["signature_write_s", "hpc_write_s"],
            "search_includes": ["search_parallel_s"],
            "search_excludes": [
                "load_s_excluded",
                "preprocess_s",
                "setup_excluded_s",
                "query_file_load",
                "query_set_generation",
            ],
        },
    }

    build_total_s = 0.0

    if not args.skip_build:
        prep_label = (
            "=== PREP: large target (Python json.loads → native bin) ==="
            if args.large_target_prep
            else "=== PREP: dict + invert + cybuf (C++) ==="
        )
        print(prep_label, flush=True)
        with MemorySampler() as mem_prep:
            if args.skip_prep:
                prep = _load_existing_prep(native_dir)
            elif args.large_target_prep:
                prep = run_prep_large_target(
                    target_file=args.target_file,
                    out_dir=native_dir,
                )
            else:
                prep = run_prep(
                    run_dir=args.run_dir,
                    target_file=args.target_file,
                    out_dir=native_dir,
                    force=force,
                    num_perm=args.num_perm,
                )
        report["prep"] = prep
        report["prep"]["memory"] = mem_prep.summary()
        if prep["timing"].get("prep_total_s", 0.0) == 0.0:
            pt = native_dir / "prep_timing.json"
            if pt.is_file():
                meta = json.loads(pt.read_text())
                prep["timing"] = meta.get("timing", prep["timing"])
                prep["prep_backend"] = meta.get("prep_backend")
                prep["n_columns"] = meta.get("n_columns")
                prep["n_tokens"] = meta.get("n_tokens")
                report["prep"] = prep
        build_total_s += prep["timing"]["prep_total_s"]

        print("=== BUILD: parallel signatures (workers=%d) ===" % sig_workers, flush=True)
        with MemorySampler() as mem_sig:
            sig_rep = run_parallel_signatures(
                bands=bands,
                cybuf=Path(prep["cybuf"]),
                out_dir=native_dir,
                workers=sig_workers,
                force=force,
            )
        release_memory()
        report["signature_build"] = sig_rep
        report["signature_build"]["memory"] = mem_sig.summary()
        build_total_s += sig_rep["compute_s"]
        if sig_rep["errors"]:
            print("signature errors:", sig_rep["errors"][:3], file=sys.stderr)

        print("=== BUILD: parallel HPC (workers=%d) ===" % hpc_workers, flush=True)
        with MemorySampler() as mem_hpc:
            hpc_rep = run_parallel_hpc(
                bands=bands,
                run_dir=args.run_dir,
                native_dir=native_dir,
                hpcx_dir=hpcx_dir,
                workers=hpc_workers,
                force=force,
            )
        release_memory()
        report["hpc_build"] = hpc_rep
        report["hpc_build"]["memory"] = mem_hpc.summary()
        build_total_s += hpc_rep["compute_s"]
        if hpc_rep["errors"]:
            print("hpc errors:", hpc_rep["errors"][:3], file=sys.stderr)

    report["build_s"] = round(build_total_s, 3)
    if not args.skip_build and "prep" in report:
        report["build_breakdown_s"] = {
            "dict_s": report["prep"]["timing"]["dict_s"],
            "inver_s": report["prep"]["timing"]["inver_s"],
            "cybuf_s": report["prep"]["timing"]["cybuf_s"],
            "signature_compute_s": report.get("signature_build", {}).get("compute_s", 0.0),
            "hpc_compute_s": report.get("hpc_build", {}).get("compute_s", 0.0),
            "total_s": round(build_total_s, 3),
        }

    if not args.skip_search:
        print("=== SEARCH: parallel HPC band search (preprocess/load excluded) ===", flush=True)
        with MemorySampler() as mem_search:
            search_rep = run_search(
                run_dir=args.run_dir,
                hpcx_dir=hpcx_dir,
                query_file=args.query_file,
                bands=bands,
                workers=search_workers,
                n_queries=args.n_queries,
                min_hits=args.min_hits,
            )
        report["search"] = search_rep
        report["search"]["memory"] = mem_search.summary()
        report["search_s"] = search_rep["search_total_s"]
        release_memory()

    report["status"] = "ok"
    report["execution_model"] = "phase_separated_with_memory_release"
    args.json_out.parent.mkdir(parents=True, exist_ok=True)
    args.json_out.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
