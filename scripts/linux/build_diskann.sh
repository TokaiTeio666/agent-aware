#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DISKANN_DIR="${DISKANN_DIR:-$ROOT/third_party/DiskANN}"

echo "system_name=DiskANN"
echo "diskann_dir=$DISKANN_DIR"

if [[ ! -d "$DISKANN_DIR" ]]; then
  echo "status=missing_source"
  echo "notes=Place_a_DiskANN_checkout_in_third_party/DiskANN_or_set_DISKANN_DIR"
  exit 2
fi

if [[ -n "${DISKANN_BUILD_COMMAND:-}" ]]; then
  (
    cd "$DISKANN_DIR"
    bash -lc "$DISKANN_BUILD_COMMAND"
  )
elif [[ -x "$DISKANN_DIR/build.sh" ]]; then
  (
    cd "$DISKANN_DIR"
    bash ./build.sh -r
  )
else
  cmake -S "$DISKANN_DIR" -B "$DISKANN_DIR/build" \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "$DISKANN_DIR/build" --config Release -j "${BUILD_JOBS:-4}"
fi

echo "status=built"

