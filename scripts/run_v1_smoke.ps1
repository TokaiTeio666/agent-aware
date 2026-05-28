$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
& (Join-Path $Root "scripts/build.ps1")
& (Join-Path $Root "build/agentmem_flow.exe") `
  --engine graph `
  --synthetic `
  --base-count 1000 `
  --query-count 100 `
  --dim 32 `
  --clusters 16 `
  --k 10 `
  --index (Join-Path $Root "build/v1_graph.idx") `
  --build-index `
  --graph-degree 16 `
  --search-width 96 `
  --entry-count 32

