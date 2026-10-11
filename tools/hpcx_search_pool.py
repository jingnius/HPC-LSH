"""Long-lived HPCX search pool: C++ dict + encode + 25-band resident HPC search."""

from __future__ import annotations

import json
import subprocess
import threading
from pathlib import Path
from typing import Any



def _stderr_drainer(proc: subprocess.Popen[str]) -> None:
    """Prevent pool from blocking when stderr pipe fills (OpenMP / profile logs)."""
    if proc.stderr is None:
        return
    try:
        for _ in proc.stderr:
            pass
    except (OSError, ValueError):
        pass


class HpcxSearchPool:
    """One C++ process: load dict/sample-orders/HPCX once; per-query preprocess+encode+parallel HPC search."""

    def __init__(
        self,
        *,
        hpcx_dir: Path,
        refs_dir: Path,
        dict_path: Path,
        bands: list[int],
        workers: int,
        min_hits: int,
        pool_cli: Path,
    ) -> None:
        self.bands = list(bands)
        self.workers = workers
        self.min_hits = min_hits
        self.proc = subprocess.Popen(
            [
                str(pool_cli),
                "--serve",
                "--hpcx-dir",
                str(hpcx_dir),
                "--refs-dir",
                str(refs_dir),
                "--dict",
                str(dict_path),
                "--bands",
                _bands_spec(bands),
                "--workers",
                str(workers),
                "--min-hits",
                str(min_hits),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        init_line = self.proc.stdout.readline()
        if not init_line:
            err = self.proc.stderr.read() if self.proc.stderr else ""
            raise RuntimeError(f"hpcx pool failed to start: {err}")
        self.load_info: dict[str, Any] = json.loads(init_line)
        if self.load_info.get("status") != "ready":
            raise RuntimeError(f"hpcx pool init failed: {self.load_info}")
        self._stderr_thread = threading.Thread(
            target=_stderr_drainer, args=(self.proc,), name="hpcx-pool-stderr", daemon=True
        )
        self._stderr_thread.start()

    @property
    def load_s(self) -> float:
        return float(self.load_info.get("load_s", 0.0))

    def search_tokens(self, tokens: list[str], *, counts_only: bool = False) -> dict[str, Any]:
        assert self.proc.stdin is not None
        assert self.proc.stdout is not None
        req: dict[str, Any] = {"tokens": tokens}
        if counts_only:
            req["counts_only"] = True
        self.proc.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            err = self.proc.stderr.read() if self.proc.stderr else ""
            raise RuntimeError(f"hpcx pool search failed: {err}")
        rep = json.loads(line)
        if rep.get("status") != "ok":
            raise RuntimeError(rep.get("error", rep))
        return rep

    def close(self) -> None:
        if self.proc.stdin and self.proc.poll() is None:
            try:
                self.proc.stdin.write(json.dumps({"cmd": "shutdown"}) + "\n")
                self.proc.stdin.flush()
                self.proc.stdout.readline()
            except OSError:
                pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def __enter__(self) -> HpcxSearchPool:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()


def _bands_spec(bands: list[int]) -> str:
    if not bands:
        return "0-0"
    bands = sorted(bands)
    if bands == list(range(bands[0], bands[-1] + 1)):
        return f"{bands[0]}-{bands[-1]}"
    return ",".join(str(b) for b in bands)
