# HPC-LSH

Default pipeline `cpp-default-v8-phase1-d2-src` (One-Hot Signature, HPC tree, local reduction, multi-key routes). Parallelism is 1: `DICT_BUILD_THREADS=1`, and signature, HPC, and search workers are 1.

This tree contains the 60,000-column target and three query sets. Each query file has 1,000 queries. The names 100, 1k, and 10k are the query-size labels.

## Requirements

Linux, `g++` with OpenMP, `make`, `python3`, `python3-config` (headers), `zlib`, and `datasketch`.

## Run

```bash
bash scripts/run_60k_prf.sh
```

The script compiles the C++ tools from source. For each containment threshold from 0.1 through 0.9 it asks MinHash LSH for `(b, r)` at `num_perm=128`, rebuilds the signatures with `b` bands and `r` rows per band, and searches with `min_hits=r`. At threshold 0.5 this is `b=25`, `r=5`, `min_hits=5`. Precision, recall, and F1 are macro averages over queries whose ground truth is nonempty. Containment is `|Q ∩ R| / |R|`.

Results are written to `work/results/prf_60k_w1_summary.json`. Generated indexes under `work/` and `sig_cpp/refs/` are local build products.
