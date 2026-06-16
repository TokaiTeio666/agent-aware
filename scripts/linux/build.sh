#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT/build"
CXX="${CXX:-g++}"
mkdir -p "$BUILD_DIR"

LIBURING_FLAGS=(-DAGENTMEM_HAS_LIBURING=0)
LIBURING_LIBS=()
if printf '#include <liburing.h>\nint main(){return 0;}\n' | \
  "$CXX" -x c++ -std=c++17 - -luring -o "$BUILD_DIR/.check_liburing" \
    >/dev/null 2>&1; then
  LIBURING_FLAGS=(-DAGENTMEM_HAS_LIBURING=1)
  LIBURING_LIBS=(-luring)
fi
rm -f "$BUILD_DIR/.check_liburing"

COMMON_SOURCES=(
  "$ROOT/src/benchmark/metrics.cpp"
  "$ROOT/src/core/brute_force.cpp"
  "$ROOT/src/core/pq_encoder.cpp"
  "$ROOT/src/data/dataset.cpp"
  "$ROOT/src/engine/storage_engine.cpp"
  "$ROOT/src/graph/disk_graph_index.cpp"
  "$ROOT/src/graph/vamana_builder.cpp"
  "$ROOT/src/storage/lsm_tree.cpp"
)

"$CXX" -std=c++17 -O3 -Wall -Wextra -pedantic \
  "${LIBURING_FLAGS[@]}" \
  -I "$ROOT/include" \
  "$ROOT/src/app/agent_mem_io_cli.cpp" \
  "${COMMON_SOURCES[@]}" \
  "${LIBURING_LIBS[@]}" \
  -o "$BUILD_DIR/agentmem_flow"

"$CXX" -std=c++17 -O3 -Wall -Wextra -pedantic \
  "${LIBURING_FLAGS[@]}" \
  -I "$ROOT/include" \
  "$ROOT/tests/smoke_tests.cpp" \
  "${COMMON_SOURCES[@]}" \
  "${LIBURING_LIBS[@]}" \
  -o "$BUILD_DIR/agent_mem_io_tests"

echo "Built $BUILD_DIR/agentmem_flow"
echo "Built $BUILD_DIR/agent_mem_io_tests"
