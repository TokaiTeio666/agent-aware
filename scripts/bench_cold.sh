#!/usr/bin/env bash
# 冷缓存基准：在真实块设备上跑一次搜索，并输出可判断实验有效性的关键字段。
#
# Phase 0 的意义不是"再跑一个数字"，而是回答一个问题：
#   这次实验的延迟，到底是磁盘 I/O 造成的，还是页缓存里的内存拷贝？
#
# - logical_to_physical_read_ratio ≈ 1  → 每次逻辑读都落到了设备
# - logical_to_physical_read_ratio << 1 → 大部分读被页缓存吸收，延迟是内存延迟
# - index_fully_cached_during_run=true  → 本实验不能用来论证磁盘 I/O 优化
#
# 用法（在 VM 内）：
#   scripts/bench_cold.sh <index> <dataset-dir> [额外参数...]
set -euo pipefail

INDEX="${1:?usage: bench_cold.sh <index> <dataset-dir> [extra args...]}"
DATASET="${2:?usage: bench_cold.sh <index> <dataset-dir> [extra args...]}"
shift 2

BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/agent-aware-search"
OUT="${OUT:-/data/work/cold_run.json}"

echo "== 校验：索引必须在真实块设备上 =="
DEV_FS=$(df -T "$INDEX" | awk 'NR==2 {print $2}')
if [[ "$DEV_FS" == "tmpfs" || "$DEV_FS" == "ramfs" ]]; then
  echo "error: index is on $DEV_FS — no physical I/O exists there" >&2
  exit 1
fi
echo "filesystem: $DEV_FS"

echo "== 逐出页缓存 =="
sync
# 两种方式：有 root 时全局 drop_caches；否则用 vmtouch 只逐出目标索引。
# 逐出后必须复核驻留为 0，否则后面的延迟数字仍然可能来自页缓存。
if [[ -w /proc/sys/vm/drop_caches ]] && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; then
  echo "evicted via drop_caches"
elif command -v vmtouch >/dev/null 2>&1; then
  vmtouch -q -e "$INDEX"
  echo "evicted via vmtouch"
else
  echo "error: need root (drop_caches) or vmtouch to evict the page cache" >&2
  exit 1
fi
RESIDENT=$(vmtouch -v "$INDEX" 2>/dev/null | grep -oE '[0-9]+/[0-9]+' | tail -1)
echo "pages resident before run: ${RESIDENT:-unknown}"
if [[ "$RESIDENT" == "0/"* ]]; then
  echo "ok: index is fully out of the page cache"
else
  echo "warning: index is not fully evicted; the run below is not a cold run" >&2
fi

echo "== 运行 =="
"$BIN" --sift-dir "$DATASET" --index-path "$INDEX" \
  --io-mode io_uring --cache-policy none \
  --memory-budget-enforce 0 \
  --output-json "$OUT" "$@" >/dev/null

python3 - "$OUT" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
rows = [
    ("recall_at_k", "recall_at_k"),
    ("qps", "qps"),
    ("avg_ms", "avg_query_latency_ms"),
    ("p99_ms", "latency_p99_ms"),
    ("reads/query", "reads_per_query"),
    ("logical bytes/query", "bytes_read_per_query"),
    ("physical bytes/query", "physical_read_bytes_per_query"),
    ("logical/physical", "logical_to_physical_read_ratio"),
    ("cache resident before", "index_cache_residency_before"),
    ("cache resident after", "index_cache_residency_after"),
    ("fully cached during run", "index_fully_cached_during_run"),
]
for label, key in rows:
    print(f"{label:26} {d.get(key)}")

ratio = d.get("logical_to_physical_read_ratio")
cached = d.get("index_fully_cached_during_run")
print()
if cached is True:
    print("结论：索引全程驻留页缓存，本实验测的是内存读，不能论证磁盘 I/O 优化。")
elif ratio is not None and ratio < 0.1:
    print(f"结论：仅 {ratio:.1%} 的逻辑读落到设备，延迟主要由页缓存决定。")
elif ratio is not None:
    print(f"结论：{ratio:.1%} 的逻辑读落到设备，本实验具有 I/O 意义。")
else:
    print("结论：物理 I/O 不可测（平台不支持），不能给出 I/O 结论。")
PY
