#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-$ROOT/archive/build_info/v6-autodl-env.txt}"
mkdir -p "$(dirname "$OUT")"

{
  echo "date: $(date -Is)"
  echo "hostname: $(hostname)"
  echo "pwd: $(pwd)"
  echo "git_commit_hash: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo no commit yet)"
  echo "git_status:"
  git -C "$ROOT" status --short || true
  echo
  echo "os:"
  uname -a
  if [[ -f /etc/os-release ]]; then
    cat /etc/os-release
  fi
  echo
  echo "compiler:"
  g++ --version | head -n 1 || true
  echo
  echo "cpu:"
  lscpu || true
  echo
  echo "memory:"
  free -h || true
  echo
  echo "disk:"
  df -h "$ROOT" || true
  lsblk || true
  echo
  echo "gpu:"
  nvidia-smi || true
  echo
  echo "limits:"
  ulimit -a || true
} > "$OUT"

echo "Wrote $OUT"
