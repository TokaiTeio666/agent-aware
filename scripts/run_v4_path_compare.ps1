$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
& (Join-Path $Root "scripts/build.ps1")

$Common = @(
  "--engine", "graph",
  "--layout", "packed",
  "--packing", "coaccess",
  "--synthetic",
  "--synthetic-workload", "agent",
  "--session-length", "10",
  "--base-count", "2000",
  "--query-count", "300",
  "--dim", "64",
  "--clusters", "32",
  "--k", "10",
  "--graph-degree", "16",
  "--search-width", "128",
  "--entry-count", "48",
  "--routing-sample-count", "512",
  "--coaccess-sessions", "96",
  "--coaccess-trace-length", "48",
  "--cache-policy", "agent",
  "--cache-pages", "24"
)

$Index = Join-Path $Root "build/v4_packed_coaccess.idx"

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --run-type warm `
  --warmup-runs 1 `
  --path-cache-policy none `
  --path-cache-capacity 0 `
  --index $Index `
  --build-index

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --run-type warm `
  --warmup-runs 1 `
  --path-cache-policy reuse `
  --path-cache-capacity 128 `
  --path-cache-hit-search-width 96 `
  --index $Index
