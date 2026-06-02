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
  "--cache-pages", "24",
  "--path-cache-policy", "reuse",
  "--path-cache-capacity", "128",
  "--path-cache-hit-search-width", "96",
  "--query-signature-policy", "routed",
  "--simhash-bits", "16",
  "--pq-prefix-subspaces", "4",
  "--pq-prefix-centroids", "16",
  "--pq-prefix-train-iterations", "4",
  "--workload-mode", "mixed",
  "--operation-count", "360",
  "--write-ratio", "20",
  "--delta-index-policy", "flat",
  "--delta-ivf-centroids", "32",
  "--delta-ivf-probes", "16",
  "--delta-ivf-train-iterations", "6",
  "--delta-ivf-rebuild-interval", "32",
  "--delta-compaction-threshold", "24",
  "--compaction-batch-size", "8",
  "--compaction-work-us", "700",
  "--run-type", "warm",
  "--warmup-runs", "1"
)

$Index = Join-Path $Root "build/v5_packed_coaccess.idx"

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --compaction-policy none `
  --wal (Join-Path $Root "build/v5_none.wal") `
  --index $Index `
  --build-index

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --compaction-policy aggressive `
  --wal (Join-Path $Root "build/v5_aggressive.wal") `
  --index $Index

& (Join-Path $Root "build/agentmem_flow.exe") @Common `
  --compaction-policy sla `
  --sla-p99-ms "0.6" `
  --wal (Join-Path $Root "build/v5_sla.wal") `
  --index $Index
