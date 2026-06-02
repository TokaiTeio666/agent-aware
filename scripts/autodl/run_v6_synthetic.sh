#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATE_TAG="${DATE_TAG:-$(date +%F)}"
RUN_DIR="$ROOT/build/v6_autodl_$DATE_TAG"
mkdir -p "$RUN_DIR" "$ROOT/archive/results" "$ROOT/archive/logs" \
  "$ROOT/archive/configs" "$ROOT/archive/build_info"

bash "$ROOT/scripts/linux/build.sh"
bash "$ROOT/scripts/autodl/collect_env.sh" \
  "$ROOT/archive/build_info/v6-autodl-$DATE_TAG.txt"

COMMON=(
  --engine graph
  --layout packed
  --packing coaccess
  --synthetic
  --synthetic-workload agent
  --session-length "${SESSION_LENGTH:-10}"
  --base-count "${BASE_COUNT:-2000}"
  --query-count "${QUERY_COUNT:-300}"
  --dim "${DIM:-64}"
  --clusters "${CLUSTERS:-32}"
  --seed "${SEED:-42}"
  --k "${K:-10}"
  --graph-degree "${GRAPH_DEGREE:-16}"
  --search-width "${SEARCH_WIDTH:-128}"
  --entry-count "${ENTRY_COUNT:-48}"
  --routing-sample-count "${ROUTING_SAMPLE_COUNT:-512}"
  --coaccess-sessions "${COACCESS_SESSIONS:-96}"
  --coaccess-trace-length "${COACCESS_TRACE_LENGTH:-48}"
  --cache-policy agent
  --cache-pages "${CACHE_PAGES:-24}"
  --path-cache-policy reuse
  --path-cache-capacity "${PATH_CACHE_CAPACITY:-128}"
  --path-cache-hit-search-width "${PATH_CACHE_HIT_SEARCH_WIDTH:-96}"
  --query-signature-policy "${QUERY_SIGNATURE_POLICY:-routed}"
  --simhash-bits "${SIMHASH_BITS:-16}"
  --pq-prefix-subspaces "${PQ_PREFIX_SUBSPACES:-4}"
  --pq-prefix-centroids "${PQ_PREFIX_CENTROIDS:-16}"
  --pq-prefix-train-iterations "${PQ_PREFIX_TRAIN_ITERATIONS:-4}"
  --workload-mode mixed
  --operation-count "${OPERATION_COUNT:-360}"
  --write-ratio "${WRITE_RATIO:-20}"
  --delta-index-policy "${DELTA_INDEX_POLICY:-flat}"
  --delta-ivf-centroids "${DELTA_IVF_CENTROIDS:-32}"
  --delta-ivf-probes "${DELTA_IVF_PROBES:-16}"
  --delta-ivf-train-iterations "${DELTA_IVF_TRAIN_ITERATIONS:-6}"
  --delta-ivf-rebuild-interval "${DELTA_IVF_REBUILD_INTERVAL:-32}"
  --delta-compaction-threshold "${DELTA_COMPACTION_THRESHOLD:-24}"
  --compaction-batch-size "${COMPACTION_BATCH_SIZE:-8}"
  --compaction-work-us 0
  --compaction-io-mode file
  --compaction-io-bytes-per-vector "${COMPACTION_IO_BYTES_PER_VECTOR:-65536}"
  --run-type "${RUN_TYPE:-warm}"
  --warmup-runs "${WARMUP_RUNS:-1}"
)

if [[ "${WAL_REPLAY:-0}" == "1" ]]; then
  COMMON+=(--wal-replay)
fi

INDEX="$RUN_DIR/v6_packed_coaccess.idx"
RESULT="$ROOT/archive/results/v6-autodl-synthetic-$DATE_TAG.txt"
LOG="$ROOT/archive/logs/v6-autodl-synthetic-$DATE_TAG.log"
CONFIG="$ROOT/archive/configs/v6-autodl-synthetic-$DATE_TAG.json"

{
  echo "variant=no_compaction"
  "$ROOT/build/agentmem_flow" "${COMMON[@]}" \
    --compaction-policy none \
    --wal "$RUN_DIR/v6_none.wal" \
    --compaction-io-path "$RUN_DIR/v6_none_compaction.bin" \
    --index "$INDEX" \
    --build-index

  echo "variant=aggressive_file_io"
  "$ROOT/build/agentmem_flow" "${COMMON[@]}" \
    --compaction-policy aggressive \
    --wal "$RUN_DIR/v6_aggressive.wal" \
    --compaction-io-path "$RUN_DIR/v6_aggressive_compaction.bin" \
    --index "$INDEX"

  echo "variant=sla_file_io"
  "$ROOT/build/agentmem_flow" "${COMMON[@]}" \
    --compaction-policy sla \
    --sla-p99-ms "${SLA_P99_MS:-0.8}" \
    --wal "$RUN_DIR/v6_sla.wal" \
    --compaction-io-path "$RUN_DIR/v6_sla_compaction.bin" \
    --index "$INDEX"
} 2>&1 | tee "$RESULT" | tee "$LOG" >/dev/null

cat > "$CONFIG" <<JSON
{
  "version": "v6",
  "date": "$DATE_TAG",
  "environment": "AutoDL/Linux target",
  "script": "scripts/autodl/run_v6_synthetic.sh",
  "dataset": {
    "type": "synthetic",
    "base_count": ${BASE_COUNT:-2000},
    "query_count": ${QUERY_COUNT:-300},
    "dim": ${DIM:-64},
    "clusters": ${CLUSTERS:-32},
    "seed": ${SEED:-42}
  },
  "workload": {
    "operation_count": ${OPERATION_COUNT:-360},
    "write_ratio_percent": ${WRITE_RATIO:-20},
    "delta_index_policy": "${DELTA_INDEX_POLICY:-flat}",
    "delta_ivf_centroids": ${DELTA_IVF_CENTROIDS:-32},
    "delta_ivf_probes": ${DELTA_IVF_PROBES:-16},
    "delta_ivf_train_iterations": ${DELTA_IVF_TRAIN_ITERATIONS:-6},
    "delta_ivf_rebuild_interval": ${DELTA_IVF_REBUILD_INTERVAL:-32},
    "wal_replay": ${WAL_REPLAY:-0},
    "run_type": "${RUN_TYPE:-warm}",
    "warmup_runs": ${WARMUP_RUNS:-1},
    "query_signature_policy": "${QUERY_SIGNATURE_POLICY:-routed}",
    "simhash_bits": ${SIMHASH_BITS:-16},
    "pq_prefix_subspaces": ${PQ_PREFIX_SUBSPACES:-4},
    "pq_prefix_centroids": ${PQ_PREFIX_CENTROIDS:-16},
    "pq_prefix_train_iterations": ${PQ_PREFIX_TRAIN_ITERATIONS:-4}
  },
  "compaction": {
    "io_mode": "file",
    "io_bytes_per_vector": ${COMPACTION_IO_BYTES_PER_VECTOR:-65536},
    "policies": ["none", "aggressive", "sla"],
    "sla_p99_ms": ${SLA_P99_MS:-0.8}
  },
  "pass_criteria": {
    "recall_at_10_min": 0.95,
    "wal_records_equal_insert_count": true,
    "sla_p99_lower_than_aggressive": true,
    "compaction_io_bytes_observable": true
  }
}
JSON

echo "Result: $RESULT"
echo "Log: $LOG"
echo "Config: $CONFIG"
