# P5 Mixed Read/Write Benchmark

## Purpose

`bench_mixed_rw` validates the P5 dynamic write path:

- WAL append
- MemTable insert
- SSTable flush
- manifest reload
- delta-layer L2 search
- base + delta result merge
- recovery after close/reopen

The benchmark replaces the older `agentmem_flow`/sync-disk benchmark targets for
P5 validation.

## Build And Run

```bash
cmake -S . -B build
cmake --build build --target bench_mixed_rw

./build/bench_mixed_rw \
  --dynamic_dir build/p5_mixed_rw_dynamic \
  --output build/p5_mixed_rw.csv \
  --num_operations 1000 \
  --topk 10
```

Or use the helper:

```bash
bash scripts/run_mixed_rw_bench.sh --num_operations 1000
```

## Workloads

By default the benchmark runs three scenarios:

| Scenario | Read Ratio | Write Ratio |
|---|---:|---:|
| read-heavy | 0.95 | 0.05 |
| balanced | 0.70 | 0.30 |
| write-heavy | 0.50 | 0.50 |

For a single custom scenario:

```bash
./build/bench_mixed_rw --read_ratio 0.8 --write_ratio 0.2
```

## Key Parameters

```text
--data_path
--index_path
--dynamic_dir
--num_operations
--read_ratio
--write_ratio
--topk
--insert_batch_size
--enable_flush
--enable_compaction
--output
--base_count
--query_count
--dim
--memtable_flush_bytes
```

`--data_path` can load fvecs base vectors. If it is omitted, the benchmark uses
synthetic vectors. `--index_path` is accepted for CLI compatibility, but the
current P5 benchmark uses exact in-memory base search plus the P5 dynamic layer.

## Output

The output is CSV. Columns:

```text
scenario,read_ratio,write_ratio,read_qps,write_qps,insert_throughput,
avg_latency,p50_latency,p95_latency,p99_latency,recall_at_10,
wal_append_avg_us,memtable_insert_avg_us,flush_duration_ms,sstable_count,
delta_record_count,memory_usage_mb,disk_usage_mb,recovery_time_ms
```

## Current Baseline

The static baseline in this first P5 benchmark is exact in-memory base search.
This gives a stable correctness baseline for checking whether newly inserted
delta records can be found and merged into the result set.

The benchmark does not yet run a packed disk graph baseline from `--index_path`.
That should be added in P6 if we want direct disk-graph-vs-dynamic comparisons
inside this benchmark binary.

## Recall

`recall_at_10` is computed against an exact base + exact delta merge in the same
run. Because the current benchmark uses exact in-memory base search and exact
delta scan, recall should normally be `1.0`. If a future run switches base search
to approximate disk graph search, this column will show the effect of approximate
base search plus dynamic result merge.

## Flush And Compaction

`flush_duration_ms` includes the final explicit flush when `--enable_flush=1`.
If `--enable_compaction=1`, it also includes a measured manual compaction pass
when at least two SSTables exist.

The current CompactionJob intentionally does not delete old SSTable files or
update the manifest. That matches the P5 Compaction task scope.

## Recovery

`recovery_time_ms` measures closing the manager, reopening it on the same
dynamic directory, loading manifest/SSTables, and replaying the current WAL.

## Current Issues

- `wal_append_avg_us` is reserved in the CSV but currently reported as `0`
  because `DynamicWriteManager::insert` does not expose separate WAL and
  MemTable timing.
- The current benchmark uses exact in-memory base search rather than the packed
  disk graph search path.
- Compaction is a manual measured pass and does not replace the manifest.

## P6 Directions

- Add timing hooks around WAL append and MemTable insert.
- Run packed disk graph baseline and dynamic merge in the same benchmark.
- Move flush and compaction off the foreground path.
- Add a small ANN index for the delta layer.
- Add WAL checkpoint/truncate to reduce recovery time.
