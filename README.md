# HPC-LSH

Default pipeline `cpp-default-v8-phase1-d2-src` (One-Hot Signature, HPC tree, local reduction, multi-key routes). Parallelism is 1: `DICT_BUILD_THREADS=1`, and signature, HPC, and search workers are 1.

This tree contains the 60,000-column target and three query sets used by the script below. Each query file has 1,000 queries. The names 100, 1k, and 10k are the query-size labels.

## Data

The 60K target, queries, and ground truth are in `data/`.

The larger target repositories, labeled 1.5M, 3M, 6M, 12M, 24M, and 48M, are published on Zenodo: [https://doi.org/10.5281/zenodo.23301030]. Each file is a JSON object whose values are web-table columns, stored as sets of distinct cell values. The labels are rounded sizes. The column counts are 1,499,926; 2,992,889; 5,986,027; 11,971,763; 23,943,730; and 47,888,035.

## Requirements

Linux, `g++` with OpenMP, `make`, `python3`, `python3-config` (headers), `zlib`, and `datasketch`.

## Run

```bash
bash scripts/run_60k_prf.sh
```

The script compiles the C++ tools from source. For each containment threshold from 0.1 through 0.9 it asks MinHash LSH for `(b, r)` at `num_perm=128`, rebuilds the signatures with `b` bands and `r` rows per band, and searches with `min_hits=r`. Precision, recall, and F1 are macro averages over queries whose ground truth is nonempty. Containment is `|Q ∩ R| / |R|`.

Results are written to `work/results/prf_60k_w1_summary.json`. Generated indexes under `work/` and `sig_cpp/refs/` are local build products.
