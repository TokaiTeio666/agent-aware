# SIFT Parameter Matrix Audit

| Parameter | Role | Why it is expected to affect results | Expected moving metrics |
| --- | --- | --- | --- |
| `search_width` | primary | Directly bounds DiskGraphSearchConfig.search_width and the number of graph expansions. | Recall, visited/page reads, QPS, P95/P99 latency. |
| `beam_width` | primary | Directly controls per-round candidate expansion and I/O batch size. | avg_batch_size, io_submit_syscalls, QPS, P95/P99 latency. |
| `memory_budget_ratio` | primary when cache_pages is auto | Determines automatic cache_pages if cache_pages is not explicitly set. | cache_pages/cache_bytes, cache_hit_rate, P95/P99 latency. |
| `io_mode` | primary | Flows into PackedDiskGraphIndex.configure_io and AsyncPageReader. | io_effective_mode, odirect/io_uring status, I/O submit/completion stats, latency. |

Primary matrix axes are `search_width`, `beam_width`, `memory_budget_ratio`, and `io_mode`.
`prefetch_depth` is intentionally secondary because the current binary supports only 0/1 and it is ignored unless effective I/O mode is `io_uring`.
