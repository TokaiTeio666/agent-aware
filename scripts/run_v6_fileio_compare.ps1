$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
& (Join-Path $Root "scripts/build.ps1")

$RunDir = Join-Path $Root "build/v6_fileio_local"
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
  "--workload-mode", "mixed",
  "--operation-count", "360",
  "--write-ratio", "20",
  "--delta-compaction-threshold", "24",
  "--compaction-batch-size", "8",
  "--compaction-work-us", "0",
  "--compaction-io-mode", "file",
  "--compaction-io-bytes-per-vector", "4096",
  "--run-type", "warm",
  "--warmup-runs", "1"
)

$Index = Join-Path $RunDir "v6_packed_coaccess.idx"

Write-Output "variant=no_compaction"
& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --compaction-policy none `
  --wal (Join-Path $RunDir "v6_none.wal") `
  --compaction-io-path (Join-Path $RunDir "v6_none_compaction.bin") `
  --index $Index `
  --build-index

Write-Output "variant=aggressive_file_io"
& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --compaction-policy aggressive `
  --wal (Join-Path $RunDir "v6_aggressive.wal") `
  --compaction-io-path (Join-Path $RunDir "v6_aggressive_compaction.bin") `
  --index $Index

Write-Output "variant=sla_file_io"
& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --compaction-policy sla `
  --sla-p99-ms "0.8" `
  --wal (Join-Path $RunDir "v6_sla.wal") `
  --compaction-io-path (Join-Path $RunDir "v6_sla_compaction.bin") `
  --index $Index
