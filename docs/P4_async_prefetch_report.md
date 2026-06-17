# P4 Async Prefetch Report

> Historical note: this report references the older `agentmem_flow` benchmark
> executable, which was removed during P5 cleanup. Use the remaining smoke tests
> and `bench_mixed_rw` for current validation.

## Scope

This report closes the P4 async-prefetch implementation pass for the SSD packed
graph search path:

- I/O modes: `pread`, `odirect`, and `io_uring`
- async path: `io_uring` page reads with bounded in-flight depth
- prefetch policy: topology-oriented `frontier-next-hop`
- statistics: QPS, Avg/P50/P95/P99 latency, I/O submits/completions,
  submit syscalls, prefetch hit rate, dedup ratio, and I/O amplification

The smoke run below validates functionality and instrumentation only. Formal
performance conclusions should still use the SIFT validation method in
`docs/VALIDATION_METHOD.md`.

## Implementation Summary

- `DiskPageReader` owns the platform I/O backend and reports requested vs.
  effective mode through `DiskGraphIoStatus`.
- The `io_uring` path batches queued reads through `submit_async_reads`, tracks
  submit syscall count, and reaps completions into decoded packed pages.
- Packed graph search keeps per-query pending, ready, and completed-prefetch
  page sets so demand reads can wait on already-submitted pages instead of
  issuing duplicate synchronous reads.
- Candidate distance now uses the async-aware local node loader. This preserves
  search semantics while avoiding synchronous page reads when `io_uring`
  completions are pending.
- Next-hop prefetch now honors `prefetch_width`; `io_depth` remains the upper
  bound on in-flight pages.
- CLI compatibility aliases were added for the plan-style options:
  `--async_io`, `--io_backend`, `--io_depth`, `--io_batch_size`,
  `--prefetch`, `--prefetch_width`, `--prefetch_depth`, and `--dedup_pages`.

## Validation

Commands run in WSL ext4:

```bash
cmake -S . -B build
cmake --build build -j2
./build/agent_mem_io_tests
./build/test_disk_index_rw
./build/test_disk_record
```

All tests passed. `agent_mem_io_tests` includes an `io_uring` packed-graph smoke
that compares `pread` vs. `io_uring + frontier-next-hop` result ids when the
runtime reports `io_uring_enabled=1`.

At the time of P4, the removed legacy CLI produced this representative smoke
output:

| Metric | Value |
|---|---:|
| `io_mode_effective` | `io_uring` |
| `io_uring_enabled` | `1` |
| `prefetch_policy` | `frontier-next-hop` |
| `recall_at_10` | `0.9900` |
| `qps` | `1529.1917` |
| `avg_latency_ms` | `0.6536` |
| `p95_latency_ms` | `2.6401` |
| `p99_latency_ms` | `3.2959` |
| `io_batch_size_avg` | `1.0357` |
| `prefetch_hit_rate` | `1.0000` |
| `page_dedup_ratio` | `0.6575` |
| `io_amplification_reads_per_result` | `0.1450` |

## Notes

- The smoke workload is intentionally tiny, so `io_batch_size_avg` is low even
  though the batch path is active.
- The implementation now exposes the metrics needed for the P3-sync,
  P4-uring-batch, and P4-uring-prefetch comparison table.
- SIFT10K/SIFT100K runs should be used for the final QPS and tail-latency claim,
  especially when comparing `pread`, `odirect`, and `io_uring` on ext4/NVMe.
