#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/build/agentmem_flow"
DATE_TAG="${DATE_TAG:-$(date +%Y%m%d-%H%M%S)}"
RUN_DIR="$ROOT/build/sift_local_$DATE_TAG"
RESULT_DIR="$ROOT/archive/results"
LOG_DIR="$ROOT/archive/logs"

SIFT_DIR="${SIFT_DIR:-$ROOT/data/sift}"
SIFT_BASE="${SIFT_BASE:-$SIFT_DIR/sift_base.fvecs}"
SIFT_QUERY="${SIFT_QUERY:-$SIFT_DIR/sift_query.fvecs}"
SIFT_TRUTH="${SIFT_TRUTH:-$SIFT_DIR/sift_groundtruth.ivecs}"

ENGINE="${ENGINE:-exact}"
BASE_LIMIT="${BASE_LIMIT:-10000}"
QUERY_LIMIT="${QUERY_LIMIT:-100}"
K="${K:-10}"
PACKING_STRATEGY="${PACKING_STRATEGY:-${PACKING:-bfs}}"
RUN_CACHE_MODE="${RUN_CACHE_MODE:-${RUN_TYPE:-smoke}}"
IO_MODE="${IO_MODE:-pread}"
RUN_TYPE_EFFECTIVE="${RUN_TYPE:-$RUN_CACHE_MODE}"
MEMORY_BUDGET_RATIO="${MEMORY_BUDGET_RATIO:-0.20}"

mkdir -p "$RUN_DIR" "$RESULT_DIR" "$LOG_DIR"

for file in "$SIFT_BASE" "$SIFT_QUERY" "$SIFT_TRUTH"; do
  if [[ ! -f "$file" ]]; then
    echo "Missing SIFT file: $file" >&2
    echo "Set SIFT_DIR or SIFT_BASE/SIFT_QUERY/SIFT_TRUTH before running." >&2
    exit 1
  fi
done

if [[ ! -x "$BIN" ]]; then
  bash "$ROOT/scripts/linux/build.sh"
fi

if [[ "$BASE_LIMIT" != "0" && "$BASE_LIMIT" -lt 1000000 ]]; then
  echo "Note: official SIFT ground truth targets SIFT1M."
  echo "For a base subset, the binary will recompute exact truth when needed."
fi

COMMON=(
  --engine "$ENGINE"
  --base "$SIFT_BASE"
  --query "$SIFT_QUERY"
  --truth "$SIFT_TRUTH"
  --base-limit "$BASE_LIMIT"
  --query-limit "$QUERY_LIMIT"
  --k "$K"
)

MEMORY_FLAG=(--memory-budget-ratio "$MEMORY_BUDGET_RATIO")
if [[ -n "${MEMORY_BUDGET_BYTES:-}" ]]; then
  MEMORY_FLAG+=(--memory-budget-bytes "$MEMORY_BUDGET_BYTES")
fi
if [[ "${ENFORCE_MEMORY_BUDGET:-0}" == "1" ]]; then
  MEMORY_FLAG+=(--enforce-memory-budget)
fi
if [[ "${ALLOW_OVER_BUDGET_FOR_DEBUG:-0}" == "1" ]]; then
  MEMORY_FLAG+=(--allow-over-budget-for-debug)
fi

RESULT="$RESULT_DIR/sift-local-$ENGINE-$DATE_TAG.txt"
LOG="$LOG_DIR/sift-local-$ENGINE-$DATE_TAG.log"

case "$ENGINE" in
  exact)
    {
      echo "system_name=agent-aware"
      echo "dataset=${DATASET_NAME:-sift_subset}"
      echo "run_cache_mode=$RUN_CACHE_MODE"
      echo "warmup_runs=${WARMUP_RUNS:-0}"
      echo "io_mode=$IO_MODE"
      echo "io_mode_effective=pread"
      echo "io_direct_enabled=0"
      echo "io_uring_enabled=0"
      "$BIN" "${COMMON[@]}" "${MEMORY_FLAG[@]}"
    } 2>&1 | tee "$RESULT" | tee "$LOG" >/dev/null
    ;;
  graph)
    BUILD_FLAG=()
    if [[ "${BUILD_INDEX:-1}" == "1" ]]; then
      BUILD_FLAG=(--build-index)
    fi
    SEARCH_EARLY_STOP_FLAG=()
    if [[ "${SEARCH_EARLY_STOP:-1}" == "1" ]]; then
      SEARCH_EARLY_STOP_FLAG=(
        --search-early-stop
        --search-early-stop-min "${SEARCH_EARLY_STOP_MIN:-192}"
      )
    fi
    ADAPTIVE_EARLY_STOP_FLAG=()
    if [[ "${ADAPTIVE_EARLY_STOP:-0}" == "1" ]]; then
      ADAPTIVE_EARLY_STOP_FLAG=(
        --adaptive-early-stop
        --early-stop-patience "${EARLY_STOP_PATIENCE:-16}"
        --early-stop-eps "${EARLY_STOP_EPS:-0.001}"
        --min-expansions "${MIN_EXPANSIONS:-64}"
        --max-expansions "${MAX_EXPANSIONS:-192}"
      )
    fi
    PQ_FLAG=()
    if [[ "${PQ_ENABLE:-0}" == "1" ]]; then
      PQ_FLAG=(
        --pq-enable
        --pq-m "${PQ_M:-8}"
        --pq-ks "${PQ_KS:-256}"
        --pq-train-limit "${PQ_TRAIN_LIMIT:-100000}"
        --pq-train-iterations "${PQ_TRAIN_ITERATIONS:-4}"
      )
    fi
    ADC_FLAG=()
    if [[ "${ADC_ENABLE:-0}" == "1" ]]; then
      ADC_FLAG=(--adc-enable)
    fi
    INDEX="${INDEX:-$RUN_DIR/sift_graph.idx}"
    {
      echo "system_name=agent-aware"
      echo "dataset=${DATASET_NAME:-sift_subset}"
      echo "run_cache_mode=$RUN_CACHE_MODE"
      echo "warmup_runs=${WARMUP_RUNS:-0}"
      "$BIN" "${COMMON[@]}" \
      --layout "${LAYOUT:-packed}" \
      --packing "$PACKING_STRATEGY" \
      --index "$INDEX" \
      "${BUILD_FLAG[@]}" \
      --graph-build-policy "${GRAPH_BUILD_POLICY:-lsh-rp}" \
      --graph-degree "${GRAPH_DEGREE:-32}" \
      --approx-projections "${APPROX_PROJECTIONS:-10}" \
      --approx-window "${APPROX_WINDOW:-32}" \
      --approx-random-samples "${APPROX_RANDOM_SAMPLES:-32}" \
      --approx-candidate-limit "${APPROX_CANDIDATE_LIMIT:-192}" \
      --lsh-tables "${LSH_TABLES:-8}" \
      --lsh-bits "${LSH_BITS:-14}" \
      --lsh-probe-radius "${LSH_PROBE_RADIUS:-0}" \
      --lsh-bucket-limit "${LSH_BUCKET_LIMIT:-64}" \
      --robust-prune-alpha "${ROBUST_PRUNE_ALPHA:-1.2}" \
      --search-width "${SEARCH_WIDTH:-1024}" \
      "${SEARCH_EARLY_STOP_FLAG[@]}" \
      "${ADAPTIVE_EARLY_STOP_FLAG[@]}" \
      --entry-count "${ENTRY_COUNT:-256}" \
      --routing-sample-count "${ROUTING_SAMPLE_COUNT:-4096}" \
      --coaccess-sessions "${COACCESS_SESSIONS:-96}" \
      --coaccess-trace-length "${COACCESS_TRACE_LENGTH:-48}" \
      --cache-policy "${CACHE_POLICY:-none}" \
      --cache-pages "${CACHE_PAGES:-256}" \
      --path-cache-policy "${PATH_CACHE_POLICY:-none}" \
      --path-cache-capacity "${PATH_CACHE_CAPACITY:-1024}" \
      --path-cache-hit-search-width "${PATH_CACHE_HIT_SEARCH_WIDTH:-1024}" \
      --query-signature-policy "${QUERY_SIGNATURE_POLICY:-routed}" \
      --hotpath-train-queries "${HOTPATH_TRAIN_QUERIES:-200}" \
      --hotpath-search-width "${HOTPATH_SEARCH_WIDTH:-128}" \
      --hotpath-entry-count "${HOTPATH_ENTRY_COUNT:-32}" \
      "${PQ_FLAG[@]}" \
      "${ADC_FLAG[@]}" \
      --rerank-topk "${RERANK_TOPK:-0}" \
      --io-mode "$IO_MODE" \
      --io-batch-size "${IO_BATCH_SIZE:-1}" \
      --workload-mode "${WORKLOAD_MODE:-read-only}" \
      --operation-count "${OPERATION_COUNT:-0}" \
      --write-ratio "${WRITE_RATIO:-0}" \
      --delete-ratio "${DELETE_RATIO:-0}" \
      --wal "${WAL:-$RUN_DIR/sift_lsm.wal}" \
      --compaction-policy "${COMPACTION_POLICY:-none}" \
      --stream-merge-index "${STREAM_MERGE_INDEX:-$RUN_DIR/sift_streammerge.idx}" \
      --run-type "$RUN_TYPE_EFFECTIVE" \
      --warmup-runs "${WARMUP_RUNS:-0}" \
      "${MEMORY_FLAG[@]}" \
      2>&1
    } | tee "$RESULT" | tee "$LOG" >/dev/null
    ;;
  *)
    echo "ENGINE must be exact or graph, got: $ENGINE" >&2
    exit 1
    ;;
esac

echo "Result: $RESULT"
echo "Log: $LOG"
