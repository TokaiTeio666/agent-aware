#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/build/agentmem_flow"

if [[ ! -x "$BIN" ]]; then
  "$ROOT/scripts/linux/build.sh"
fi

drop_caches() {
  sync
  if [[ "$(id -u)" -eq 0 ]]; then
    sh -c 'echo 3 > /proc/sys/vm/drop_caches'
  else
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
  fi
}

COMMON=(
  --engine graph
  --synthetic
  --base-count 2000
  --query-count 200
  --dim 64
  --clusters 32
  --k 10
  --graph-degree 16
  --search-width 128
  --entry-count 48
  --routing-sample-count 512
)

echo "== V2 strict cold/warm experiment =="
echo "Note: strict cold requires permission to drop Linux page cache."
echo "Recommendation: run this repo and index files from WSL ext4, not /mnt/c."

echo
echo "== Build one-node index =="
"$BIN" "${COMMON[@]}" \
  --layout one-node \
  --run-type smoke \
  --index "$ROOT/build/v2_linux_onenode.idx" \
  --build-index \
  >/tmp/agentmem_v2_build_onenode.log

echo "== Build coaccess packed index =="
"$BIN" "${COMMON[@]}" \
  --layout packed \
  --packing coaccess \
  --run-type smoke \
  --index "$ROOT/build/v2_linux_packed_coaccess.idx" \
  --build-index \
  --coaccess-sessions 96 \
  --coaccess-trace-length 48 \
  >/tmp/agentmem_v2_build_coaccess.log

echo
echo "== Strict cold: one-node =="
drop_caches
"$BIN" "${COMMON[@]}" \
  --layout one-node \
  --run-type cold \
  --index "$ROOT/build/v2_linux_onenode.idx"

echo
echo "== Strict cold: packed coaccess =="
drop_caches
"$BIN" "${COMMON[@]}" \
  --layout packed \
  --packing coaccess \
  --run-type cold \
  --index "$ROOT/build/v2_linux_packed_coaccess.idx" \
  --coaccess-sessions 96 \
  --coaccess-trace-length 48

echo
echo "== Warm: one-node =="
"$BIN" "${COMMON[@]}" \
  --layout one-node \
  --run-type warm \
  --warmup-runs 1 \
  --index "$ROOT/build/v2_linux_onenode.idx"

echo
echo "== Warm: packed coaccess =="
"$BIN" "${COMMON[@]}" \
  --layout packed \
  --packing coaccess \
  --run-type warm \
  --warmup-runs 1 \
  --index "$ROOT/build/v2_linux_packed_coaccess.idx" \
  --coaccess-sessions 96 \
  --coaccess-trace-length 48

