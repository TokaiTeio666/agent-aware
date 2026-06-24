#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TRACE="build/prefetch_closed_loop_trace.csv"
BOOTSTRAP_MODEL="build/prefetch_bootstrap_xgboost.txt"
MODEL="build/prefetch_closed_loop_xgboost.txt"
METRICS="build/prefetch_closed_loop_metrics.json"
BOOTSTRAP_JSON="build/prefetch_closed_loop_bootstrap.json"
XGBOOST_JSON="build/prefetch_closed_loop_xgboost.json"
INDEX="build/prefetch_closed_loop.idx"

rm -f "$TRACE" "$BOOTSTRAP_MODEL" "$MODEL" "$METRICS" \
  "$BOOTSTRAP_JSON" "$XGBOOST_JSON"

cmake --build build --target agent_aware_flow -j "${JOBS:-2}"

cat >"$BOOTSTRAP_MODEL" <<'MODEL'
# agent-aware xgboost text dump
base_score=0
booster[0]:
0:[num_candidates_on_page<2.5] yes=1,no=2,missing=1
	1:leaf=0.25
	2:leaf=1.0
booster[1]:
0:[contains_topk_candidate<0.5] yes=1,no=2,missing=1
	1:leaf=0.0
	2:leaf=0.75
MODEL

COMMON_ARGS=(
  --synthetic 1
  --base-limit 2000
  --query-limit 4
  --synthetic-dim 16
  --synthetic-clusters 4
  --graph-degree 8
  --search-width 32
  --beam-width 4
  --entry-count 8
  --enable-pq 1
  --pq-subspaces 4
  --pq-centroids 16
  --pq-train-limit 512
  --pq-iterations 2
  --rerank-topk 20
  --io-mode io_uring
  --io-depth 8
  --io-batch 4
  --cache-pages 4
)

./build/agent_aware_flow \
  "${COMMON_ARGS[@]}" \
  --rebuild-index 1 \
  --index-path "$INDEX" \
  --output-json "$BOOTSTRAP_JSON" \
  --enable-prefetch 1 \
  --prefetch-policy xgboost \
  --prefetch-model "$BOOTSTRAP_MODEL" \
  --prefetch-width 2 \
  --prefetch-top-k 2 \
  --prefetch-max-inflight 2 \
  --prefetch-score-threshold -1000000000 \
  --prefetch-trace "$TRACE" >/tmp/prefetch_closed_loop_bootstrap.out

python3 scripts/train_prefetch_ranker.py \
  --trace "$TRACE" \
  --model-out "$MODEL" \
  --metrics-out "$METRICS" \
  --top-k 2 \
  --max-inflight 2 \
  --rounds 8 \
  --backend auto >/tmp/prefetch_closed_loop_train.out

./build/agent_aware_flow \
  "${COMMON_ARGS[@]}" \
  --rebuild-index 0 \
  --index-path "$INDEX" \
  --output-json "$XGBOOST_JSON" \
  --enable-prefetch 1 \
  --prefetch-policy xgboost \
  --prefetch-model "$MODEL" \
  --prefetch-width 2 \
  --prefetch-top-k 2 \
  --prefetch-max-inflight 2 \
  --prefetch-score-threshold -1000000000 >/tmp/prefetch_closed_loop_xgboost.out

python3 - <<'PY'
import csv
import json
from pathlib import Path

trace = Path("build/prefetch_closed_loop_trace.csv")
metrics = json.loads(Path("build/prefetch_closed_loop_metrics.json").read_text())
bootstrap = json.loads(Path("build/prefetch_closed_loop_bootstrap.json").read_text())
xgboost = json.loads(Path("build/prefetch_closed_loop_xgboost.json").read_text())

rows = list(csv.DictReader(trace.open()))
if not rows:
    raise SystemExit("prefetch trace is empty")
required = {
    "query_id",
    "group_id",
    "candidate_page_id",
    "prefetch_score",
    "selected_by_policy",
    "was_prefetched",
    "label",
}
missing = required - set(rows[0])
if missing:
    raise SystemExit(f"prefetch trace missing columns: {sorted(missing)}")
if bootstrap["status"] != "completed" or xgboost["status"] != "completed":
    raise SystemExit("online xgboost smoke did not complete")
if bootstrap["pq_adc_enabled"] is not True or xgboost["pq_adc_enabled"] is not True:
    raise SystemExit("smoke must run through PQ+ADC")
if xgboost["prefetch_ranker"] != "xgboost":
    raise SystemExit("online run did not use xgboost ranker")
if metrics["splits"]["train"]["xgboost"]["groups"] <= 0:
    raise SystemExit("replay metrics did not include train groups")

print(
    "closed_loop_ok",
    f"trace_rows={len(rows)}",
    f"bootstrap_prefetch={bootstrap['stats']['prefetch_submitted']}",
    f"xgboost_prefetch={xgboost['stats']['prefetch_submitted']}",
    f"trainer_backend={metrics['backend']}",
)
PY
