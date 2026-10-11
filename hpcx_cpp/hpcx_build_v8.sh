#!/usr/bin/env bash
# HPC index entry. Compiles hpcx_build_cli from sources in this directory, then runs it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
if [[ -f "$REPO/env/source_v8_phase1_d2.sh" ]]; then
  # shellcheck source=/dev/null
  source "$REPO/env/source_v8_phase1_d2.sh"
fi
if [[ ! -x "$ROOT/hpcx_build_cli" ]]; then
  make -C "$ROOT" hpcx_build_cli
fi
exec "$ROOT/hpcx_build_cli" "$@"
