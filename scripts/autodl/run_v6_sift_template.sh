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
SEARCH_EARLY_STOP_FLAG=()
if [[ "${SEARCH_EARLY_STOP:-1}" == "1" ]]; then
  SEARCH_EARLY_STOP_FLAG=(
    --search-early-stop
    --search-early-stop-min "${SEARCH_EARLY_STOP_MIN:-192}"
  )
fi

COMMON=(
  --engine graph
  --layout packed
  --packing "${PACKING:-bfs}"
  --base "$SIFT_BASE"
  --query "$SIFT_QUERY"
  --truth "$SIFT_TRUTH"
  --base-limit "$BASE_LIMIT"
  --query-limit "$QUERY_LIMIT"
  --k "${K:-10}"
  --graph-build-policy "${GRAPH_BUILD_POLICY:-lsh-rp}"
  --graph-degree "${GRAPH_DEGREE:-32}"
  --approx-projections "${APPROX_PROJECTIONS:-10}"
  --approx-window "${APPROX_WINDOW:-32}"
  --approx-random-samples "${APPROX_RANDOM_SAMPLES:-32}"
  --approx-candidate-limit "${APPROX_CANDIDATE_LIMIT:-192}"
  --lsh-tables "${LSH_TABLES:-8}"
  --lsh-bits "${LSH_BITS:-14}"
  --lsh-probe-radius "${LSH_PROBE_RADIUS:-0}"
  --lsh-bucket-limit "${LSH_BUCKET_LIMIT:-64}"
  --robust-prune-alpha "${ROBUST_PRUNE_ALPHA:-1.2}"
  --search-width "${SEARCH_WIDTH:-1024}"
  "${SEARCH_EARLY_STOP_FLAG[@]}"
  --entry-count "${ENTRY_COUNT:-256}"
  --routing-sample-count "${ROUTING_SAMPLE_COUNT:-4096}"
  --coaccess-sessions "${COACCESS_SESSIONS:-96}"
  --coaccess-trace-length "${COACCESS_TRACE_LENGTH:-48}"
  --cache-policy agent
  --cache-pages "${CACHE_PAGES:-256}"
  --path-cache-policy reuse
  --path-cache-capacity "${PATH_CACHE_CAPACITY:-1024}"
  --path-cache-hit-search-width "${PATH_CACHE_HIT_SEARCH_WIDTH:-1024}"
  --query-signature-policy "${QUERY_SIGNATURE_POLICY:-routed}"
  --simhash-bits "${SIMHASH_BITS:-16}"
  --pq-prefix-subspaces "${PQ_PREFIX_SUBSPACES:-4}"
  --pq-prefix-centroids "${PQ_PREFIX_CENTROIDS:-16}"
  --pq-prefix-train-iterations "${PQ_PREFIX_TRAIN_ITERATIONS:-4}"
  --workload-mode "${WORKLOAD_MODE:-mixed}"
  --operation-count "${OPERATION_COUNT:-1200}"
  --write-ratio "${WRITE_RATIO:-10}"
  --delete-ratio "${DELETE_RATIO:-0}"
  --delta-index-policy "${DELTA_INDEX_POLICY:-flat}"
  --delta-ivf-centroids "${DELTA_IVF_CENTROIDS:-64}"
  --delta-ivf-probes "${DELTA_IVF_PROBES:-16}"
  --delta-ivf-train-iterations "${DELTA_IVF_TRAIN_ITERATIONS:-8}"
  --delta-ivf-rebuild-interval "${DELTA_IVF_REBUILD_INTERVAL:-64}"
  --delta-compaction-threshold "${DELTA_COMPACTION_THRESHOLD:-64}"
  --compaction-batch-size "${COMPACTION_BATCH_SIZE:-16}"
  --compaction-work-us 0
  --compaction-io-mode file
  --compaction-io-bytes-per-vector "${COMPACTION_IO_BYTES_PER_VECTOR:-65536}"
  --run-type "${RUN_TYPE:-warm}"
  --warmup-runs "${WARMUP_RUNS:-1}"
  --index "$INDEX"
  --stream-merge-index "${STREAM_MERGE_INDEX:-$RUN_DIR/v6_sift_streammerge.idx}"
  "${BUILD_FLAG[@]}"
)

if [[ "${WAL_REPLAY:-0}" == "1" ]]; then
  COMMON+=(--wal-replay)
fi

COMPACTION_POLICY="${COMPACTION_POLICY:-compare}"
COMPACTION_POLICIES_JSON='["aggressive", "sla"]'

case "$COMPACTION_POLICY" in
  compare)
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
    ;;
  stream-merge)
    COMPACTION_POLICIES_JSON='["stream-merge"]'
    {
      echo "variant=stream_merge"
      "$ROOT/build/agentmem_flow" "${COMMON[@]}" \
        --compaction-policy stream-merge \
        --wal "$RUN_DIR/v6_sift_stream_merge.wal" \
        --compaction-io-path "$RUN_DIR/v6_sift_stream_merge_compaction.bin"
    } 2>&1 | tee "$RESULT" | tee "$LOG" >/dev/null
    ;;
  none)
    COMPACTION_POLICIES_JSON='["none"]'
    {
      echo "variant=read_only_or_no_compaction"
      "$ROOT/build/agentmem_flow" "${COMMON[@]}" \
        --compaction-policy none \
        --wal "$RUN_DIR/v6_sift_none.wal" \
        --compaction-io-path "$RUN_DIR/v6_sift_none_compaction.bin"
    } 2>&1 | tee "$RESULT" | tee "$LOG" >/dev/null
    ;;
  *)
    echo "COMPACTION_POLICY must be compare, stream-merge, or none. Got: $COMPACTION_POLICY" >&2
    exit 1
    ;;
esac

cat > "$CONFIG" <<JSON
{
  "version": "v6-v8",
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
    "packing": "${PACKING:-bfs}",
    "graph_build_policy": "${GRAPH_BUILD_POLICY:-lsh-rp}",
    "approx_projections": ${APPROX_PROJECTIONS:-10},
    "approx_window": ${APPROX_WINDOW:-32},
    "approx_random_samples": ${APPROX_RANDOM_SAMPLES:-32},
    "approx_candidate_limit": ${APPROX_CANDIDATE_LIMIT:-192},
    "lsh_tables": ${LSH_TABLES:-8},
    "lsh_bits": ${LSH_BITS:-14},
    "lsh_probe_radius": ${LSH_PROBE_RADIUS:-0},
    "lsh_bucket_limit": ${LSH_BUCKET_LIMIT:-64},
    "robust_prune_alpha": ${ROBUST_PRUNE_ALPHA:-1.2},
    "note": "lsh-rp is the SIFT1M-oriented FreshLSH-Vamana builder; approx-rp remains available for smaller validation runs."
  },
  "search": {
    "search_width": ${SEARCH_WIDTH:-1024},
    "search_early_stop": ${SEARCH_EARLY_STOP:-1},
    "search_early_stop_min": ${SEARCH_EARLY_STOP_MIN:-192},
    "entry_count": ${ENTRY_COUNT:-256},
    "routing_sample_count": ${ROUTING_SAMPLE_COUNT:-4096}
  },
  "query_signature": {
    "policy": "${QUERY_SIGNATURE_POLICY:-routed}",
    "simhash_bits": ${SIMHASH_BITS:-16},
    "pq_prefix_subspaces": ${PQ_PREFIX_SUBSPACES:-4},
    "pq_prefix_centroids": ${PQ_PREFIX_CENTROIDS:-16},
    "pq_prefix_train_iterations": ${PQ_PREFIX_TRAIN_ITERATIONS:-4}
  },
  "delta": {
    "index_policy": "${DELTA_INDEX_POLICY:-flat}",
    "ivf_centroids": ${DELTA_IVF_CENTROIDS:-64},
    "ivf_probes": ${DELTA_IVF_PROBES:-16},
    "ivf_train_iterations": ${DELTA_IVF_TRAIN_ITERATIONS:-8},
    "ivf_rebuild_interval": ${DELTA_IVF_REBUILD_INTERVAL:-64},
    "wal_replay": ${WAL_REPLAY:-0}
  },
  "fresh_lsm": {
    "workload_mode": "${WORKLOAD_MODE:-mixed}",
    "operation_count": ${OPERATION_COUNT:-1200},
    "write_ratio": ${WRITE_RATIO:-10},
    "delete_ratio": ${DELETE_RATIO:-0},
    "stream_merge_index": "${STREAM_MERGE_INDEX:-$RUN_DIR/v6_sift_streammerge.idx}"
  },
  "compaction": {
    "io_mode": "file",
    "io_bytes_per_vector": ${COMPACTION_IO_BYTES_PER_VECTOR:-65536},
    "policy_mode": "$COMPACTION_POLICY",
    "policies": $COMPACTION_POLICIES_JSON
  }
}
JSON

echo "Result: $RESULT"
echo "Log: $LOG"
echo "Config: $CONFIG"
