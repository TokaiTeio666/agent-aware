#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_LOCAL="$ROOT/scripts/linux/run_sift_local.sh"
INDEX_DIR="${INDEX_DIR:-$ROOT/build/sift_cache_indexes}"
mkdir -p "$INDEX_DIR"

QUERY_LIMIT="${QUERY_LIMIT:-1000}"
BASE_LIMIT="${BASE_LIMIT:-0}"
SEARCH_WIDTH="${SEARCH_WIDTH:-1024}"
CACHE_POLICIES=(${CACHE_POLICIES:-none lru 2q graph-aware-2q})
AGENT_WORKLOADS=(${AGENT_WORKLOADS:-random hotspot session-local})
INDEX_PATH="${INDEX_PATH:-$INDEX_DIR/sift-cache.idx}"

build_index="${BUILD_INDEX:-1}"
for workload in "${AGENT_WORKLOADS[@]}"; do
  for policy in "${CACHE_POLICIES[@]}"; do
    tag="cache-${workload}-${policy}-$(date +%Y%m%d-%H%M%S)"
    ENGINE="${ENGINE:-graph}" \
    DATASET_NAME="${DATASET_NAME:-sift1m}" \
    BASE_LIMIT="$BASE_LIMIT" \
    QUERY_LIMIT="$QUERY_LIMIT" \
    INDEX_PATH="$INDEX_PATH" \
    BUILD_INDEX="$build_index" \
    REUSE_INDEX="$([[ "$build_index" == "0" ]] && echo 1 || echo 0)" \
    SEARCH_WIDTH="$SEARCH_WIDTH" \
    CACHE_POLICY="$policy" \
    CACHE_PAGES="${CACHE_PAGES:-4096}" \
    CACHE_BUDGET_BYTES="${CACHE_BUDGET_BYTES:-0}" \
    CACHE_PROTECT_HOT_PAGES="${CACHE_PROTECT_HOT_PAGES:-1}" \
    CACHE_HOT_DEGREE_THRESHOLD="${CACHE_HOT_DEGREE_THRESHOLD:-16}" \
    PATH_CACHE_POLICY="${PATH_CACHE_POLICY:-none}" \
    AGENT_WORKLOAD="$workload" \
    RUN_TYPE="${RUN_TYPE:-dev}" \
    DATE_TAG="$tag" \
      bash "$RUN_LOCAL"
    build_index=0
  done
done

python3 "$ROOT/scripts/linux/summarize_sift_results.py"
