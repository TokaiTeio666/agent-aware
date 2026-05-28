$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
& (Join-Path $Root "scripts/build.ps1")

$Common = @(
  "--engine", "graph",
  "--synthetic",
  "--base-count", "2000",
  "--query-count", "200",
  "--dim", "64",
  "--clusters", "32",
  "--k", "10",
  "--graph-degree", "16",
  "--search-width", "128",
  "--entry-count", "48",
  "--routing-sample-count", "512"
)

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --layout one-node `
  --run-type cold `
  --index (Join-Path $Root "build/v2_onenode_cold.idx") `
  --build-index

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --layout packed `
  --packing random `
  --run-type cold `
  --index (Join-Path $Root "build/v2_packed_random_cold.idx") `
  --build-index `
  --coaccess-sessions 96 `
  --coaccess-trace-length 48

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --layout packed `
  --packing bfs `
  --run-type cold `
  --index (Join-Path $Root "build/v2_packed_bfs_cold.idx") `
  --build-index `
  --coaccess-sessions 96 `
  --coaccess-trace-length 48

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --layout packed `
  --packing coaccess `
  --run-type cold `
  --index (Join-Path $Root "build/v2_packed_coaccess_cold.idx") `
  --build-index `
  --coaccess-sessions 96 `
  --coaccess-trace-length 48

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --layout one-node `
  --run-type warm `
  --warmup-runs 1 `
  --index (Join-Path $Root "build/v2_onenode_cold.idx")

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --layout packed `
  --packing coaccess `
  --run-type warm `
  --warmup-runs 1 `
  --index (Join-Path $Root "build/v2_packed_coaccess_cold.idx") `
  --coaccess-sessions 96 `
  --coaccess-trace-length 48

