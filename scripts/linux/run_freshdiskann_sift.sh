#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RESULT_DIR="$ROOT/archive/results"
LOG_DIR="$ROOT/archive/logs"
DATE_TAG="${DATE_TAG:-$(date +%Y%m%d-%H%M%S)}"
RESULT="$RESULT_DIR/system-freshdiskann-$DATE_TAG.txt"
LOG="$LOG_DIR/system-freshdiskann-$DATE_TAG.log"

mkdir -p "$RESULT_DIR" "$LOG_DIR"

{
  echo "system_name=FreshDiskANN"
  echo "dataset=${DATASET_NAME:-sift1m}"
  echo "base_count=${BASE_LIMIT:-1000000}"
  echo "query_count=${QUERY_LIMIT:-10000}"
  echo "k=${K:-10}"
  echo "run_cache_mode=${RUN_CACHE_MODE:-cold}"
  if [[ -z "${FRESHDISKANN_RUN_COMMAND:-}" ]]; then
    echo "status=missing_run_command"
    echo "notes=Set_FRESHDISKANN_RUN_COMMAND_to_the_same-machine_FreshDiskANN_runner"
    exit 2
  fi
  echo "status=running"
  /usr/bin/time -v bash -lc "$FRESHDISKANN_RUN_COMMAND"
  echo "status=completed"
} 2>&1 | tee "$RESULT" | tee "$LOG"

python3 "$ROOT/scripts/linux/summarize_system_comparison.py"

