# AgentMem-Flow

AgentMem-Flow is an Agent-memory-oriented vector retrieval I/O engine prototype.
The current codebase contains V0-V5 baselines: exact search, naive SSD graph
search, packed page layout, Agent-aware cache, query path cache, and a minimal
dynamic write path with WAL + Delta Index + SLA-aware compaction. V6 adds
file-backed compaction I/O and AutoDL deployment scripts.

## V0 features

- In-memory brute-force Top-K search with squared L2 distance.
- SIFT `fvecs` base/query loader.
- SIFT `ivecs` ground-truth loader for Recall@K.
- Clustered synthetic data generator for quick smoke tests.
- Metrics: Recall@K, QPS, average latency, P50, P95, P99.

## V1 features

- Naive DiskANN-style graph baseline with one fixed-size node page per vector.
- Exact kNN graph builder for small/medium experimental datasets.
- Binary graph index file with header plus 4 KB node pages by default.
- Graph ANN search with configurable degree, search width, and entry count.
- Resident sampled router for query-specific entry selection. This is a V1
  engineering stand-in for a future PQ/centroid router.
- SSD-style counters: reads/query, graph expansions/query, visited nodes/query,
  and read amplification per returned result.

## V2 features

- Packed page layout with multiple graph nodes in one fixed-size page.
- Page-level SoA layout inside each packed page: node ids, degrees, vectors, and
  neighbor blocks are stored in separate contiguous regions.
- Packing strategies: `random`, `bfs`, and `coaccess`.
- Agent-style co-access packing based on synthetic session traces over graph
  paths.
- Cold/warm run labels and warmup support via `--run-type` and
  `--warmup-runs`.
- V2 comparison script: `scripts/run_v2_compare.ps1`.

## V3 features

- Global packed page cache across queries.
- Cache policies: `none`, `lru`, and `agent`.
- Agent-aware eviction score combines frequency, recency, and page density.
- Cache metrics: page cache hit rate, hits/query, misses/query, and
  requests/query.
- V3 comparison script: `scripts/run_v3_cache_compare.ps1`.

## V4 features

- Agent-style synthetic workload with consecutive session-local queries.
- Query signature based on resident routed entry.
- Query Path Cache with seed and Top-K reuse.
- Reduced search width on path-cache hits.
- Path cache metrics: hit rate, hits/query, requests/query.
- V4 comparison script: `scripts/run_v4_path_compare.ps1`.

## V5 features

- WAL append path for inserted Agent memory vectors.
- In-memory Delta flat index with active and sealed segments.
- Query-time Top-K merge between Main Graph ANN results and Delta exact results.
- Mixed read/write workload controls via `--workload-mode mixed`,
  `--operation-count`, and `--write-ratio`.
- Compaction policies: `none`, `aggressive`, and `sla`.
- SLA-aware compaction shifts merge work away from the query critical path.
- Dynamic metrics: insert latency, WAL records/bytes, delta active/sealed size,
  compaction ops, and compaction interference.
- V5 comparison script: `scripts/run_v5_mixed_compare.ps1`.

## V6 features

- File-backed compaction I/O via `--compaction-io-mode file`.
- Linux file compaction path uses write + fsync to create measurable I/O work.
- Metric: `compaction_io_bytes`.
- Local V6 comparison script: `scripts/run_v6_fileio_compare.ps1`.
- AutoDL deployment scripts under `scripts/autodl/`.
- AutoDL deployment guide: `docs/AUTODL_DEPLOYMENT.md`.

## Iteration archive policy

Each version iteration must archive both analysis and measured results:

- Analysis reports: `docs/iterations/vX-analysis.md`
- Raw or summarized run results: `archive/results/vX-*.txt`
- Experiment configs: `archive/configs/vX-*.json`
- Raw logs: `archive/logs/vX-*.log`
- Build and environment info: `archive/build_info/vX-*.txt`
- Validation method snapshots: `archive/validation/*.md`

Every version report should record the goal, implementation scope, benchmark
command, random seed, run type, pass criteria, metrics, interpretation, risks,
and next-step recommendation. Temporary large experiment outputs can still use
the ignored `results/` directory, but final evidence used for reports should be
copied into `archive/`.

The formal validation method is maintained in `docs/VALIDATION_METHOD.md`.
The project baseline and roadmap are maintained in `docs/PROJECT_PLAN.md`.
WSL/Linux strict cold-start instructions are maintained in
`docs/WSL_COLD_START.md`.

## Build

On this workspace, CMake is optional. The provided PowerShell script builds with
`g++`:

```powershell
.\scripts\build.ps1
```

If CMake is available:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Run smoke tests

```powershell
.\scripts\run_v0_smoke.ps1
.\scripts\run_v1_smoke.ps1
.\scripts\run_v2_compare.ps1
.\scripts\run_v3_cache_compare.ps1
.\scripts\run_v4_path_compare.ps1
.\scripts\run_v5_mixed_compare.ps1
.\scripts\run_v6_fileio_compare.ps1
```

Equivalent direct V0 command:

```powershell
.\build\agentmem_flow.exe --engine exact --synthetic --base-count 1000 --query-count 100 --dim 32 --clusters 16 --k 10
```

Equivalent direct V1 command:

```powershell
.\build\agentmem_flow.exe `
  --engine graph `
  --synthetic `
  --base-count 1000 `
  --query-count 100 `
  --dim 32 `
  --clusters 16 `
  --k 10 `
  --index build\v1_graph.idx `
  --build-index `
  --graph-degree 16 `
  --search-width 96 `
  --entry-count 32 `
  --routing-sample-count 256
```

Equivalent direct V2 co-access command:

```powershell
.\build\agentmem_flow.exe `
  --engine graph `
  --layout packed `
  --packing coaccess `
  --run-type cold `
  --synthetic `
  --base-count 2000 `
  --query-count 200 `
  --dim 64 `
  --clusters 32 `
  --k 10 `
  --index build\v2_packed_coaccess_cold.idx `
  --build-index `
  --graph-degree 16 `
  --search-width 128 `
  --entry-count 48 `
  --routing-sample-count 512 `
  --coaccess-sessions 96 `
  --coaccess-trace-length 48
```

## Run on SIFT fvecs/ivecs files

```powershell
.\build\agentmem_flow.exe `
  --engine exact `
  --base data\sift_base.fvecs `
  --query data\sift_query.fvecs `
  --truth data\sift_groundtruth.ivecs `
  --base-limit 100000 `
  --query-limit 1000 `
  --k 10
```

When `--truth` is omitted, V0 reports `truth=exact_self`; this is useful for
smoke testing, but real recall comparisons should use an external ground-truth
file or a future approximate index compared against this exact baseline.

For V1 graph search, omit `--truth` only for small experiments where exact
ground truth can be computed on the fly:

```powershell
.\build\agentmem_flow.exe `
  --engine graph `
  --base data\sift_base.fvecs `
  --query data\sift_query.fvecs `
  --truth data\sift_groundtruth.ivecs `
  --index build\sift_v1_graph.idx `
  --build-index `
  --graph-degree 16 `
  --search-width 128 `
  --entry-count 48 `
  --routing-sample-count 512
```
