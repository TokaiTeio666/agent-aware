#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_LOCAL="$ROOT/scripts/linux/run_sift_local.sh"
INDEX_DIR="${INDEX_DIR:-$ROOT/build/sift_io_indexes}"
mkdir -p "$INDEX_DIR"

QUERY_LIMIT="${QUERY_LIMIT:-1000}"
BASE_LIMIT="${BASE_LIMIT:-0}"
SEARCH_WIDTH="${SEARCH_WIDTH:-1024}"
IO_MODES=(${IO_MODES:-pread odirect io_uring})
INDEX_PATH="${INDEX_PATH:-$INDEX_DIR/sift-io.idx}"

build_index="${BUILD_INDEX:-1}"
for mode in "${IO_MODES[@]}"; do
  tag="io-${mode}-$(date +%Y%m%d-%H%M%S)"
  ENGINE="${ENGINE:-graph}" \
  DATASET_NAME="${DATASET_NAME:-sift1m}" \
  BASE_LIMIT="$BASE_LIMIT" \
  QUERY_LIMIT="$QUERY_LIMIT" \
  INDEX_PATH="$INDEX_PATH" \
  BUILD_INDEX="$build_index" \
  REUSE_INDEX="$([[ "$build_index" == "0" ]] && echo 1 || echo 0)" \
  SEARCH_WIDTH="$SEARCH_WIDTH" \
  IO_MODE="$mode" \
  IO_DEPTH="${IO_DEPTH:-32}" \
  IO_BATCH_SIZE="${IO_BATCH_SIZE:-32}" \
  PREFETCH_WIDTH="${PREFETCH_WIDTH:-32}" \
  PREFETCH_POLICY="${PREFETCH_POLICY:-frontier-next-hop}" \
  ALLOW_IO_FALLBACK="${ALLOW_IO_FALLBACK:-1}" \
  RUN_TYPE="${RUN_TYPE:-dev}" \
  DATE_TAG="$tag" \
    bash "$RUN_LOCAL"
  build_index=0
done

python3 "$ROOT/scripts/linux/summarize_sift_results.py"
