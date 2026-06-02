#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_LOCAL="$ROOT/scripts/linux/run_sift_local.sh"
INDEX_DIR="${INDEX_DIR:-$ROOT/build/sift_sweep_indexes}"
QUERY_LIMIT="${QUERY_LIMIT:-1000}"
mkdir -p "$INDEX_DIR"

BASE_LIMITS=(${BASE_LIMITS:-10000 100000})
SEARCH_WIDTHS=(${SEARCH_WIDTHS:-128 256 512 768 1024})
EARLY_STOP_MINS=(${EARLY_STOP_MINS:-64 96 128 160 192})

for base_limit in "${BASE_LIMITS[@]}"; do
  exact_tag="sweep-${base_limit}-exact-$(date +%Y%m%d-%H%M%S)"
  BASE_LIMIT="$base_limit" QUERY_LIMIT="$QUERY_LIMIT" ENGINE=exact \
    DATE_TAG="$exact_tag" bash "$RUN_LOCAL"

  index="$INDEX_DIR/sift-${base_limit}-${GRAPH_BUILD_POLICY:-lsh-rp}.idx"
  build_index=1
  for width in "${SEARCH_WIDTHS[@]}"; do
    for min_expansions in "${EARLY_STOP_MINS[@]}"; do
      graph_tag="sweep-${base_limit}-graph-w${width}-m${min_expansions}-$(date +%Y%m%d-%H%M%S)"
      BASE_LIMIT="$base_limit" QUERY_LIMIT="$QUERY_LIMIT" ENGINE=graph \
        INDEX="$index" BUILD_INDEX="$build_index" \
        SEARCH_WIDTH="$width" SEARCH_EARLY_STOP=1 \
        SEARCH_EARLY_STOP_MIN="$min_expansions" \
        CACHE_POLICY=none PATH_CACHE_POLICY=none \
        DATE_TAG="$graph_tag" bash "$RUN_LOCAL"
      build_index=0
    done
  done
done

python3 "$ROOT/scripts/linux/summarize_sift_results.py"
