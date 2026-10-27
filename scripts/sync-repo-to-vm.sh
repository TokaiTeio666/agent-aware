#!/usr/bin/env bash
# 把 Windows 侧仓库复制到 VM 的 ext4 原生文件系统。
# 刻意不走 VMware 共享文件夹：vmhgfs 会让 cmake 构建和基准 I/O 数字失真。
#
# 用法（Windows Git Bash）：
#   bash scripts/sync-repo-to-vm.sh <vm-ip>
set -euo pipefail

VM_HOST="${1:-}"
VM_USER="${VM_USER:-dev}"
VM_PATH="${VM_PATH:-/data/agent-aware}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "$VM_HOST" ]]; then
  echo "用法: bash scripts/sync-repo-to-vm.sh <vm-ip>" >&2
  echo "在 VM 里查 IP: ip -4 addr show | grep inet" >&2
  exit 1
fi

echo "== $SRC_DIR -> $VM_USER@$VM_HOST:$VM_PATH =="

# 排除构建产物、索引、临时目录；保留 .git 以便追溯 commit
tar -czf - -C "$SRC_DIR" \
    --exclude='build' \
    --exclude='indexes' \
    --exclude='logs' \
    --exclude='*.idx' \
    --exclude='*.pptx' \
    --exclude='.claude' \
    --exclude='*.exe' \
    . \
  | ssh -o StrictHostKeyChecking=accept-new "$VM_USER@$VM_HOST" \
      "mkdir -p '$VM_PATH' && tar -xzf - -C '$VM_PATH'"

echo "== 完成 =="
echo "在 VM 内构建："
echo "  cd $VM_PATH"
echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \\"
echo "        -DCMAKE_CXX_FLAGS='-O3 -march=native -Wall -Wextra'"
echo "  cmake --build build -j\$(nproc)"
