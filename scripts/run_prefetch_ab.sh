#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

INDEX="${1:-indexes/sift.idx}"
MODEL="${2:-build/xgboost_model.txt}"
QUERY_OFFSET="${3:-0}"
QUERY_LIMIT="${4:-1000}"

SUITE_ID="${PREFETCH_SUITE_ID:-prefetch_ab_$(date +%Y%m%d-%H%M%S)}"
OUT_ROOT="${PREFETCH_AB_OUT_ROOT:-build/prefetch_ab_runs/$SUITE_ID}"
SEEDS_TEXT="${PREFETCH_AB_SEEDS:-42,43,44,45,46}"
CACHE_CONDITIONS="${PREFETCH_AB_CACHE_CONDITIONS:-cold,hot}"

TOP_K="${TOP_K:-10}"
SEARCH_WIDTH="${SEARCH_WIDTH:-350}"
BEAM_WIDTH="${BEAM_WIDTH:-16}"
ENTRY_COUNT="${ENTRY_COUNT:-64}"
RERANK_TOPK="${RERANK_TOPK:-100}"

PQ_SUBSPACES="${PQ_SUBSPACES:-32}"
PQ_CENTROIDS="${PQ_CENTROIDS:-256}"
PQ_TRAIN_LIMIT="${PQ_TRAIN_LIMIT:-100000}"
PQ_ITERATIONS="${PQ_ITERATIONS:-8}"

IO_MODE="${IO_MODE:-io_uring}"
IO_DEPTH="${IO_DEPTH:-64}"
IO_BATCH="${IO_BATCH:-16}"

CACHE_PAGES="${CACHE_PAGES:-15000}"
MEMORY_BUDGET="${MEMORY_BUDGET:-0.20}"

PREFETCH_WIDTH="${PREFETCH_WIDTH:-16}"
PREFETCH_TOPK="${PREFETCH_TOPK:-16}"
PREFETCH_MAX_INFLIGHT="${PREFETCH_MAX_INFLIGHT:-8}"
PREFETCH_EARLY_TRIGGER="${PREFETCH_EARLY_TRIGGER:-rerank}"
REPLAY_TOP_K="${REPLAY_TOP_K:-$PREFETCH_TOPK}"

if [[ "${PREFETCH_AB_SMOKE:-0}" == "1" ]]; then
  SEEDS_TEXT="${PREFETCH_AB_SEEDS:-42,43}"
  QUERY_LIMIT="${PREFETCH_AB_QUERY_LIMIT:-20}"
fi

if [[ -z "${HOT_WARMUP_QUERY_LIMIT+x}" ]]; then
  if [[ "${PREFETCH_AB_SMOKE:-0}" == "1" ]]; then
    HOT_WARMUP_QUERY_LIMIT=5
  else
    HOT_WARMUP_QUERY_LIMIT="$QUERY_LIMIT"
  fi
fi

IFS=',' read -r -a SEEDS <<< "$SEEDS_TEXT"
read -r -a EXTRA_ARGS <<< "${PREFETCH_AB_EXTRA_ARGS:-}"
mkdir -p "$OUT_ROOT"

if [[ ! -f "$MODEL" ]]; then
  echo "missing prefetch model: $MODEL" >&2
  exit 1
fi

MODEL_ABS="$(realpath "$MODEL")"
POLICY_VERSION="${PREFETCH_POLICY_VERSION:-xgboost:$(sha256sum "$MODEL_ABS" | awk '{print substr($1,1,12)}')}"

cmake --build build --target agent-aware -j "${JOBS:-$(nproc)}"

COMMON_ARGS=(
  --index-path "$INDEX"
  --top-k "$TOP_K"
  --search-width "$SEARCH_WIDTH"
  --beam-width "$BEAM_WIDTH"
  --entry-count "$ENTRY_COUNT"
  --rerank-topk "$RERANK_TOPK"
  --enable-pq 1
  --pq-subspaces "$PQ_SUBSPACES"
  --pq-centroids "$PQ_CENTROIDS"
  --pq-train-limit "$PQ_TRAIN_LIMIT"
  --pq-iterations "$PQ_ITERATIONS"
  --io-mode "$IO_MODE"
  --io-depth "$IO_DEPTH"
  --io-batch "$IO_BATCH"
  --cache-pages "$CACHE_PAGES"
  --memory-budget-ratio "$MEMORY_BUDGET"
  --query-offset "$QUERY_OFFSET"
  --query-limit "$QUERY_LIMIT"
  --rebuild-index 0
)

reset_cold_cache() {
  if [[ -z "${PREFETCH_COLD_RESET_CMD:-}" ]]; then
    echo "PREFETCH_COLD_RESET_CMD is required for cold cache runs" >&2
    exit 1
  fi
  bash -lc "$PREFETCH_COLD_RESET_CMD"
}

run_arm() {
  local cache_condition="$1"
  local round_dir="$2"
  local round_index="$3"
  local arm="$4"
  local seed="$5"
  local run_id="${SUITE_ID}:${cache_condition}:round-${round_index}:${arm}:seed-${seed}"
  local json_path="${round_dir}/${arm}.json"
  local log_path="${round_dir}/${arm}.log"
  local trace_path="${round_dir}/${arm}.trace.csv"

  if [[ "$cache_condition" == "cold" ]]; then
    reset_cold_cache
  fi

  local warmup_args=()
  if [[ "$cache_condition" == "hot" ]]; then
    warmup_args=(--warmup-query-limit "$HOT_WARMUP_QUERY_LIMIT")
  fi

  if [[ "$arm" == "baseline" ]]; then
    if ! ./build/agent-aware \
      "${COMMON_ARGS[@]}" \
      "${EXTRA_ARGS[@]}" \
      "${warmup_args[@]}" \
      --seed "$seed" \
      --run-id "$run_id" \
      --policy-version "baseline:none" \
      --cache-condition "$cache_condition" \
      --output-json "$json_path" \
      --enable-prefetch 0 \
      --prefetch-policy none \
      > "$log_path" 2>&1; then
      echo "baseline failed; tailing $log_path" >&2
      tail -n 120 "$log_path" >&2
      return 1
    fi
  else
    if ! ./build/agent-aware \
      "${COMMON_ARGS[@]}" \
      "${EXTRA_ARGS[@]}" \
      "${warmup_args[@]}" \
      --seed "$seed" \
      --run-id "$run_id" \
      --policy-version "$POLICY_VERSION" \
      --cache-condition "$cache_condition" \
      --output-json "$json_path" \
      --enable-prefetch 1 \
      --prefetch-policy xgboost \
      --prefetch-model "$MODEL_ABS" \
      --prefetch-width "$PREFETCH_WIDTH" \
      --prefetch-top-k "$PREFETCH_TOPK" \
      --prefetch-max-inflight "$PREFETCH_MAX_INFLIGHT" \
      --prefetch-early-trigger "$PREFETCH_EARLY_TRIGGER" \
      --prefetch-trace "$trace_path" \
      > "$log_path" 2>&1; then
      echo "candidate failed; tailing $log_path" >&2
      tail -n 120 "$log_path" >&2
      return 1
    fi

    python3 scripts/analyze_prefetch_trace.py \
      --trace "$trace_path" \
      --json-out "${round_dir}/candidate.trace.summary.json" \
      --md-out "${round_dir}/candidate.trace.summary.md" \
      --group-by trigger > "${round_dir}/candidate.trace.summary.stdout.json"
    python3 scripts/validate_prefetch_trace.py \
      --trace "$trace_path" \
      --result-json "$json_path" \
      --json-out "${round_dir}/candidate.trace.validation.json" \
      > "${round_dir}/candidate.trace.validation.stdout.json"
    python3 scripts/replay_prefetch_policy.py \
      --trace "$trace_path" \
      --model "$MODEL_ABS" \
      --top-k "$REPLAY_TOP_K" \
      --match-online-budget \
      --out-json "${round_dir}/candidate.replay.json" \
      --out-md "${round_dir}/candidate.replay.md" \
      > "${round_dir}/candidate.replay.stdout.json"
  fi
}

round_index=0
for cache_condition in ${CACHE_CONDITIONS//,/ }; do
  if [[ "$cache_condition" != "cold" && "$cache_condition" != "hot" ]]; then
    echo "unsupported cache condition: $cache_condition" >&2
    exit 1
  fi
  cache_dir="$OUT_ROOT/$cache_condition"
  mkdir -p "$cache_dir"
  round_index=0
  for seed in "${SEEDS[@]}"; do
    round_index=$((round_index + 1))
    round_dir="$cache_dir/round-$(printf '%02d' "$round_index")"
    rm -rf "$round_dir"
    mkdir -p "$round_dir"
    if (( round_index % 2 == 1 )); then
      order=(baseline candidate)
    else
      order=(candidate baseline)
    fi
    printf '%s\n' "${order[*]}" > "$round_dir/order.txt"
    echo "=== ${cache_condition} round ${round_index} seed ${seed} order ${order[*]} ==="
    for arm in "${order[@]}"; do
      run_arm "$cache_condition" "$round_dir" "$round_index" "$arm" "$seed"
    done
  done
done

python3 scripts/summarize_prefetch_ab.py \
  --root "$OUT_ROOT" \
  --json-out "$OUT_ROOT/report.json" \
  --md-out "$OUT_ROOT/report.md" \
  > "$OUT_ROOT/report.stdout.json"

echo "A/B report: $OUT_ROOT/report.md"
