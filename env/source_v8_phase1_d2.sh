#!/usr/bin/env bash
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$REPO/env/hpc_v8_phase1_d2.env"

export HPC_LSH_REPO="$REPO"
export TMPDIR="${TMPDIR:-$REPO/work/tmp}"
export HPC_LSH_SCRATCH="${HPC_LSH_SCRATCH:-$REPO/work/scratch}"
export HPCX_TMPDIR="$TMPDIR"
mkdir -p "$TMPDIR" "$HPC_LSH_SCRATCH" "$REPO/work/results" "$REPO/work/artifacts"

export SIG_CYBUF_MMAP="${SIG_CYBUF_MMAP:-1}"
export SIG_CYBUF_VECTOR="${SIG_CYBUF_VECTOR:-0}"
export SIG_LAZY_INVER="${SIG_LAZY_INVER:-1}"
export SIG_INVER_MMAP="${SIG_INVER_MMAP:-1}"
export SIG_STREAM_COLUMNS="${SIG_STREAM_COLUMNS:-1}"
export SIG_STREAM_BATCH_SIZE="${SIG_STREAM_BATCH_SIZE:-100000}"
