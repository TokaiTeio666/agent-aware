$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
& (Join-Path $Root "scripts/build.ps1")

$RunDir = Join-Path $Root "build/v5_2_delta_ann"
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

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
  "--cache-pages", "24",
  "--path-cache-policy", "reuse",
  "--path-cache-capacity", "128",
  "--path-cache-hit-search-width", "96",
  "--query-signature-policy", "routed",
  "--workload-mode", "mixed",
  "--operation-count", "600",
  "--write-ratio", "50",
  "--compaction-policy", "none",
  "--run-type", "warm",
  "--warmup-runs", "1"
)

$Index = Join-Path $RunDir "v5_2_packed_coaccess.idx"

Write-Output "variant=delta_flat"
& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --delta-index-policy flat `
  --wal (Join-Path $RunDir "v5_2_flat.wal") `
  --index $Index `
  --build-index

Write-Output "variant=delta_ivf_flat"
& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --delta-index-policy ivf-flat `
  --delta-ivf-centroids 32 `
  --delta-ivf-probes 16 `
  --delta-ivf-train-iterations 8 `
  --delta-ivf-rebuild-interval 32 `
  --wal (Join-Path $RunDir "v5_2_ivf.wal") `
  --index $Index
