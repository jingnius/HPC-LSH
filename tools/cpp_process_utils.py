"""Subprocess helpers: one fresh process per band/task, release memory between phases."""

from __future__ import annotations

import gc
import json
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Callable

REPO = Path(__file__).resolve().parents[1]


def release_memory() -> None:
    gc.collect()


def parallel_cli_subprocess(
    tasks: list[dict],
    build_cmd: Callable[[dict], list[str]],
    *,
    workers: int,
    parse_stdout: Callable[[str, dict], dict] | None = None,
) -> list[dict]:
    """Run each task in a fresh CLI subprocess (thread pool waits only; CLI exits fully)."""
    if not tasks:
        release_memory()
        return []

    workers = max(1, min(workers, len(tasks)))

    def _one(task: dict) -> dict:
        cmd = build_cmd(task)
        try:
            from cpu_affinity_util import maybe_taskset_cmd

            cmd = maybe_taskset_cmd(cmd)
        except ImportError:
            pass
        t0 = time.perf_counter()
        try:
            proc = subprocess.run(cmd, check=True, capture_output=True, text=True)
            extra = parse_stdout(proc.stdout, task) if parse_stdout else {}
            return {
                **task,
                "status": extra.get("status", "built"),
                "wall_s": round(time.perf_counter() - t0, 3),
                **{k: v for k, v in extra.items() if k != "status"},
            }
        except subprocess.CalledProcessError as exc:
            err = (exc.stderr or exc.stdout or str(exc))[-800:]
            return {
                **task,
                "status": "error",
                "error": err,
                "wall_s": round(time.perf_counter() - t0, 3),
            }

    results: list[dict] = []
    if workers <= 1 or len(tasks) == 1:
        for task in tasks:
            results.append(_one(task))
    else:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = [pool.submit(_one, task) for task in tasks]
            for fut in as_completed(futs):
                results.append(fut.result())

    release_memory()
    return results


def spawn_python_worker(
    script: Path,
    task: dict,
    *,
    worker_flag: str = "--band-worker",
) -> dict:
    """Fresh Python interpreter: run one band worker and read JSON result file."""
    out_path = Path(task.get("_result_path") or "")
    if not out_path:
        raise ValueError("task must include _result_path for spawn_python_worker")
    payload = {k: v for k, v in task.items() if not k.startswith("_")}
    payload["result_path"] = str(out_path)
    cmd = [sys.executable, str(script), worker_flag, json.dumps(payload)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    if not out_path.is_file():
        raise RuntimeError(f"missing worker result: {out_path}")
    try:
        return json.loads(out_path.read_text())
    finally:
        out_path.unlink(missing_ok=True)


def parallel_python_workers(
    tasks: list[dict],
    script: Path,
    *,
    workers: int,
    worker_flag: str = "--band-worker",
    tmp_dir: Path | None = None,
) -> list[dict]:
    """Each task in a fresh Python subprocess (exit after one band)."""
    if not tasks:
        release_memory()
        return []

    workers = max(1, min(workers, len(tasks)))
    tmp = tmp_dir or (REPO / "work/tmp")
    tmp.mkdir(parents=True, exist_ok=True)
    stamped: list[dict] = []
    for i, task in enumerate(tasks):
        stamped.append(
            {
                **task,
                "_result_path": str(tmp / f"cpp_worker_{script.stem}_{task.get('band', i)}_{time.time_ns()}.json"),
            }
        )

    def _one(task: dict) -> dict:
        t0 = time.perf_counter()
        try:
            rep = spawn_python_worker(script, task, worker_flag=worker_flag)
            rep.setdefault("wall_s", round(time.perf_counter() - t0, 3))
            return rep
        except Exception as exc:  # noqa: BLE001
            return {
                **{k: v for k, v in task.items() if not k.startswith("_")},
                "status": "error",
                "error": str(exc)[-800:],
                "wall_s": round(time.perf_counter() - t0, 3),
            }

    results: list[dict] = []
    if workers <= 1 or len(stamped) == 1:
        for task in stamped:
            results.append(_one(task))
    else:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = [pool.submit(_one, task) for task in stamped]
            for fut in as_completed(futs):
                results.append(fut.result())

    release_memory()
    return results
