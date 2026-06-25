#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# ============================================================
# Config — 按需修改
# ============================================================
INDEX="${1:-indexes/sift.idx}"
MODEL="${2:-build/prefetch_closed_loop_xgboost.txt}"
BASELINE_JSON="build/prefetch_ab_baseline.json"
XGBOOST_JSON="build/prefetch_ab_xgboost.json"
TRACE="build/prefetch_ab_trace.csv"

# ---------- 搜索参数 ----------
TOP_K=10
SEARCH_WIDTH=350
BEAM_WIDTH=16
ENTRY_COUNT=64
RERANK_TOPK=100

# ---------- PQ ----------
PQ_SUBSPACES=32
PQ_CENTROIDS=256
PQ_TRAIN_LIMIT=100000
PQ_ITERATIONS=8

# ---------- I/O ----------
IO_MODE=io_uring
IO_DEPTH=32
IO_BATCH=16

# ---------- Cache ----------
CACHE_PAGES=0          # 0 = auto from memory budget
MEMORY_BUDGET=0.20

# ---------- Prefetch ----------
PREFETCH_WIDTH=4
PREFETCH_TOPK=4
PREFETCH_MAX_INFLIGHT=0
PREFETCH_EARLY_TRIGGER=pre-beam

# ============================================================
rm -f "$BASELINE_JSON" "$XGBOOST_JSON"

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
)

echo "=== [1/2] BASELINE (zero prefetch) ==="
./build/agent-aware \
  "${COMMON_ARGS[@]}" \
  --rebuild-index 0 \
  --output-json "$BASELINE_JSON" \
  --enable-prefetch 0 \
  --prefetch-policy none \
  2>&1 | tee /tmp/prefetch_ab_baseline.out

echo ""
echo "=== [2/2] XGBOOST ==="
./build/agent-aware \
  "${COMMON_ARGS[@]}" \
  --rebuild-index 0 \
  --output-json "$XGBOOST_JSON" \
  --enable-prefetch 1 \
  --prefetch-policy xgboost \
  --prefetch-model "$MODEL" \
  --prefetch-width "$PREFETCH_WIDTH" \
  --prefetch-top-k "$PREFETCH_TOPK" \
  --prefetch-max-inflight "$PREFETCH_MAX_INFLIGHT" \
  --prefetch-early-trigger "$PREFETCH_EARLY_TRIGGER" \
  --prefetch-trace "$TRACE" \
  2>&1 | tee /tmp/prefetch_ab_xgboost.out

# ============================================================
echo ""
echo "=== COMPARISON ==="
python3 - <<'PY'
import json

baseline = json.loads(open("build/prefetch_ab_baseline.json"))
xgboost  = json.loads(open("build/prefetch_ab_xgboost.json"))

bs = baseline["stats"]
xg = xgboost["stats"]

def fmt(v):
    return f"{v:,.1f}" if isinstance(v, float) else f"{v:,}"

def row(label, key, unit=""):
    b = bs.get(key, 0)
    x = xg.get(key, 0)
    delta = ""
    if isinstance(b, (int, float)) and isinstance(x, (int, float)) and b > 0:
        pct = (x - b) / b * 100
        delta = f" ({pct:+.1f}%)"
    print(f"  {label:<35} {fmt(b):>12}{unit}  →  {fmt(x):>12}{unit}{delta}")

print(f"\n{'Metric':<35} {'baseline':>14} {'xgboost':>14}")
print("-" * 70)
row("avg_latency_ms",     "avg_latency_ms",       "ms")
row("p50_latency_ms",     "p50_latency_ms",       "ms")
row("p99_latency_ms",     "p99_latency_ms",       "ms")
row("qps",                "qps")
row("recall@10",           "recall_at_10")
row("page_reads",          "page_reads")
row("demand_reads",        "demand_reads")
row("prefetch_submitted",  "prefetch_submitted")
row("prefetch_ready_hit",  "prefetch_ready_hit")
row("prefetch_useful",     "prefetch_useful_pages")
row("prefetch_wasted",     "prefetch_wasted_pages")
row("cache_hit_rate",      "cache_hit_rate")
print()

# 关键结论
lat_b = baseline.get("avg_latency_ms", 0)
lat_x = xgboost.get("avg_latency_ms", 0)
recall_b = baseline.get("recall_at_10", 0)
recall_x = xgboost.get("recall_at_10", 0)

print("Summary:")
if lat_b > 0:
    lat_delta = (lat_x - lat_b) / lat_b * 100
    print(f"  Latency: {lat_delta:+.1f}%  (negative = xgboost faster)")
if recall_b > 0:
    r_delta = (recall_x - recall_b) * 100
    print(f"  Recall:  {r_delta:+.3f} percentage points")
PY
