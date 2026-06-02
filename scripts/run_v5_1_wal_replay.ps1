$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
& (Join-Path $Root "scripts/build.ps1")

$RunDir = Join-Path $Root "build/v5_1_wal_replay"
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

$Common = @(
  "--engine", "graph",
  "--layout", "packed",
  "--packing", "coaccess",
  "--synthetic",
  "--synthetic-workload", "agent",
  "--session-length", "10",
  "--base-count", "1200",
  "--query-count", "160",
  "--dim", "32",
  "--clusters", "16",
  "--k", "10",
  "--graph-degree", "16",
  "--search-width", "96",
  "--entry-count", "32",
  "--routing-sample-count", "128",
  "--coaccess-sessions", "64",
  "--coaccess-trace-length", "32",
  "--cache-policy", "agent",
  "--cache-pages", "16",
  "--path-cache-policy", "reuse",
  "--path-cache-capacity", "64",
  "--path-cache-hit-search-width", "64",
  "--query-signature-policy", "routed",
  "--delta-index-policy", "flat",
  "--compaction-policy", "none",
  "--run-type", "warm",
  "--warmup-runs", "1"
)

$Index = Join-Path $RunDir "v5_1_packed_coaccess.idx"
$Wal = Join-Path $RunDir "v5_1_replay.wal"

Write-Output "variant=wal_source_run"
& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --workload-mode mixed `
  --operation-count 160 `
  --write-ratio 20 `
  --wal $Wal `
  --index $Index `
  --build-index

Write-Output "variant=wal_replay_run"
& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --workload-mode mixed `
  --operation-count 160 `
  --write-ratio 0 `
  --wal $Wal `
  --wal-replay `
  --index $Index
