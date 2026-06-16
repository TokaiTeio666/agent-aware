# Agent-Mem-IO Architecture

This repository is organized around the layering described in `PROJECT_PLAN.md`.
The first refactor pass keeps the existing benchmark behavior intact while
making module ownership explicit.

## Layers

| Layer | Public headers | Sources | Responsibility |
| --- | --- | --- | --- |
| Core | `include/agentmem/core` | `src/core` | Vector containers, L2 distance, exact search baseline |
| Data | `include/agentmem/data` | `src/data` | SIFT fvecs/ivecs loading and reproducible synthetic data |
| Graph | `include/agentmem/graph` | `src/graph` | DiskANN/Vamana-style build, packed node pages, PQ ADC, graph search |
| Storage | `include/agentmem/storage` | `src/storage` | WAL replay, Delta indexes, LSM-style write-path primitives |
| Engine | `include/agentmem/engine` | `src/engine` | Stable API facade for Agent and benchmark integration |
| Benchmark | `include/agentmem/benchmark` | `src/benchmark` | Recall, latency, QPS, and reporting helpers |
| App | none | `src/app` | CLI benchmark executable |

Compatibility shim headers remain in `include/agentmem/*.h`, so older includes
such as `agentmem/graph_index.h` still compile. New code should include the
layered paths directly.

## Current Execution Path

1. `src/app/agent_mem_io_cli.cpp` parses benchmark options and loads data.
2. Memory mode uses `agentmem::search_memory_fast` as the full-precision
   correctness and performance upper-bound baseline. `exact` is retained as a
   compatibility alias.
3. Graph mode builds or opens the disk graph index through
   `agentmem::PackedDiskGraphIndex` or `agentmem::NaiveDiskGraphIndex`.
4. Query routing, path cache, PQ ADC filtering, page cache, and disk I/O stats
   are collected in the graph search result.
5. Mixed workload mode appends writes to `agentmem::WalWriter`, searches Delta
   indexes, merges Main+Delta Top-K results, and optionally runs StreamMerge.
6. Memory-budget reporting estimates resident engine memory against the
   `--memory-budget-ratio` or `--memory-budget-bytes` constraint.

## Refactor Boundaries

- `agentmem_core` is the library target used by both the benchmark executable
  and tests.
- `agent_mem_io_benchmark` emits the historical binary name `agentmem_flow` so
  scripts do not need to change.
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
