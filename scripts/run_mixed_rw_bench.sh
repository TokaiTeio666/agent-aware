#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUTPUT_PATH="${BUILD_DIR}/p5_mixed_rw.csv"
DYNAMIC_DIR="${BUILD_DIR}/p5_mixed_rw_dynamic"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --target bench_mixed_rw

"${BUILD_DIR}/bench_mixed_rw" \
  --dynamic_dir "${DYNAMIC_DIR}" \
  --output "${OUTPUT_PATH}" \
  "$@"
