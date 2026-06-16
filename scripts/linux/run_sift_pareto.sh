#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_LOCAL="$ROOT/scripts/linux/run_sift_local.sh"
INDEX_DIR="${INDEX_DIR:-$ROOT/build/sift_pareto_indexes}"
mkdir -p "$INDEX_DIR"

QUERY_LIMIT="${QUERY_LIMIT:-1000}"
BASE_LIMIT="${BASE_LIMIT:-0}"
SEARCH_WIDTHS=(${SEARCH_WIDTHS:-768 1024 1280 1536})
EARLY_STOP_MINS=(${EARLY_STOP_MINS:-512 768 1024})
INDEX_PATH="${INDEX_PATH:-$INDEX_DIR/sift-pareto.idx}"

build_index="${BUILD_INDEX:-1}"
for width in "${SEARCH_WIDTHS[@]}"; do
  for min_expansions in "${EARLY_STOP_MINS[@]}"; do
    tag="pareto-w${width}-e${min_expansions}-$(date +%Y%m%d-%H%M%S)"
    ENGINE="${ENGINE:-graph}" \
    DATASET_NAME="${DATASET_NAME:-sift1m}" \
    BASE_LIMIT="$BASE_LIMIT" \
    QUERY_LIMIT="$QUERY_LIMIT" \
    INDEX_PATH="$INDEX_PATH" \
    BUILD_INDEX="$build_index" \
    REUSE_INDEX="$([[ "$build_index" == "0" ]] && echo 1 || echo 0)" \
    SEARCH_WIDTH="$width" \
    EARLY_STOP="${EARLY_STOP:-1}" \
    EARLY_STOP_MIN="$min_expansions" \
    RUN_TYPE="${RUN_TYPE:-dev}" \
    DATE_TAG="$tag" \
      bash "$RUN_LOCAL"
    build_index=0
  done
done

python3 "$ROOT/scripts/linux/summarize_sift_results.py"
