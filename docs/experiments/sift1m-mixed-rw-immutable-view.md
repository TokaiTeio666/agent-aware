# SIFT1M Mixed Read/Write Immutable View Report

## 1. 实验目的

本报告用于说明当前项目在 SIFT1M SSD packed Vamana 主索引上的高并发混合读写能力，并解释 `DynamicWriteManager` 读侧 immutable read view 优化带来的性能变化。

核心问题：

```text
上一版 mixed read/write 已跑通功能闭环，但 read_qps 只有约 2，P95 达到秒级。
读路径分段指标显示瓶颈集中在 latest_record_lookup。
```

本版本验证：

1. SSD 主图纯读性能是否正常。
2. 混合写入时 reader 是否还能保持百毫秒级查询。
3. 后台 compaction 是否能与读写并行。
4. 动态 Recall 是否仍满足正确性要求。

## 2. 实验环境与数据

| 项目 | 配置 |
| --- | --- |
| 数据集 | SIFT1M |
| base | 1,000,000 vectors |
| query | 1,000 generated benchmark queries；主路径 SIFT 官方测试为 100 query |
| dim | 128 |
| 主索引 | `indexes/sift1m_vamana_pq100_p4096_sm.idx` |
| 主索引类型 | SSD packed Vamana graph |
| TopK | 10 |
| search_width | 350 |
| beam_width | 16 |
| entry_count | 64 |
| I/O mode | `io_uring` |
| cache policy | `graph-aware-2q` |
| 动态层 | WAL + MemTable + SSTable + manifest + compaction |

## 3. 实现摘要

### 3.1 Base + Delta 查询路径

```text
query
  -> read_sequence = DynamicWriteManager::current_sequence()
  -> SSD packed graph base search
  -> latest_records_for(base_topk_ids, read_sequence)
  -> search_delta_l2_at(query, read_sequence)
  -> merge_base_and_delta_l2()
  -> final topK
```

### 3.2 Immutable Read View

优化前：

```text
reader latest_record_lookup
  -> lock DynamicWriteManager::mutex_
  -> scan memtable/recent/SSTable
  -> release lock
```

优化后：

```text
writer append / recovery / close
  -> update mutable state under mutex_
  -> publish shared_ptr<const DynamicReadView>

reader lookup / snapshot / delta search
  -> atomic_load(read_view_)
  -> filter sequence_id <= read_sequence
  -> no manager mutex
```

该设计让 reader 不再和 writer、flush、compaction 在全局 manager mutex 上排队。

## 4. 运行命令

### 4.1 纯读 baseline

```bash
for t in 1 2 4 8; do
  ./build/bench_mixed_rw \
    --data_path data/sift/sift_base.fvecs \
    --base_count 1000000 \
    --query_count 1000 \
    --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
    --dynamic_dir build/sift1m_readonly_t${t}_dynamic \
    --duration_sec 30 \
    --read_threads "$t" \
    --write_threads 0 \
    --read_ratio 1 \
    --write_ratio 0 \
    --recall_sample_rate 0 \
    --enable_compaction 0 \
    --topk 10 \
    --search_width 350 \
    --entry_count 64 \
    --beam_width 16 \
    --io_mode io_uring \
    --cache_policy graph-aware-2q \
    --output build/sift1m_readonly_t${t}.json
done
```

### 4.2 Mixed no recall, no compaction

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --dynamic_dir build/sift1m_mixed_immutable_no_compaction_dynamic \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 1 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --recall_sample_rate 0 \
  --enable_compaction 0 \
  --topk 10 \
  --search_width 350 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_mixed_rw_no_recall_no_compaction_immutable_view.json
```

### 4.3 Mixed no recall, background compaction

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --dynamic_dir build/sift1m_mixed_immutable_compaction_dynamic \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 1 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --recall_sample_rate 0 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --compaction_interval_ms 1000 \
  --topk 10 \
  --search_width 350 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_mixed_rw_no_recall_compaction_immutable_view.json
```

### 4.4 Dynamic Recall evidence

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --dynamic_dir build/sift1m_dynamic_recall_immutable_view_dynamic \
  --duration_sec 30 \
  --read_threads 1 \
  --write_threads 1 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --recall_sample_rate 1 \
  --recall_max_samples_per_sec 1 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --compaction_interval_ms 1000 \
  --topk 10 \
  --search_width 350 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_dynamic_recall_immutable_view.json
```

## 5. 结果

### 5.1 SIFT1M SSD 主路径

结果文件：

```text
logs/sift_bench/codex_sift1m_once_20260620-022610/result.json
```

| 指标 | 值 |
| --- | ---: |
| Recall@10 | 0.9940 |
| QPS | 2.5989 |
| avg latency | 384.78 ms |
| P95 | 501.005 ms |
| P99 | 507.288 ms |
| resident ratio | 0.199992 |
| I/O mode | io_uring |
| cache policy | graph-aware-2q |

该结果证明主索引满足赛题 Recall >= 85% 与内存 10%-20% 约束。

### 5.2 纯读扩展性

| reader threads | read_ops | read_qps | P50 | P95 | P99 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 383 | 12.76 | 71 ms | 91 ms | 102 ms |
| 2 | 741 | 24.64 | 79 ms | 105 ms | 121 ms |
| 4 | 1281 | 42.61 | 92 ms | 119 ms | 130 ms |
| 8 | 1704 | 56.55 | 136 ms | 178 ms | 212 ms |

结论：SSD base graph 本身不是 1 QPS 级瓶颈，多 reader 独立 engine 有效。

### 5.3 Immutable View 优化前后对比

| 场景 | read_qps | write_qps | read P50 | read P95 | read P99 | latest_record_lookup P95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| old no compaction | 2.03 | 148.68 | 1275 ms | 5934 ms | 7007 ms | 5872 ms |
| new no compaction | 23.50 | 217.05 | 128 ms | 167 ms | 198 ms | 0.071 ms |
| old compaction | 2.42 | 148.38 | 793 ms | 4168 ms | 8876 ms | 4105 ms |
| new compaction | 28.86 | 221.33 | 129 ms | 178 ms | 198 ms | 0.073 ms |

结论：

1. `latest_record_lookup` 是上一版混合读写慢的直接原因。
2. immutable read view 将 `latest_record_lookup_p95_ms` 从秒级降到约 0.07ms。
3. 混合读写 read_qps 提升约 10x，write_qps 也从约 148 提升到 217-221。
4. 后台 compaction 打开后仍能保持百毫秒级 P95。

### 5.4 读路径分段

| 场景 | base_search P95 | latest_lookup P95 | delta_search P95 | merge P95 | page_read_wait P95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| new no compaction | 162 ms | 0.071 ms | 10.38 ms | 0.051 ms | 9.72 ms |
| new compaction | 171 ms | 0.073 ms | 11.46 ms | 0.054 ms | 9.60 ms |
| dynamic recall evidence | 118 ms | 0.073 ms | 8.63 ms | 0.037 ms | 7.31 ms |

说明：

- 主路径耗时现在主要由 SSD base graph search 决定。
- delta linear scan 在 6k-7k delta 下仍可接受，P95 约 8-12ms。
- merge 和 latest lookup 已经不是瓶颈。

### 5.5 动态 Recall evidence

结果文件：

```text
build/sift1m_dynamic_recall_immutable_view.json
```

| 指标 | 值 |
| --- | ---: |
| read_qps | 8.84 |
| write_qps | 250.36 |
| read P95 | 233 ms |
| Recall@10 | 1.0 |
| recall_samples | 27 |
| delta_record_count | 7535 |
| compaction_count | 15 |
| exact_recall_p95_ms | 183 ms |

该实验用于证明动态正确性，不用于主路径吞吐解释。`exact_recall_p95_ms` 是额外验证成本，已经单独记录。

## 6. 赛题能力映射

| 赛题要求 | 当前证据 |
| --- | --- |
| 内存限制 10%-20% | SIFT1M 主路径 resident ratio `0.199992` |
| Recall@10 >= 85% | SSD 主路径 `0.9940`，动态 evidence `1.0` |
| 高并发 Top-K 检索 | 纯读 8 reader `56.55 read_qps` |
| 实时插入/更新路径 | mixed workload 中 writer 持续写入 6k-7k delta records |
| LSM 写优化 | WAL/MemTable/SSTable/flush/compaction/manifest/recovery 均可运行 |
| 混合读写性能评估 | no compaction 与 compaction 两组 JSON 结果 |
| 动态数据接入检索 | base + delta merge，update/delete 覆盖 base id |
| 可解释指标 | read_breakdown 分段耗时与 Recall exact 耗时分离 |

## 7. 风险与下一步

### 当前风险

1. `DynamicReadView` 每次写入发布完整历史副本，大 delta 下写放大和内存会增加。
2. delta search 仍是 linear scan，delta 到 50k/100k 后需要继续压测。
3. recovery 需要扫描 SSTable 构造 read view，恢复时间从亚毫秒上升到 28-34ms。
4. 周期性 rebuild 主图尚未实现。

### 下一步建议

1. 将 read view 从完整 vector copy 改为 immutable chunks 或 append-only shared history。
2. 为 delta 建轻量内存图或 HNSW/Vamana mini-index。
3. 增加 `preload_delta_count` 压测 1k/10k/50k/100k delta。
4. 实现周期性 rebuild，把长期 delta 合并回 packed graph 主索引。
5. 把 recovery read view 构建改为按 manifest 增量加载或懒加载。

## 8. 报告结论

当前版本已经从“混合读写功能闭环可运行”推进到“高并发混合读写主路径可解释、可复现、可展示”：

```text
SIFT1M SSD 主图 + dynamic LSM delta + immutable read view
在 4 reader + 1 writer + 后台 compaction 下：
read_qps = 28.86
write_qps = 221.33
read P95 = 178 ms
Recall@10 evidence = 1.0
```

这组结果可以作为当前阶段的主要赛题展示数据。

