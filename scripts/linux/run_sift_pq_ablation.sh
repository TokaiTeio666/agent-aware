#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_LOCAL="$ROOT/scripts/linux/run_sift_local.sh"
INDEX_DIR="${INDEX_DIR:-$ROOT/build/sift_pq_indexes}"
mkdir -p "$INDEX_DIR"

QUERY_LIMIT="${QUERY_LIMIT:-1000}"
BASE_LIMIT="${BASE_LIMIT:-0}"
SEARCH_WIDTH="${SEARCH_WIDTH:-1024}"
RERANK_TOPKS=(${RERANK_TOPKS:-0 50 100 200})
INDEX_PATH="${INDEX_PATH:-$INDEX_DIR/sift-pq.idx}"

build_index="${BUILD_INDEX:-1}"

tag="pq-off-$(date +%Y%m%d-%H%M%S)"
ENGINE="${ENGINE:-graph}" \
DATASET_NAME="${DATASET_NAME:-sift1m}" \
BASE_LIMIT="$BASE_LIMIT" \
QUERY_LIMIT="$QUERY_LIMIT" \
INDEX_PATH="$INDEX_PATH" \
BUILD_INDEX="$build_index" \
SEARCH_WIDTH="$SEARCH_WIDTH" \
PQ_ENABLE=0 \
ADC_ENABLE=0 \
RERANK_TOPK=0 \
RUN_TYPE="${RUN_TYPE:-dev}" \
DATE_TAG="$tag" \
  bash "$RUN_LOCAL"
build_index=0

for rerank in "${RERANK_TOPKS[@]}"; do
  tag="pq-adc-r${rerank}-$(date +%Y%m%d-%H%M%S)"
  ENGINE="${ENGINE:-graph}" \
  DATASET_NAME="${DATASET_NAME:-sift1m}" \
  BASE_LIMIT="$BASE_LIMIT" \
  QUERY_LIMIT="$QUERY_LIMIT" \
  INDEX_PATH="$INDEX_PATH" \
  BUILD_INDEX=0 \
  REUSE_INDEX=1 \
  SEARCH_WIDTH="$SEARCH_WIDTH" \
  PQ_ENABLE=1 \
  ADC_ENABLE=1 \
  PQ_M="${PQ_M:-8}" \
  PQ_KS="${PQ_KS:-256}" \
  PQ_TRAIN_LIMIT="${PQ_TRAIN_LIMIT:-100000}" \
  RERANK_TOPK="$rerank" \
  RUN_TYPE="${RUN_TYPE:-dev}" \
  DATE_TAG="$tag" \
    bash "$RUN_LOCAL"
done

python3 "$ROOT/scripts/linux/summarize_sift_results.py"
