#!/usr/bin/env bash
set -uo pipefail

sync

if [[ -w /proc/sys/vm/drop_caches ]]; then
  if echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; then
    echo "Dropped Linux page cache."
    exit 0
  fi
fi

if command -v sudo >/dev/null 2>&1; then
  if sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null; then
    echo "Dropped Linux page cache with sudo."
    exit 0
  fi
fi

echo "Cannot write /proc/sys/vm/drop_caches in this container. Treat the next run as cold-like, not strict cold." >&2
exit 0
