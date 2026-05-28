#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATE_TAG="${DATE_TAG:-$(date +%F)}"
RUN_DIR="$ROOT/build/v6_sift_$DATE_TAG"
mkdir -p "$RUN_DIR" "$ROOT/archive/results" "$ROOT/archive/logs" \
  "$ROOT/archive/configs" "$ROOT/archive/build_info"

: "${SIFT_BASE:?Set SIFT_BASE to base .fvecs path}"
: "${SIFT_QUERY:?Set SIFT_QUERY to query .fvecs path}"
: "${SIFT_TRUTH:?Set SIFT_TRUTH to official groundtruth .ivecs path}"

bash "$ROOT/scripts/linux/build.sh"
bash "$ROOT/scripts/autodl/collect_env.sh" \
  "$ROOT/archive/build_info/v6-autodl-sift-$DATE_TAG.txt"

BASE_LIMIT="${BASE_LIMIT:-100000}"
QUERY_LIMIT="${QUERY_LIMIT:-1000}"
BUILD_INDEX="${BUILD_INDEX:-0}"
INDEX="${INDEX:-$RUN_DIR/v6_sift_packed_coaccess.idx}"
RESULT="$ROOT/archive/results/v6-autodl-sift-$DATE_TAG.txt"
LOG="$ROOT/archive/logs/v6-autodl-sift-$DATE_TAG.log"
CONFIG="$ROOT/archive/configs/v6-autodl-sift-$DATE_TAG.json"

BUILD_FLAG=()
if [[ "$BUILD_INDEX" == "1" ]]; then
  BUILD_FLAG=(--build-index)
fi

COMMON=(
  --engine graph
  --layout packed
  --packing coaccess
  --base "$SIFT_BASE"
  --query "$SIFT_QUERY"
  --truth "$SIFT_TRUTH"
  --base-limit "$BASE_LIMIT"
  --query-limit "$QUERY_LIMIT"
  --k "${K:-10}"
  --graph-degree "${GRAPH_DEGREE:-16}"
  --search-width "${SEARCH_WIDTH:-128}"
  --entry-count "${ENTRY_COUNT:-48}"
  --routing-sample-count "${ROUTING_SAMPLE_COUNT:-512}"
  --coaccess-sessions "${COACCESS_SESSIONS:-96}"
  --coaccess-trace-length "${COACCESS_TRACE_LENGTH:-48}"
  --cache-policy agent
  --cache-pages "${CACHE_PAGES:-256}"
  --path-cache-policy reuse
  --path-cache-capacity "${PATH_CACHE_CAPACITY:-1024}"
  --path-cache-hit-search-width "${PATH_CACHE_HIT_SEARCH_WIDTH:-96}"
  --workload-mode mixed
  --operation-count "${OPERATION_COUNT:-1200}"
  --write-ratio "${WRITE_RATIO:-10}"
  --delta-compaction-threshold "${DELTA_COMPACTION_THRESHOLD:-64}"
  --compaction-batch-size "${COMPACTION_BATCH_SIZE:-16}"
  --compaction-work-us 0
  --compaction-io-mode file
  --compaction-io-bytes-per-vector "${COMPACTION_IO_BYTES_PER_VECTOR:-65536}"
  --run-type warm
  --warmup-runs "${WARMUP_RUNS:-1}"
  --index "$INDEX"
  "${BUILD_FLAG[@]}"
)

{
  echo "variant=aggressive_file_io"
  "$ROOT/build/agentmem_flow" "${COMMON[@]}" \
    --compaction-policy aggressive \
    --wal "$RUN_DIR/v6_sift_aggressive.wal" \
    --compaction-io-path "$RUN_DIR/v6_sift_aggressive_compaction.bin"

  echo "variant=sla_file_io"
  "$ROOT/build/agentmem_flow" "${COMMON[@]}" \
    --compaction-policy sla \
    --sla-p99-ms "${SLA_P99_MS:-2.0}" \
    --wal "$RUN_DIR/v6_sift_sla.wal" \
    --compaction-io-path "$RUN_DIR/v6_sift_sla_compaction.bin"
} 2>&1 | tee "$RESULT" | tee "$LOG" >/dev/null

cat > "$CONFIG" <<JSON
{
  "version": "v6",
  "date": "$DATE_TAG",
  "environment": "AutoDL/Linux target",
  "script": "scripts/autodl/run_v6_sift_template.sh",
  "dataset": {
    "base": "$SIFT_BASE",
    "query": "$SIFT_QUERY",
    "truth": "$SIFT_TRUTH",
    "base_limit": $BASE_LIMIT,
    "query_limit": $QUERY_LIMIT,
    "ground_truth": "official ivecs"
  },
  "index": {
    "path": "$INDEX",
    "build_index": "$BUILD_INDEX",
    "note": "Current exact graph builder is O(N^2). For SIFT100K/SIFT1M, prefer a prebuilt index or run smaller BUILD_INDEX=1 experiments first."
  },
  "compaction": {
    "io_mode": "file",
    "io_bytes_per_vector": ${COMPACTION_IO_BYTES_PER_VECTOR:-65536},
    "policies": ["aggressive", "sla"]
  }
}
JSON

echo "Result: $RESULT"
echo "Log: $LOG"
echo "Config: $CONFIG"
