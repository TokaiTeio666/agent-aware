# Agent-Mem-IO Architecture

This repository is organized around the layering described in `PROJECT_PLAN.md`.
The current P5 pass keeps the core search modules intact while replacing the
older benchmark executable with the dynamic mixed read/write benchmark.

## Layers

| Layer | Public headers | Sources | Responsibility |
| --- | --- | --- | --- |
| Core | `include/agentmem/core` | `src/core` | Vector containers, L2 distance, exact search baseline |
| Data | `include/agentmem/data` | `src/data` | SIFT fvecs/ivecs loading and reproducible synthetic data |
| Graph | `include/agentmem/graph` | `src/graph` | DiskANN/Vamana-style build, packed node pages, PQ ADC, graph search |
| Storage | `include/agentmem/storage` | `src/storage` | WAL replay, Delta indexes, LSM-style write-path primitives |
| Engine | `include/agentmem/engine` | `src/engine` | Stable API facade for Agent and benchmark integration |
| Dynamic | `include/agentmem/dynamic` | `src/dynamic` | P5 WAL, MemTable, SSTable, Compaction, Manifest, dynamic write manager |
| Benchmark | none | `bench` | Standalone benchmark executables |

Compatibility shim headers remain in `include/agentmem/*.h`, so older includes
such as `agentmem/graph_index.h` still compile. New code should include the
layered paths directly.

## Current Execution Path

1. Static graph tests build or open the disk graph index through
   `agentmem::PackedDiskGraphIndex` or `agentmem::NaiveDiskGraphIndex`.
2. Query routing, path cache, PQ ADC filtering, page cache, and disk I/O stats
   are collected in the graph search result.
3. P5 dynamic writes flow through WAL, MemTable, SSTable, Manifest, optional
   Compaction, and dynamic result merge.
4. `bench_mixed_rw` runs the P5 mixed read/write validation path.

## Refactor Boundaries

- `agentmem_core` is the library target used by the benchmark executable and
  tests.
- The historical `agentmem_flow` benchmark target has been removed in favor of
  `bench_mixed_rw`.
- `agent_mem_io_tests` covers the lowest-risk correctness smoke checks:
  synthetic data, brute-force search, WAL replay, Delta search, PQ ADC, packed
  graph build/search, and the new StorageEngine facade.

## Next Internal Split

`src/graph/disk_graph_index.cpp` still contains several submodules in one
translation unit. The next low-risk split is:

- `graph_build.cpp`: exact/approx/LSH candidate graph construction and robust
  prune.
- `disk_layout.cpp`: headers, page packing, decode, and metadata validation.
- `page_io.cpp`: pread, O_DIRECT, io_uring fallback, and prefetch accounting.
- `graph_cache.cpp`: LRU, 2Q, and graph-aware 2Q page eviction.
- `pq_adc.cpp`: PQ training, code storage, ADC table, and rerank helpers.
- `graph_search.cpp`: Naive and packed search loops.

That split should be behavior-preserving and guarded by the current smoke tests
plus at least one synthetic benchmark run.
