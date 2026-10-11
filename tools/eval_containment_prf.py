"""Macro P/R/F1 vs joinable ground truth (containment labels under HPC-LSH/data only)."""

from __future__ import annotations

import json
import statistics
from pathlib import Path
from typing import Any


def _prf(tp: int, fp: int, fn: int) -> tuple[float, float, float]:
    prec = tp / (tp + fp) if (tp + fp) else 0.0
    rec = tp / (tp + fn) if (tp + fn) else 0.0
    f1 = 2 * prec * rec / (prec + rec) if (prec + rec) else 0.0
    return prec, rec, f1


def eval_containment_prf(
    candidates: dict[str, set[str]],
    qids: list[str],
    gt_file: Path,
    tau: float = 0.5,
) -> dict[str, Any]:
    gt_path = Path(gt_file)
    root = json.loads(gt_path.read_text())
    if "joinable_by_threshold" not in root:
        raise ValueError(f"expected joinable_by_threshold in {gt_path}")
    tau_key = f"{tau:.1f}"
    if tau_key not in root["joinable_by_threshold"]:
        raise KeyError(f"threshold {tau_key} missing in {gt_path}")
    gt_at = root["joinable_by_threshold"][tau_key]

    tp = fp = fn = raw = 0
    tp_nz = fp_nz = fn_nz = 0
    per_query_ps: list[float] = []
    per_query_rs: list[float] = []
    n_empty_gt = 0

    for qid in qids:
        cs = {str(c) for c in candidates.get(qid, set())}
        gs = {str(x) for x in gt_at.get(qid, [])}
        t = len(cs & gs)
        f = len(cs - gs)
        n = len(gs - cs)
        tp += t
        fp += f
        fn += n
        raw += len(cs)
        if not gs:
            n_empty_gt += 1
            continue
        tp_nz += t
        fp_nz += f
        fn_nz += n
        per_query_ps.append(t / len(cs) if cs else 0.0)
        per_query_rs.append(t / len(gs))

    micro_all_p, micro_all_r, micro_all_f1 = _prf(tp, fp, fn)
    micro_gt_p, micro_gt_r, micro_gt_f1 = _prf(tp_nz, fp_nz, fn_nz)
    macro_p = statistics.mean(per_query_ps) if per_query_ps else 0.0
    macro_r = statistics.mean(per_query_rs) if per_query_rs else 0.0
    macro_f1 = 2 * macro_p * macro_r / (macro_p + macro_r) if (macro_p + macro_r) else 0.0

    return {
        "tau": tau,
        "gt_file": str(gt_path.resolve()),
        "aggregation": {
            "primary": "macro_gt_nonempty",
            "n_queries_total": len(qids),
            "n_gt_nonempty": len(per_query_ps),
            "n_empty_gt_skipped_for_macro": n_empty_gt,
        },
        "macro": {
            "precision": macro_p,
            "recall": macro_r,
            "f1": macro_f1,
        },
        "micro_all_queries": {
            "tp": tp,
            "fp": fp,
            "fn": fn,
            "raw_cand_total": raw,
            "precision": micro_all_p,
            "recall": micro_all_r,
            "f1": micro_all_f1,
        },
        "micro_gt_nonempty": {
            "tp": tp_nz,
            "fp": fp_nz,
            "fn": fn_nz,
            "precision": micro_gt_p,
            "recall": micro_gt_r,
            "f1": micro_gt_f1,
        },
        "precision": macro_p,
        "recall": macro_r,
        "f1": macro_f1,
        "raw_cand_total": raw,
    }
