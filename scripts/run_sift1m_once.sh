#!/usr/bin/env bash
set -euo pipefail

# 默认参数来自 tools/benchmarks/sift_search_benchmark.cpp。
# 第一次建图：
#   REBUILD_INDEX=1 bash run_sift1m_once.sh
# 后续复用索引：
#   bash run_sift1m_once.sh
# 临时覆盖搜索参数：
#   EXTRA_ARGS="--search-width 512 --beam-width 32" bash run_sift1m_once.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-"$ROOT/build"}"
BINARY="${BINARY:-"$BUILD_DIR/agent_aware_flow"}"
BUILD="${BUILD:-1}"
JOBS="${JOBS:-$(nproc)}"
TAG="${TAG:-sift1m_once_$(date +%Y%m%d-%H%M%S)}"
OUTPUT_DIR="${OUTPUT_DIR:-"$ROOT/logs/sift_bench/$TAG"}"
RESULT_JSON="$OUTPUT_DIR/result.json"
EXTRA_ARGS="${EXTRA_ARGS:-}"
REBUILD_INDEX="${REBUILD_INDEX:-}"

mkdir -p "$OUTPUT_DIR"

if [[ "$BUILD" == "1" ]]; then
  cmake --build "$BUILD_DIR" --target agent_aware_flow -j"$JOBS"
fi

if [[ ! -x "$BINARY" ]]; then
  echo "missing executable: $BINARY" >&2
  exit 1
fi

cmd=("$BINARY" --output-json "$RESULT_JSON")

if [[ -n "$REBUILD_INDEX" ]]; then
  cmd+=(--rebuild-index "$REBUILD_INDEX")
fi

if [[ -n "$EXTRA_ARGS" ]]; then
  # shellcheck disable=SC2206
  extra=($EXTRA_ARGS)
  cmd+=("${extra[@]}")
fi

{
  printf 'cwd=%q\n' "$ROOT"
  printf 'command='
  printf '%q ' "${cmd[@]}"
  printf '\n'
  printf 'rebuild_index_env=%q\n' "$REBUILD_INDEX"
  printf 'extra_args=%q\n' "$EXTRA_ARGS"
} > "$OUTPUT_DIR/command.txt"

start_epoch="$(date +%s)"
if "${cmd[@]}" > "$OUTPUT_DIR/stdout.json" 2> "$OUTPUT_DIR/stderr.log"; then
  status=0
else
  status=$?
fi
end_epoch="$(date +%s)"
wall_seconds=$((end_epoch - start_epoch))
{
  printf 'start_epoch=%s\n' "$start_epoch"
  printf 'end_epoch=%s\n' "$end_epoch"
  printf 'wall_seconds=%s\n' "$wall_seconds"
  printf 'status=%s\n' "$status"
} > "$OUTPUT_DIR/time.txt"

if [[ "$status" -ne 0 ]]; then
  echo "agent_aware_flow failed with status $status" >&2
  echo "logs: $OUTPUT_DIR" >&2
  exit "$status"
fi

python3 - "$RESULT_JSON" "$wall_seconds" <<'PY'
import json
import sys
from pathlib import Path

result_path = Path(sys.argv[1])
wall_seconds = int(sys.argv[2])
result = json.loads(result_path.read_text())
stats = result.get("stats", {})
queries = max(1, int(result.get("query_count", 0) or 0))

def per_query(name):
    return float(stats.get(name, 0)) / queries

elapsed_ms = float(result.get("elapsed_ms", 0.0))
avg_latency_ms = elapsed_ms / queries

print("SIFT1M summary")
print(f"  result_json: {result_path}")
print(f"  wall_seconds: {wall_seconds}")
print(
    "  dataset: "
    f"base={result.get('base_count')} query={result.get('query_count')} "
    f"dim={result.get('dim')} k={result.get('top_k')}"
)
print(
    "  search: "
    f"width={result.get('effective_search_width')} "
    f"beam={result.get('effective_beam_width')} "
    f"entry={result.get('entry_count')} "
    f"entry_strategy={result.get('entry_strategy')} "
    f"entry_seed_count={result.get('entry_seed_count')}"
)
print(
    "  io/cache: "
    f"io={result.get('io_effective_mode')} "
    f"cache={result.get('cache_policy')} "
    f"prefetch={result.get('prefetch_enabled')}"
)
print(f"  recall@{result.get('top_k')}: {float(result.get('recall_at_k', 0.0)):.4f}")
print(f"  qps: {float(result.get('qps', 0.0)):.4f}")
print(f"  avg_latency_ms: {avg_latency_ms:.3f}")
print(f"  measured_avg_query_latency_ms: {float(result.get('avg_query_latency_ms', 0.0)):.3f}")
print(f"  latency_p95_ms: {float(result.get('latency_p95_ms', 0.0)):.3f}")
print(f"  latency_p99_ms: {float(result.get('latency_p99_ms', 0.0)):.3f}")
print(f"  submitted_reads_per_query: {per_query('submitted_reads'):.3f}")
print(f"  demand_reads_per_query: {per_query('demand_reads'):.3f}")
print(f"  prefetch_reads_per_query: {per_query('prefetch_reads'):.3f}")
print(f"  visited_per_query: {per_query('visited'):.3f}")
print(f"  expanded_per_query: {per_query('expanded'):.3f}")
print(f"  duplicate_skipped_per_query: {per_query('duplicate_skipped'):.3f}")
PY
