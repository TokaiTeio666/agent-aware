#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_LOCAL="$ROOT/scripts/linux/run_sift_local.sh"
INDEX_DIR="${INDEX_DIR:-$ROOT/build/sift1m_indexes}"
BASE_LIMIT="${BASE_LIMIT:-1000000}"
QUERY_LIMIT="${QUERY_LIMIT:-10000}"
WARMUP_RUNS="${WARMUP_RUNS:-1}"
SEARCH_WIDTHS=(${SEARCH_WIDTHS:-128 256 512 1024})
DROP_OS_CACHE="${DROP_OS_CACHE:-0}"
IO_MODE="${IO_MODE:-pread}"

mkdir -p "$INDEX_DIR"

drop_os_cache() {
  if [[ "$DROP_OS_CACHE" != "1" ]]; then
    echo "cache_drop_status=skipped"
    return
  fi
  if ! command -v sudo >/dev/null 2>&1 || ! sudo -n true 2>/dev/null; then
    echo "cache_drop_status=unavailable_no_passwordless_sudo"
    return
  fi
  sync
  sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches'
  echo "cache_drop_status=completed"
}

run_exact() {
  local cache_mode="$1"
  local tag="sift1m-exact-${cache_mode}-$(date +%Y%m%d-%H%M%S)"
  BASE_LIMIT="$BASE_LIMIT" QUERY_LIMIT="$QUERY_LIMIT" ENGINE=exact \
    DATASET_NAME=sift1m RUN_CACHE_MODE="$cache_mode" IO_MODE="$IO_MODE" \
    DATE_TAG="$tag" bash "$RUN_LOCAL"
}

run_graph() {
  local cache_mode="$1"
  local width="$2"
  local build_index="$3"
  local warmups="$4"
  local index="$INDEX_DIR/sift1m-${GRAPH_BUILD_POLICY:-lsh-rp}.idx"
  local tag="sift1m-graph-w${width}-${cache_mode}-$(date +%Y%m%d-%H%M%S)"
  BASE_LIMIT="$BASE_LIMIT" QUERY_LIMIT="$QUERY_LIMIT" ENGINE=graph \
    DATASET_NAME=sift1m RUN_CACHE_MODE="$cache_mode" IO_MODE="$IO_MODE" \
    INDEX="$index" BUILD_INDEX="$build_index" SEARCH_WIDTH="$width" \
    SEARCH_EARLY_STOP=1 SEARCH_EARLY_STOP_MIN="${SEARCH_EARLY_STOP_MIN:-64}" \
    WARMUP_RUNS="$warmups" DATE_TAG="$tag" bash "$RUN_LOCAL"
}

drop_os_cache
run_exact cold
for ((i = 0; i < WARMUP_RUNS; ++i)); do
  run_exact warmup
done
run_exact warm

build_index=1
for width in "${SEARCH_WIDTHS[@]}"; do
  drop_os_cache
  run_graph cold "$width" "$build_index" 0
  build_index=0
  run_graph warm "$width" 0 "$WARMUP_RUNS"
done

python3 "$ROOT/scripts/linux/summarize_sift_results.py"

