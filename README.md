# AgentMem-Flow

AgentMem-Flow is an Agent-memory-oriented vector retrieval I/O engine prototype.
The current codebase contains V0-V9 baselines: exact search, naive SSD graph
search, packed page layout, Agent-aware cache, query path cache, WAL + Delta
Index + SLA-aware compaction, file-backed compaction I/O, query signature
policy comparison, FreshVamana + LSM-style StreamMerge for delete-heavy
dynamic updates, FreshLSH-Vamana construction for SIFT1M-oriented builds, and
memory-budget accounting for 10%-20% resident-engine-memory experiments.

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
- Query signatures for path reuse: resident routed entry, SimHash, PQ prefix,
  and SimHash+PQ hybrid.
- Query Path Cache with seed and Top-K reuse.
- Reduced search width on path-cache hits.
- Path cache metrics: hit rate, hits/query, requests/query.
- V4 comparison script: `scripts/run_v4_path_compare.ps1`.

## V5 features

- WAL append path for inserted Agent memory vectors.
- WAL replay via `--wal-replay` to restore delta records after restart.
- In-memory Delta flat index with active and sealed segments.
- Optional Delta IVF-flat ANN index via `--delta-index-policy ivf-flat`,
  with farthest-point centroid initialization, periodic k-means rebuilds, and
  flat delta retained as the correctness reference.
- Query-time Top-K merge between Main Graph ANN results and Delta exact results.
- Mixed read/write workload controls via `--workload-mode mixed`,
  `--operation-count`, and `--write-ratio`.
- Compaction policies: `none`, `aggressive`, and `sla`.
- SLA-aware compaction shifts merge work away from the query critical path.
- Dynamic metrics: insert latency, WAL records/bytes, delta active/sealed size,
  WAL replay records/bytes, delta search latency, delta recall, compaction ops,
  and compaction interference.
- V5 comparison script: `scripts/run_v5_mixed_compare.ps1`.
- V5.1 WAL replay script: `scripts/run_v5_1_wal_replay.ps1`.
- V5.2 Delta ANN script: `scripts/run_v5_2_delta_ann_compare.ps1`.

Delta IVF-flat tuning knobs:

- `--delta-ivf-centroids 32`
- `--delta-ivf-probes 16`
- `--delta-ivf-train-iterations 6`
- `--delta-ivf-rebuild-interval 32`

## V6 features

- File-backed compaction I/O via `--compaction-io-mode file`.
- Linux file compaction path uses write + fsync to create measurable I/O work.
- Metric: `compaction_io_bytes`.
- Local V6 comparison script: `scripts/run_v6_fileio_compare.ps1`.
- AutoDL deployment scripts under `scripts/autodl/`.
- AutoDL deployment guide: `docs/AUTODL_DEPLOYMENT.md`.

## V7 features

- Query signature policy comparison script:
  `scripts/run_v7_signature_compare.ps1`.
- Archived comparison for `routed`, `simhash`, `pq-prefix`, and `simhash-pq`.
- V7 metrics focus: path cache hit rate, SSD reads/query, expanded/query, and
  Recall@10 stability across signature policies.

## V8 features

- Approximate graph builder via `--graph-build-policy approx-rp`.
- FreshVamana-style RobustPrune via `--robust-prune-alpha`.
- Delete tombstones in mixed workloads via `--delete-ratio`.
- WAL insert/delete records and replay counters.
- LSM-style StreamMerge via `--compaction-policy stream-merge`.
- StreamMerge output index via `--stream-merge-index`.
- SIFT local helper defaults to approx-rp + BFS packing for SIFT100K/SIFT1M
  development runs.

## V9 features

- FreshLSH-Vamana graph construction via `--graph-build-policy lsh-rp`.
- Multi-table SimHash LSH candidate generation:
  `--lsh-tables`, `--lsh-bits`, `--lsh-probe-radius`,
  `--lsh-bucket-limit`.
- Batch reciprocal edge pruning replaces per-edge reverse insertion.
- Batched FreshVamana delete patch avoids scanning the graph once per delete.
- Optional adaptive graph search stop via `--search-early-stop` and
  `--search-early-stop-min`.
- SIFT helper defaults now target SIFT1M construction with `lsh-rp`.

## Vamana builder

- Standalone Vamana graph construction is available through
  `agentmem::VamanaBuilder`.
- The benchmark can build disk indexes with `--graph-build-policy vamana`,
  using medoid routing, `search_for_construction`, RobustPrune, and reverse-edge
  re-pruning before writing one-node or packed graph pages.

## Memory budget controls

`--memory-budget-ratio` defaults to `0.20` and reports whether estimated
resident engine memory fits within that fraction of the raw vector bytes.
`--memory-budget-bytes` can set a stricter absolute cap. Use
`--enforce-memory-budget` to fail over-budget runs, or
`--allow-over-budget-for-debug` to keep collecting metrics while still
reporting `memory_budget_pass=0`.

Key output fields:

- `memory_budget_bytes`
- `memory_resident_bytes`
- `memory_resident_ratio`
- `memory_budget_pass`
- `memory_accounting_scope=engine_resident`
- `memory_bytes_*` component breakdown

## Query signature policies

`--query-signature-policy` controls the key used by Query Path Cache:

- `routed`: V4 baseline; use the nearest resident routed entry id.
- `simhash`: random-hyperplane SimHash over the query vector.
- `pq-prefix`: train small resident-sample PQ codebooks and hash the prefix
  codes.
- `simhash-pq`: combine SimHash and PQ prefix for a more selective signature.

Useful knobs:

- `--simhash-bits 16`
- `--pq-prefix-subspaces 4`
- `--pq-prefix-centroids 16`
- `--pq-prefix-train-iterations 4`

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
The project baseline and roadmap are maintained in `PROJECT_PLAN.md`.
WSL/Linux validation entry points live under `scripts/linux/`.

## Source layout

The implementation is split to match the project plan:

- `include/agentmem/core` and `src/core`: vector types, distance, exact search.
- `include/agentmem/data` and `src/data`: SIFT and synthetic data loading.
- `include/agentmem/graph` and `src/graph`: DiskANN/Vamana-style graph build,
  packed pages, PQ ADC filtering, cache-aware search, and disk I/O hooks.
- `include/agentmem/storage` and `src/storage`: WAL, Delta indexes, and
  LSM-style write-path components.
- `include/agentmem/engine` and `src/engine`: public StorageEngine facade for
  Agent or benchmark integration.
- `include/agentmem/benchmark` and `src/benchmark`: metrics and reporting
  helpers.
- `src/app`: benchmark CLI. The executable output name remains
  `agentmem_flow` for compatibility with existing scripts.

## Build

On WSL/Linux, the direct build script uses `g++` and produces both the
benchmark CLI and smoke test binary:

```bash
bash scripts/linux/build.sh
```

If CMake is available:

```bash
cmake -S . -B build
cmake --build build
```

## Run smoke tests

```bash
./build/agent_mem_io_tests
```

Equivalent direct V0 memory command:

```powershell
.\build\agentmem_flow.exe --engine memory --synthetic --base-count 1000 --query-count 100 --dim 32 --clusters 16 --k 10
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
  --query-signature-policy routed `
  --coaccess-sessions 96 `
  --coaccess-trace-length 48
```

## Run on SIFT fvecs/ivecs files

On WSL2/Ubuntu, put the official SIFT files in `data/sift/`:

```text
data/sift/sift_base.fvecs
data/sift/sift_query.fvecs
data/sift/sift_groundtruth.ivecs
```

Then run a small local smoke benchmark:

```bash
bash scripts/linux/run_sift_local.sh
```

Useful WSL overrides:

```bash
SIFT_DIR=/path/to/sift BASE_LIMIT=100000 QUERY_LIMIT=1000 ENGINE=memory \
  bash scripts/linux/run_sift_local.sh
```

For graph runs on SIFT, the Linux helper defaults to a no-cache baseline with
the FreshLSH-Vamana builder (`GRAPH_BUILD_POLICY=lsh-rp`) and BFS packing. This
avoids the old O(N^2) exact kNN graph build path and is the recommended first
run for SIFT100K/SIFT1M experiments:

```bash
SIFT_DIR=/path/to/sift ENGINE=graph BASE_LIMIT=100000 QUERY_LIMIT=1000 \
  BUILD_INDEX=1 bash scripts/linux/run_sift_local.sh
```

Tuning knobs:

```bash
GRAPH_DEGREE=32 SEARCH_WIDTH=1024 ENTRY_COUNT=256 ROUTING_SAMPLE_COUNT=4096 \
APPROX_WINDOW=32 APPROX_CANDIDATE_LIMIT=192 \
LSH_TABLES=8 LSH_BITS=14 LSH_BUCKET_LIMIT=64 SEARCH_EARLY_STOP_MIN=192 \
ROBUST_PRUNE_ALPHA=1.2
```

The official `sift_groundtruth.ivecs` file is for the full SIFT1M base. If
`--base-limit` loads only a subset, the binary now detects out-of-range truth
ids and recomputes exact ground truth for the loaded subset before reporting
Recall@K.

```powershell
.\build\agentmem_flow.exe `
  --engine memory `
  --base data\sift\sift_base.fvecs `
  --query data\sift\sift_query.fvecs `
  --truth data\sift\sift_groundtruth.ivecs `
  --base-limit 100000 `
  --query-limit 1000 `
  --k 10
```

Equivalent Linux command using the official directory layout:

```bash
./build/agentmem_flow \
  --engine memory \
  --sift-dir data/sift \
  --base-limit 100000 \
  --query-limit 1000 \
  --k 10
```

`--engine exact` remains accepted as a compatibility alias for the same
`search_memory_fast` full-precision in-memory path.

Equivalent Linux graph command without the helper:

```bash
./build/agentmem_flow \
  --engine graph \
  --sift-dir data/sift \
  --base-limit 100000 \
  --query-limit 1000 \
  --layout packed \
  --packing bfs \
  --build-index \
  --graph-build-policy lsh-rp \
  --graph-degree 32 \
  --approx-window 32 \
  --approx-random-samples 32 \
  --approx-candidate-limit 192 \
  --lsh-tables 8 \
  --lsh-bits 14 \
  --lsh-bucket-limit 64 \
  --search-width 1024 \
  --search-early-stop \
  --search-early-stop-min 192 \
  --entry-count 256 \
  --routing-sample-count 4096 \
  --k 10
```

FreshVamana + LSM-style StreamMerge mixed workload:

```bash
SIFT_DIR=/path/to/sift \
ENGINE=graph BASE_LIMIT=100000 QUERY_LIMIT=1000 BUILD_INDEX=1 \
WORKLOAD_MODE=mixed OPERATION_COUNT=5000 WRITE_RATIO=20 DELETE_RATIO=50 \
COMPACTION_POLICY=stream-merge \
bash scripts/linux/run_sift_local.sh
```

In this mode, inserts go through the writable TempIndex/WAL path, deletes are
recorded as tombstones and filtered from query results, and the final
StreamMerge writes a new LTI with FreshVamana delete patching and RobustPrune.
To query the merged LTI in a later run, set `INDEX` to the generated
`STREAM_MERGE_INDEX` path and leave `BUILD_INDEX=0`.

When `--truth` is omitted, V0 reports `truth=exact_self`; this is useful for
smoke testing, but real recall comparisons should use an external ground-truth
file or a future approximate index compared against this exact baseline.

For V1 graph search, omit `--truth` only for small experiments where exact
ground truth can be computed on the fly:

```powershell
.\build\agentmem_flow.exe `
  --engine graph `
  --base data\sift\sift_base.fvecs `
  --query data\sift\sift_query.fvecs `
  --truth data\sift\sift_groundtruth.ivecs `
  --index build\sift_v1_graph.idx `
  --build-index `
  --graph-degree 16 `
  --search-width 128 `
  --entry-count 48 `
  --routing-sample-count 512
```
