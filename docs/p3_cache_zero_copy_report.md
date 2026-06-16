# P3 Cache And Zero-Copy Report

## Scope

This run validates the P3 packed-graph cache path on a small synthetic workload:

- dataset: synthetic, 3,000 base vectors, 200 queries, dim 32
- layout: packed pages, 64 cache pages
- graph: LSH-RP build, degree 16, search width 128, entry count 64
- I/O: pread, no async prefetch
- result files: `archive/results/p3-cache-*.txt`

## Implementation Summary

- Packed pages keep raw page bytes and node vector offsets, so candidate and rerank distance use `compute_distance_direct` on page-resident vector data.
- Packed page cache supports `pin`, `unpin`, `is_pinned`, and `PagePinGuard`.
- Eviction skips pinned pages and records pinned-eviction skips.
- Graph-Aware 2Q uses normal 2Q recency as the main signal with conservative hub/frequency bias.
- `PackedDiskGraphIndex::search_one` is guarded by a mutex for thread-safe access to the shared page cache and page reader.

## Cache Hit-Rate Results

| Policy | Cache Pages | Recall@10 | Hit Rate | Hub Hit Rate | SSD Reads/Q | Avg ms | P95 ms | P99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| No Cache | 0 | 0.9980 | 0.0000 | 0.0000 | 232.6350 | 0.4854 | 0.5611 | 0.5869 |
| LRU | 64 | 0.9980 | 0.9149 | 0.0000 | 19.8050 | 0.2268 | 0.2673 | 0.3707 |
| 2Q | 64 | 0.9980 | 0.8707 | 0.0000 | 30.0850 | 0.2424 | 0.3014 | 0.3359 |
| Graph-Aware 2Q | 64 | 0.9980 | 0.8718 | 0.8794 | 29.8250 | 0.2537 | 0.3076 | 0.3957 |

## Notes

- Graph-Aware 2Q slightly improves over plain 2Q on total hit rate and reads/query in this smoke workload, and exposes hub hit rate separately.
- LRU is still strongest on this particular synthetic access pattern.
- `distance_direct_calls=46527` for all policies, confirming the packed path exercised direct page-resident distance computation.
- No ThreadSanitizer run was performed here; the validation log records build and smoke status at `archive/logs/p3-cache-thread-safety.log`.
