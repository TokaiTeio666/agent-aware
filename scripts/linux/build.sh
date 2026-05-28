#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT/build"
mkdir -p "$BUILD_DIR"

g++ -std=c++17 -O3 -Wall -Wextra -pedantic \
  -I "$ROOT/include" \
  "$ROOT/src/main.cpp" \
  "$ROOT/src/dataset.cpp" \
  "$ROOT/src/brute_force.cpp" \
  "$ROOT/src/dynamic_index.cpp" \
  "$ROOT/src/graph_index.cpp" \
  "$ROOT/src/metrics.cpp" \
  -o "$BUILD_DIR/agentmem_flow"

echo "Built $BUILD_DIR/agentmem_flow"
