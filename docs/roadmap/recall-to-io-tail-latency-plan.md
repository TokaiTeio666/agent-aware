# Recall 达标后转向 I/O 放大与尾延迟优化计划

## 0. 背景与目标

当前项目已经具备 SIFT1M SSD packed graph 查询、混合读写、动态 delta、读路径 breakdown、cache / prefetch / io_uring 等基础能力。下一阶段的目标不再是单纯继续拉高 Recall，而是在固定 Recall 下限的前提下，系统性降低：

- 每次查询的 SSD 读次数；
- 每次查询读取字节数；
- 无效预取与重复 page 读取；
- base / delta / merge 阶段候选集放大；
- P95 / P99 / P999 尾延迟。

核心推进顺序：

```text
先量化每阶段 I/O 和候选数
再做前置过滤和候选裁剪
然后改数据布局和缓存
最后做自适应预算与尾延迟治理
```

本计划默认优化对象是：

```text
SIFT1M SSD packed Vamana 主索引
+ DynamicWriteManager base/delta 合并路径
+ bench_mixed_rw mixed read/write benchmark
```

除非实验明确证明图质量不足，否则本阶段不优先重建大索引、不引入新 ANN 算法。

---

## 1. Recall 护栏

所有性能优化必须先定义 Recall 护栏，避免用结果质量换延迟。

建议护栏：

| 项目 | 建议值 |
| --- | ---: |
| 主指标 | `Recall@10` |
| 最低门槛 | `>= 0.95` |
| 与当前最佳 baseline 的允许回退 | `<= 0.005` |
| 统计方式 | 独立 recall evidence 实验，不混入吞吐压测 |
| 主路径吞吐实验 | `--recall_sample_rate 0` |

性能实验只在满足 Recall 护栏的参数组合里排序。

推荐报告口径：

```text
在 Recall@10 >= 0.95 的配置集合中，选择 reads/query、bytes/query、P99 最低的配置。
```

---

## 2. 当前已有指标与缺口

### 2.1 已有可直接使用的指标

当前代码已经具备较多读路径指标，不需要重做一套统计框架。

| 层级 | 已有字段 | 用途 |
| --- | --- | --- |
| benchmark | `read_qps`、`read_p50/p95/p99_ms` | 查询吞吐和尾延迟 |
| benchmark | `graph_reads_per_read` | 每次 read 提交的 graph page 读取数 |
| benchmark | `cache_hit_rate` | page cache 命中率 |
| benchmark | `page_read_wait_ms_per_read` | 每次 read 的 page wait 总耗时 |
| benchmark | `search_mutex_wait_ms_per_read` | 搜索锁等待 |
| read breakdown | `dynamic_snapshot_*` | 动态快照开销 |
| read breakdown | `base_search_*` | base SSD graph 搜索耗时 |
| read breakdown | `latest_record_lookup_*` | base id 最新版本检查耗时 |
| read breakdown | `delta_search_*` | delta search 耗时 |
| read breakdown | `merge_*` | base/delta merge 耗时 |
| read breakdown | `exact_recall_*` | exact recall 验证耗时 |
| graph stats | `expanded`、`visited` | 图搜索扩展和访问节点数 |
| graph stats | `page_requests_before_dedup`、`page_requests_after_dedup` | page 去重效果 |
| graph stats | `page_cache_hits/misses/evictions/promotions` | 缓存行为 |
| graph stats | `prefetch_*` | 预取提交、命中、浪费、跳过原因 |
| graph stats | `batch_count`、`batch_expanded`、`max_batch_size` | beam batch 生效情况 |
| graph stats | `rerank_reads` | rerank 引入的额外读取 |
| graph stats | `pq_filter_reject_count/accept_count` | PQ filter 裁剪效果 |

对应主要代码入口：

| 文件 | 关注点 |
| --- | --- |
| `include/graph/disk_graph_index.h` | `DiskGraphSearchConfig`、`DiskGraphSearchStats` |
| `src/graph/disk_graph_index.cpp` | packed graph 搜索、batch、early stop、rerank |
| `include/core/query_page_session.h` | query 级 page 生命周期 |
| `src/core/query_page_session.cpp` | page dedup、cache、prefetch、batch load |
| `include/core/io_stats.h` | `P4IoStats` |
| `include/engine/storage_engine.h` | `EngineSearchStats`、read timing |
| `src/engine/storage_engine.cpp` | base / latest / delta / merge 分段 |
| `src/mixed_rw_benchmark.cpp` | benchmark 聚合与 JSON 输出 |

### 2.2 需要补齐的指标

下一步重点是补“候选数”和“字节数”，让 I/O 放大能被解释。

建议新增字段：

| 字段 | 层级 | 说明 |
| --- | --- | --- |
| `bytes_read_per_read` | benchmark | `submitted_reads * page_size / read_ops` |
| `demand_bytes_read_per_read` | benchmark | demand read 读取字节 |
| `prefetch_bytes_read_per_read` | benchmark | prefetch read 读取字节 |
| `wasted_prefetch_bytes_per_read` | benchmark | `prefetch_wasted_pages * page_size / read_ops` |
| `unique_pages_per_read` | graph | query 内实际 unique page 数 |
| `duplicate_pages_per_read` | graph | batch / prefetch 去重前后的差值 |
| `candidate_pushes_per_read` | graph | 邻居进入候选队列次数 |
| `candidate_pops_per_read` | graph | 从候选队列弹出次数 |
| `base_candidates_before_filter` | engine | base graph 返回或中间候选数 |
| `base_candidates_after_filter` | engine | tombstone / latest 过滤后的 base 候选数 |
| `delta_candidates_scanned` | engine | delta search 扫描候选数 |
| `delta_candidates_returned` | engine | delta search 返回候选数 |
| `merge_input_candidates` | engine | merge 前 base + delta 候选总数 |
| `merge_output_candidates` | engine | merge 后输出候选数 |
| `rerank_candidates` | graph | rerank 实际候选数 |
| `deadline_hit_count` | benchmark | 自适应预算阶段使用 |
| `fallback_partial_count` | benchmark | deadline 下 partial return 次数 |

JSON 建议结构：

```json
{
  "io_amplification": {
    "page_size": 4096,
    "submitted_reads_per_read": 318.0,
    "bytes_read_per_read": 1302528.0,
    "demand_bytes_read_per_read": 1048576.0,
    "prefetch_bytes_read_per_read": 253952.0,
    "wasted_prefetch_bytes_per_read": 49152.0,
    "unique_pages_per_read": 280.0,
    "duplicate_pages_per_read": 38.0
  },
  "candidate_amplification": {
    "expanded_per_read": 128.0,
    "visited_per_read": 820.0,
    "candidate_pushes_per_read": 790.0,
    "candidate_pops_per_read": 150.0,
    "base_candidates_before_filter": 100.0,
    "base_candidates_after_filter": 95.0,
    "delta_candidates_scanned": 4000.0,
    "delta_candidates_returned": 10.0,
    "merge_input_candidates": 105.0,
    "merge_output_candidates": 10.0
  }
}
```

---

## 3. Phase 0：固定可对比基线

### 目标

先建立可复现 baseline，后续所有优化都只和这组结果比较。

### 任务

1. 记录环境：
   - git commit；
   - dirty status；
   - CPU / memory / SSD；
   - WSL ext4 或原生 Linux；
   - index path；
   - data path；
   - build type；
   - liburing 是否可用。

2. 编译 Release：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

3. 跑纯读 baseline：

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 0 \
  --recall_sample_rate 0 \
  --enable_compaction 0 \
  --topk 10 \
  --search_width 256 \
  --beam_width 16 \
  --io_mode pread \
  --cache_policy graph-aware-2q \
  --output build/io_tail_baseline_readonly_t4.json
```

4. 跑 mixed no recall baseline：

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 1 \
  --recall_sample_rate 0 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --topk 10 \
  --search_width 256 \
  --beam_width 16 \
  --io_mode pread \
  --cache_policy graph-aware-2q \
  --output build/io_tail_baseline_mixed_no_recall.json
```

5. 跑 recall evidence：

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --duration_sec 20 \
  --read_threads 1 \
  --write_threads 1 \
  --recall_sample_rate 0.05 \
  --recall_max_samples_per_sec 1 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --topk 10 \
  --search_width 256 \
  --beam_width 16 \
  --io_mode pread \
  --cache_policy graph-aware-2q \
  --output build/io_tail_baseline_recall_evidence.json
```

### 验收标准

| 项目 | 标准 |
| --- | --- |
| JSON 完整 | 三个结果文件均生成 |
| Recall 护栏 | evidence 中 `Recall@10 >= 0.95` |
| 主路径无污染 | pure read / mixed no recall 中 `recall_samples = 0` |
| 可解释 | `read_breakdown`、`graph_reads_per_read`、`cache_hit_rate` 均存在 |

---

## 4. Phase 1：量化每阶段 I/O 和候选数

### 目标

回答三个问题：

```text
每次查询到底读了多少 page / bytes？
这些读取发生在哪个阶段？
候选集在哪个阶段被放大，在哪个阶段被裁掉？
```

### 代码任务

| 优先级 | 任务 | 修改入口 |
| --- | --- | --- |
| P0 | 输出 bytes/read | `src/mixed_rw_benchmark.cpp` |
| P0 | 聚合 `expanded/visited/batch/rerank` per read | `append_read_timing`、`finalize_result` |
| P0 | 输出 demand / prefetch / wasted bytes | `DiskGraphSearchStats` + JSON |
| P1 | 增加 candidate push/pop 统计 | `src/graph/disk_graph_index.cpp` |
| P1 | 增加 base/delta/merge candidate count | `EngineSearchStats`、`src/engine/storage_engine.cpp` |
| P1 | 输出 top slow query 样本 | `src/mixed_rw_benchmark.cpp` |

### 指标解释

| 指标 | 判断方式 |
| --- | --- |
| `bytes_read_per_read` 高 | page size、search_width、cache miss 或 prefetch 过度 |
| `unique_pages_per_read` 高 | 搜索路径分散，布局或图质量问题 |
| `duplicate_pages_per_read` 高 | batch page 去重价值大，或候选集中同页节点多 |
| `wasted_prefetch_bytes_per_read` 高 | prefetch 太激进，应降 `prefetch_width/depth` |
| `candidate_pushes_per_read / expanded_per_read` 高 | 邻居扩张过宽，需要过滤或 early stop |
| `delta_candidates_scanned` 高 | delta brute force 到达瓶颈 |
| `rerank_reads` 高 | rerank 候选过多，或 full vector 读取后置成本高 |

### 实验矩阵

第一轮只变一个参数，建立曲线。

| 实验轴 | 取值 |
| --- | --- |
| `search_width` | `128 / 192 / 256 / 350 / 512` |
| `beam_width` | `8 / 16 / 32` |
| `io_mode` | `pread / io_uring` |
| `cache_policy` | `none / lru / 2q / graph-aware-2q` |

第一轮建议固定：

```text
topk = 10
entry_count = 64
query_count = 1000
duration_sec = 30
read_threads = 4
write_threads = 0
recall_sample_rate = 0
```

### 输出表

| run_id | Recall@10 | QPS | P95 | P99 | reads/q | bytes/q | unique_pages/q | cache_hit | wasted_prefetch/q |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| SW128-B16 | | | | | | | | | |
| SW192-B16 | | | | | | | | | |
| SW256-B16 | | | | | | | | | |
| SW350-B16 | | | | | | | | | |

### 验收标准

进入下一阶段前必须能明确说出：

```text
当前 P99 主要来自 base_search、page_read_wait、delta_search、merge、还是 search_mutex_wait。
当前 I/O 放大主要来自 search_width、cache miss、prefetch waste、rerank，还是 delta。
```

---

## 5. Phase 2：前置过滤和候选裁剪

### 目标

在 Recall 护栏下减少无效扩展、无效 page 读取和 merge 输入规模。

优化顺序：

```text
先低风险裁剪
再启用近似过滤
最后处理 delta 增长
```

### 5.1 低风险裁剪

| 任务 | 做法 | 验收 |
| --- | --- | --- |
| 收紧 `search_width` | 从 baseline 向下扫 `256 -> 192 -> 128` | Recall 不低于护栏 |
| 调大有效 `beam_width` | 对比 `8/16/32` | P99 降低且 reads/q 不升 |
| 开启 early stop | 设置 `early_stop`、`adaptive_early_stop`、`min_expansions` | expanded/q 下降 |
| 保证 page dedup | `page_dedup = true` | duplicate pages 被消除 |
| 保证 same page reuse | `same_page_reuse = true` | `same_page_node_reuse` 上升 |

建议先找最小 Recall 达标点：

```text
search_width = 128 / 192 / 256 / 350
beam_width = 16 / 32
early_stop = off / on
adaptive_early_stop = off / on
```

### 5.2 PQ / ADC 过滤

当前 `DiskGraphSearchConfig` 已有：

```text
pq_model
adc_enable
rerank_topk
pq_filter_reject_count
pq_filter_accept_count
```

可按以下顺序推进：

| 步骤 | 动作 | 指标 |
| --- | --- | --- |
| 1 | 只启用 ADC 距离估计，不过滤 | `adc_table_build_us` |
| 2 | 对明显劣质候选做 reject | `pq_filter_reject_count` |
| 3 | 限制 full vector rerank 候选 | `rerank_reads` |
| 4 | 扫 `rerank_topk = 20 / 50 / 100` | Recall / P99 |

验收标准：

```text
pq_filter_reject_count 上升
rerank_reads 下降
Recall@10 不明显下降
P99 或 bytes/q 下降
```

### 5.3 动态可见性前置过滤

混合读写路径里，base 结果需要和 latest record / tombstone 合并。当前优先补以下统计和过滤：

| 任务 | 说明 |
| --- | --- |
| base id latest lookup 批量化 | 对 graph top-k 或 rerank candidates 批量查 latest |
| tombstone bitmap | delete base id 时先用 bitmap 排除 |
| updated base id side index | updated id 命中时直接替换为新版本 |
| visible delta bitmap | delta search 前按 read_sequence 快速过滤不可见记录 |

注意：

```text
前置过滤必须保留 read_sequence 语义，不能让未来写入污染当前 read。
```

### 5.4 Delta 候选裁剪

当 `delta_candidates_scanned` 随 delta size 线性上升时，按阈值切换策略。

| delta 规模 | 策略 |
| ---: | --- |
| `< 10k` | brute force delta scan 可接受 |
| `10k - 50k` | 增加 bitmap / sequence 过滤，限制返回候选 |
| `> 50k` | 建 delta memory graph 或分层 delta index |
| `> 100k` | 触发 rebuild / compact-to-base 评估 |

实验矩阵：

| `preload_delta_count` | 观察 |
| ---: | --- |
| `1k` | delta scan 基线 |
| `10k` | 是否仍可接受 |
| `50k` | 是否开始拖 P95/P99 |
| `100k` | 是否必须引入 delta index |

### Phase 2 验收标准

| 指标 | 目标 |
| --- | ---: |
| Recall@10 | `>= 0.95` |
| reads/query | 相比 Phase 0 降低 `>= 20%` |
| bytes/query | 相比 Phase 0 降低 `>= 20%` |
| expanded/query | 明显下降 |
| P99 | 不高于 Phase 0，优先下降 |

---

## 6. Phase 3：数据布局和缓存

### 目标

当候选裁剪已经做到可解释后，再通过布局和缓存减少随机读、提高同页复用和热点命中率。

### 6.1 数据布局优化

当前 `DiskGraphBuildConfig` 已包含：

```text
page_size
packing_strategy
coaccess_sessions
coaccess_trace_length
hotpath_queries
hotpath_search_width
hotpath_entry_count
neighbor_pq_code_bytes
```

建议按以下顺序推进。

| 优先级 | 任务 | 目的 |
| --- | --- | --- |
| P0 | 统计 page 内有效访问密度 | 判断一页读回来用了几个节点 |
| P0 | 输出 `nodes_used_per_loaded_page` | 衡量同页复用 |
| P1 | 对比 `one-node` 与 co-access packing | 减少 unique pages/q |
| P1 | 将 entry / hub 附近节点聚集 | 提高入口阶段缓存命中 |
| P2 | PQ code 与 full vector 分层读取 | 粗搜只读压缩表示，rerank 才读 full vector |
| P2 | 评估 page size | 4KB / 8KB / 16KB 的读放大和命中率 |

布局实验表：

| packing | page_size | nodes/page | Recall@10 | reads/q | bytes/q | same_page_reuse/q | P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| one-node | 4096 | | | | | | |
| coaccess | 4096 | | | | | | |
| hotpath | 4096 | | | | | | |

判断标准：

```text
如果 unique_pages/q 下降但 bytes/q 上升，需要比较 P99 是否下降。
如果 same_page_reuse/q 没有上升，说明 packing 没有命中真实搜索路径。
```

### 6.2 缓存优化

当前已支持 `graph-aware-2q`、page cache stats、hub cache stats、pin/unpin 等能力。下一阶段不急着换缓存算法，先做对照。

实验矩阵：

| cache_policy | cache_pages | hot_degree_threshold |
| --- | ---: | ---: |
| none | 0 | 0 |
| lru | 4096 | 0 |
| 2q | 4096 | 0 |
| graph-aware-2q | 4096 | 8 |
| graph-aware-2q | 8192 | 8 |
| graph-aware-2q | 16384 | 8 |
| graph-aware-2q | 8192 | 16 |
| graph-aware-2q | 8192 | 32 |

需要输出：

| 指标 | 用途 |
| --- | --- |
| `cache_hit_rate` | 总体命中 |
| `page_cache_hub_hits / page_cache_hub_requests` | hub 命中 |
| `page_cache_evictions` | 淘汰压力 |
| `page_cache_pinned_eviction_skips` | pin 是否影响淘汰 |
| `same_page_node_reuse` | query 内复用 |
| `distance_direct_calls` | 零拷贝距离路径覆盖率 |

### 6.3 预取治理

预取不是越多越好。本阶段目标是控制有效预取率。

| 指标 | 处理 |
| --- | --- |
| `prefetch_useful_pages / prefetch_submitted_pages` 低 | 降低 `prefetch_width` |
| `prefetch_wasted_pages` 高 | 降低 `prefetch_depth` 或启用更严格 page reuse 判断 |
| `demand_priority_blocks_prefetch` 高 | demand reserve 太紧，或 prefetch 太激进 |
| `prefetch_skip_cached` 高 | cache 已覆盖，减少预取 |
| `prefetch_pending_hit` 高 | 预取方向正确，但完成不够早 |

预取参数：

| 参数 | 取值 |
| --- | --- |
| `prefetch_policy` | `none / frontier-next-hop` |
| `prefetch_width` | `0 / 4 / 8 / 16` |
| `prefetch_depth` | `0 / 1` |
| `prefetch_min_candidates_per_page` | `1 / 2 / 4` |

### Phase 3 验收标准

| 指标 | 目标 |
| --- | ---: |
| cache hit rate | 比 Phase 0 上升 |
| useful prefetch ratio | `>= 0.50`，否则默认关闭或收紧 |
| unique pages/query | 比 Phase 2 继续下降 |
| bytes/query | 不因 page size 或 prefetch 明显反弹 |
| P99 | 比 Phase 2 下降 |

---

## 7. Phase 4：自适应预算与尾延迟治理

### 目标

固定一个全局 `search_width` 容易让简单 query 过度搜索，让困难 query 仍然拖尾。自适应预算的目标是：

```text
简单 query 少读、少扩展、快返回；
困难 query 在 deadline 内逐步加预算；
超出 deadline 时可解释地返回 partial 或 fallback。
```

### 7.1 自适应搜索预算

建议实现三档预算：

| 档位 | search_width | beam_width | 使用场景 |
| --- | ---: | ---: | --- |
| fast | 128 | 16 | 默认起步 |
| normal | 192 / 256 | 16 / 32 | fast 置信度不足 |
| deep | 350 | 32 | deadline 内最后补救 |

置信度信号：

| 信号 | 含义 |
| --- | --- |
| top-k worst distance 连续不改善 | 可 early stop |
| candidate frontier best 已明显差于 top-k worst | 可 early stop |
| `stagnant_expansions` 超阈值 | 可 early stop |
| cache miss streak 过长 | 降低预取或停止深搜 |
| page read wait 已接近 deadline | 返回当前 best |
| delta size 很小 | 不需要扩大 delta search |

需要新增输出：

| 字段 | 说明 |
| --- | --- |
| `budget_tier` | `fast/normal/deep` |
| `budget_escalations` | query 内升档次数 |
| `early_stop_reason` | `frontier_gap/stagnation/deadline/max_width` |
| `deadline_hit_count` | 命中 deadline 的次数 |
| `partial_return_count` | deadline 下返回当前 best 的次数 |

### 7.2 Tail latency 治理

按优先级处理以下问题。

| 优先级 | 问题 | 动作 |
| --- | --- | --- |
| P0 | 锁导致串行 | 观察 `search_mutex_wait_ms_per_read`，必要时拆分 `search_mutex_` 保护范围 |
| P0 | prefetch 抢占 demand I/O | 保留 demand reserve，限制 prefetch budget |
| P0 | 大 query 拖慢普通 query | 增加 per-query deadline 和 max page reads |
| P1 | cache 全局锁竞争 | 分片 cache 锁或读写锁 |
| P1 | 少数 query miss 爆炸 | 输出 top slow query trace，分析路径 |
| P2 | mixed RW compaction 干扰 | compaction 限速或分时执行 |

注意：

```text
当前多 reader 可能每个线程创建独立 PackedGraphEngine，但 PackedDiskGraphIndex 内仍有 search_mutex_。
必须用 search_mutex_wait 指标确认是否构成真实尾延迟瓶颈，再决定是否拆锁。
```

### 7.3 Deadline 策略

建议先只做软 deadline，不改变正确性语义。

| 配置 | 建议 |
| --- | --- |
| `query_deadline_ms` | 从当前 P95 的 1.2 倍开始 |
| `min_expansions_before_deadline` | `64` 或 `128` |
| `max_reads_before_deadline` | 从 baseline reads/q 的 P95 开始 |
| partial return | 只在已有 top-k 时允许 |
| recall evidence | 单独验证 deadline 对 Recall 的影响 |

### Phase 4 验收标准

| 指标 | 目标 |
| --- | ---: |
| P99 | 相比 Phase 3 下降 |
| P999 | 明显收敛 |
| Recall@10 | 仍满足护栏 |
| deadline hit | 可解释且比例可控 |
| partial return | 不成为主要路径 |
| read QPS | 不低于 Phase 3 |

---

## 8. 推荐执行排期

### Week 1：量化和诊断

| Day | 任务 | 交付物 |
| --- | --- | --- |
| D1 | 补 bytes/q、candidate count、JSON 输出 | 一版可编译 benchmark |
| D2 | 跑 pure read / mixed no recall / recall evidence | baseline JSON |
| D3 | search_width / beam_width 小矩阵 | reads/q、bytes/q、P99 曲线 |
| D4 | 输出诊断结论 | 当前最大 I/O 放大来源 |
| D5 | 根据诊断选 Phase 2 优化点 | 下一轮实验配置 |

### Week 2：裁剪和缓存

| Day | 任务 | 交付物 |
| --- | --- | --- |
| D6-D7 | early stop / beam / PQ filter 对照 | candidate 裁剪结果 |
| D8 | delta 规模压力测试 | delta 阈值结论 |
| D9 | cache policy / cache_pages 对照 | cache 曲线 |
| D10 | prefetch width/depth 对照 | useful/wasted prefetch 曲线 |

### Week 3：布局和尾延迟

| Day | 任务 | 交付物 |
| --- | --- | --- |
| D11-D12 | co-access / hotpath packing 小规模验证 | unique pages/q 对照 |
| D13 | 自适应 budget prototype | fast/normal/deep 统计 |
| D14 | deadline / partial return 实验 | P99/P999 对照 |
| D15 | 整理最终报告 | 推荐参数和下一阶段任务 |

---

## 9. 首轮最小 PR 范围

第一轮不要直接改搜索策略，先把量化能力补齐。

### 修改范围

| 文件 | 修改 |
| --- | --- |
| `include/graph/disk_graph_index.h` | 增加候选数、bytes 相关 stats |
| `src/graph/disk_graph_index.cpp` | 记录 push/pop、unique page、rerank candidates |
| `include/engine/storage_engine.h` | 增加 base/delta/merge candidate stats |
| `src/engine/storage_engine.cpp` | 记录 base/delta/merge 输入输出数量 |
| `src/mixed_rw_benchmark.cpp` | 聚合 per-read 指标并输出 JSON/CSV |

### 不做的事

```text
不改 packed graph 文件格式
不重建 SIFT1M 主索引
不引入新 ANN 算法
不把 recall exact 回算混入主路径吞吐
不同时改布局、缓存和搜索策略
```

### 首轮验收

```text
cmake --build build -j$(nproc)
./build/bench_mixed_rw ... --recall_sample_rate 0 --output build/io_tail_metrics_smoke.json
```

JSON 中至少包含：

```text
read_breakdown
io_amplification
candidate_amplification
benchmark_config
```

---

## 10. 决策树

完成 Phase 1 后按下面判断下一步。

```text
如果 base_search 占比最高：
  先调 search_width / beam_width / early_stop / PQ filter

如果 page_read_wait 占比最高：
  先看 cache_hit_rate、bytes/q、prefetch_waste，再调 cache 和 prefetch

如果 search_mutex_wait 占比高：
  优先拆锁或改每线程独立 reader/cache 共享策略

如果 delta_search 占比随 delta 线性增长：
  增加 delta 过滤、delta graph 或 rebuild 阈值

如果 merge 占比高：
  缩小 base/delta 候选输入，避免过量进入 merge

如果 Recall 低于护栏：
  回退最近一次裁剪，优先修图质量或提高 search_width，不继续压 I/O

如果 prefetch_wasted_bytes 高：
  降 prefetch_width/depth，或关闭预取作为默认配置
```

---

## 11. 最终报告模板

最终报告建议包含五张表。

### 表 1：Recall 护栏

| config | Recall@10 | samples | pass |
| --- | ---: | ---: | --- |
| baseline | | | |
| optimized | | | |

### 表 2：I/O 放大

| config | reads/q | bytes/q | unique pages/q | duplicate pages/q | wasted prefetch bytes/q |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline | | | | | |
| Phase 2 | | | | | |
| Phase 3 | | | | | |
| Phase 4 | | | | | |

### 表 3：候选放大

| config | expanded/q | visited/q | pushes/q | pops/q | rerank candidates | merge input |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | | | | | | |
| optimized | | | | | | |

### 表 4：延迟

| config | QPS | P50 | P95 | P99 | P999 |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline | | | | | |
| optimized | | | | | |

### 表 5：阶段耗时

| config | base_search P95 | page_wait P95 | delta P95 | merge P95 | mutex wait P95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline | | | | | |
| optimized | | | | | |

---

## 12. 成功标准

本阶段完成时，应能给出一个明确结论：

```text
在 Recall@10 >= 0.95 的前提下，
通过候选裁剪、缓存/预取治理、布局或自适应预算，
将 reads/query、bytes/query 和 P99 延迟分别降低到可解释的新水平。
```

建议目标：

| 指标 | 目标 |
| --- | ---: |
| Recall@10 | `>= 0.95` |
| reads/query | 相比 Phase 0 降低 `>= 30%` |
| bytes/query | 相比 Phase 0 降低 `>= 30%` |
| P99 | 相比 Phase 0 降低 `>= 25%` |
| useful prefetch ratio | `>= 0.50`，否则默认收紧或关闭 |
| cache hit rate | 不低于 Phase 0 |
| dirty dynamic correctness | insert/update/delete 语义不回退 |

如果只能达成部分目标，优先级如下：

```text
Recall 护栏 > 正确性 > P99 > bytes/query > QPS > 平均延迟
```

