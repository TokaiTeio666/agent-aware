# SIFT1M 混合读写下一步优化计划

## 0. 当前执行状态（2026-06-21）

本计划中的 P0/P1 核心任务已经完成，并形成当前可展示版本：

| 任务 | 状态 | 证据 |
| --- | --- | --- |
| 纯读 baseline | 已完成 | `build/sift1m_readonly_t1.json` ~ `build/sift1m_readonly_t8.json` |
| 关闭 Recall exact 的混合读写主路径 | 已完成 | `build/sift1m_mixed_rw_no_recall_no_compaction_immutable_view.json` |
| 后台 compaction 对照 | 已完成 | `build/sift1m_mixed_rw_no_recall_compaction_immutable_view.json` |
| 读路径分段指标 | 已完成 | JSON `read_breakdown` 字段 |
| 动态 Recall 单独展示实验 | 已完成 | `build/sift1m_dynamic_recall_immutable_view.json` |
| reader 是否被锁串行化 | 已定位并优化 | `latest_record_lookup_p95_ms` 从秒级降到约 `0.07ms` |

关键实现更新：

```text
DynamicWriteManager reader path
  from: global mutex + scan dynamic state
  to: atomic shared_ptr<const DynamicReadView> immutable snapshot
```

关键结果：

| 场景 | read_qps | write_qps | read P95 | latest_record_lookup P95 |
| --- | ---: | ---: | ---: | ---: |
| 优化前 no compaction | 2.03 | 148.68 | 5934 ms | 5872 ms |
| 优化后 no compaction | 23.50 | 217.05 | 167 ms | 0.071 ms |
| 优化前 compaction | 2.42 | 148.38 | 4168 ms | 4105 ms |
| 优化后 compaction | 28.86 | 221.33 | 178 ms | 0.073 ms |

完整报告见：

```text
docs/experiments/sift1m-mixed-rw-immutable-view.md
docs/changelog.md
```

## 1. 当前测试结论

本轮已经在 SIFT1M 已构建好的 SSD packed Vamana 主索引上跑通了混合读写链路，当前系统已经具备以下能力：

```text
SIFT1M SSD 主图查询
+ 并发 reader / writer
+ WAL / MemTable / SSTable 动态写入
+ base / delta merge
+ read_sequence 动态快照
+ 动态 Recall 抽样验证
+ 后台 compaction
+ manifest recovery
```

这说明系统已经从“设计方案”进入“端到端可运行原型”阶段。

但是当前结果也暴露出一个核心问题：

```text
功能闭环已经跑通，但读路径性能证明还不够强。
```

尤其在第一份偏吞吐测试中：

```text
read_threads = 4
read_ops = 35
duration = 30.55s
read_qps = 1.15
```

读吞吐明显偏低，需要进一步拆分读路径耗时，确认瓶颈来自 Base SSD graph search、Delta search、base/delta merge、锁竞争、cache miss、io_uring，还是动态 Recall 精确回算。

---

## 2. 下一阶段目标

下一阶段不优先继续增加新功能，而是优先完成以下目标：

1. **拆分性能瓶颈**
   - 分清楚读慢是 SSD graph 本身慢，还是混合读写框架引入的额外开销。

2. **分离性能实验和正确性实验**
   - 吞吐实验关闭动态 Recall exact 回算。
   - Recall 实验单独开启抽样验证。

3. **补齐读路径分段指标**
   - 输出 base search、delta search、merge、exact recall、lock wait 等耗时。

4. **验证真并发**
   - 确认多个 reader 是否真的并发，而不是被锁串行化。

5. **验证 Delta 增长影响**
   - 观察 delta 从 1k、10k、50k 增长时对 read latency 和 merge cost 的影响。

6. **形成可汇报实验矩阵**
   - 输出可以放进报告的 QPS、P95/P99、Recall、compaction、recovery、delta size 对照结果。

---

## 3. 实施优先级

| 优先级 | 任务 | 目标 |
|---|---|---|
| P0 | 跑纯读 baseline | 判断 Base SSD graph 原始读性能 |
| P0 | 关闭 Recall exact 跑混合读写 | 得到不受验证开销污染的主路径性能 |
| P0 | 增加读路径分段耗时指标 | 找到 read_qps 低的直接原因 |
| P1 | 验证 reader 是否被锁串行化 | 确认每线程独立 engine 是否生效 |
| P1 | 动态 Recall 单独跑展示实验 | 证明 read_sequence + visible set 正确 |
| P1 | compaction on/off 对照 | 证明后台整理对 P99 的影响 |
| P2 | delta 规模压力测试 | 判断 delta scan 何时成为瓶颈 |
| P2 | 调 search_width / beam_width | 找到 SIFT1M 性能和 Recall 平衡点 |
| P3 | Delta memory graph 或 rebuild 阈值 | 解决大 Delta 下查询退化 |

---

## 4. Phase 0：固定当前可运行基线

在继续改代码之前，先保存当前状态，避免后续优化破坏已跑通的混合读写链路。

### 任务

```bash
git status
git add .
git commit -m "baseline: sift1m mixed rw concurrent pipeline"
```

如果当前工作区还有实验结果，也建议保存：

```text
build/sift1m_mixed_rw_concurrent.json
build/sift1m_mixed_rw_concurrent_recall.json
```

### 记录内容

需要记录：

```text
git commit hash
index path
benchmark command
random seed
CPU / memory / SSD 环境
是否 dirty
```

### 验收标准

| 项目 | 标准 |
|---|---|
| 代码状态 | 当前混合读写链路可回退 |
| 结果文件 | 两份 JSON 结果保留 |
| 环境记录 | 命令、索引路径、参数完整保存 |

---

## 5. Phase 1：纯读 Baseline 实验

当前最重要的问题是确认 Base SSD graph 纯读性能。

### 目的

判断 `read_qps = 1.15` 是来自：

```text
SSD graph 搜索本身很慢
还是
混合读写 / delta / merge / compaction 引入的额外开销
```

### 实验设置

关闭 writer、delta 写入、Recall exact、compaction，只测 SSD 主图查询。

建议参数：

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --duration_sec 30 \
  --read_threads 1 \
  --write_threads 0 \
  --recall_sample_rate 0 \
  --enable_compaction 0 \
  --topk 10 \
  --search_width 350 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_readonly_t1.json
```

然后依次测试：

```text
read_threads = 1 / 2 / 4 / 8
```

输出文件：

```text
build/sift1m_readonly_t1.json
build/sift1m_readonly_t2.json
build/sift1m_readonly_t4.json
build/sift1m_readonly_t8.json
```

### 需要观察

| 指标 | 目的 |
|---|---|
| read_qps | 纯读吞吐 |
| read_p50/p95/p99 | 纯读尾延迟 |
| cache_hit_rate | 是否大量 miss |
| graph_reads_per_query | SSD 读放大 |
| base_search_ms | Base 搜索耗时 |
| search_mutex_wait_ms | 是否锁等待 |
| page_read_ms | I/O 耗时 |

### 判断标准

| 结果 | 解释 |
|---|---|
| 纯读也很慢 | 瓶颈在 SSD graph search / cache / io_uring / search_width |
| 纯读较快，混合慢 | 瓶颈在 DynamicWriteManager / delta merge / lock / compaction |
| 线程数增加 QPS 不涨 | reader 可能仍被锁串行化，或 I/O 饱和 |
| 线程数增加 P99 爆炸 | cache / SSD / io_uring 队列竞争明显 |

---

## 6. Phase 2：混合读写主路径实验

### 目的

测量不受动态 Recall exact 回算污染的真实混合读写主路径性能。

### 实验设置

关闭 Recall exact：

```text
--recall_sample_rate 0
```

保留 writer、WAL、MemTable、SSTable、base/delta merge。

建议命令：

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
  --enable_compaction 0 \
  --topk 10 \
  --search_width 350 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_mixed_rw_no_recall_no_compaction.json
```

再开启 compaction：

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
  --search_width 350 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_mixed_rw_no_recall_compaction.json
```

### 需要对比

| 对照 | 目的 |
|---|---|
| no compaction | 主路径基础性能 |
| compaction on | 后台整理对读写延迟影响 |
| pure read | 判断写入是否拖慢查询 |

### 验收标准

| 项目 | 标准 |
|---|---|
| 读写均有操作 | read_ops 和 write_ops 都大于 0 |
| 写入可持续 | write_qps 稳定，write P95 不异常 |
| 读路径可解释 | read_qps 低时能通过分段耗时定位原因 |
| compaction 不崩溃 | 开启后台 compaction 后系统稳定运行 |

---

## 7. Phase 3：读路径分段指标补齐

当前结果最大问题是 read_qps 低，但缺少定位依据。下一步必须在 benchmark JSON 中增加读路径分段耗时。

### 新增指标

建议每个 read 统计：

```text
base_search_ms
delta_search_ms
merge_ms
latest_record_lookup_ms
dynamic_snapshot_ms
exact_recall_ms
reader_lock_wait_ms
search_mutex_wait_ms
page_read_ms
cache_lookup_ms
total_read_ms
```

建议聚合输出：

```text
base_search_p50/p95/p99_ms
delta_search_p50/p95/p99_ms
merge_p50/p95/p99_ms
exact_recall_p50/p95/p99_ms
lock_wait_p50/p95/p99_ms
```

### 读路径分解

```text
read_start
  -> acquire snapshot
  -> base graph search
  -> delta search
  -> latest record lookup
  -> base/delta merge
  -> optional exact recall
read_end
```

### JSON 输出示例

```json
{
  "read_breakdown": {
    "base_search_avg_ms": 120.5,
    "base_search_p95_ms": 300.2,
    "delta_search_avg_ms": 2.1,
    "merge_avg_ms": 0.4,
    "exact_recall_avg_ms": 0.0,
    "search_mutex_wait_avg_ms": 0.0,
    "reader_lock_wait_avg_ms": 0.2
  }
}
```

### 验收标准

| 项目 | 标准 |
|---|---|
| 能解释 read_qps | read 总耗时能拆成几个主要部分 |
| 能区分验证开销 | exact_recall_ms 单独统计 |
| 能发现锁竞争 | lock_wait / mutex_wait 有统计 |
| 能支撑报告 | 可以画读路径耗时堆叠图 |

---

## 8. Phase 4：动态 Recall 单独展示实验

动态 Recall 不应该和主路径吞吐实验混在一起。

### 目的

证明：

```text
read_sequence 快照正确
visible set 正确
insert/update/delete 可见性正确
base/delta merge 正确
```

而不是证明吞吐。

### 建议参数

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
  --search_width 350 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_dynamic_recall_evidence.json
```

### 输出指标

```text
recall_at_10
recall_samples
recall_read_sequence_min
recall_read_sequence_max
exact_visible_delta_avg
exact_deleted_count_avg
exact_updated_count_avg
exact_recall_avg_ms
```

### 注意事项

SIFT1M 下 exact 回算会扫描 100 万 base 向量，因此：

```text
动态 Recall 实验的 read latency 不代表主路径查询延迟
```

报告中必须明确说明：

```text
Recall 抽样用于验证动态正确性，吞吐评估使用关闭 exact 回算的实验结果。
```

### 验收标准

| 项目 | 标准 |
|---|---|
| Recall 样本数 | 至少 5 个样本，最好 10 个以上 |
| read_sequence | 输出 min/max |
| visible delta | 输出平均可见 delta 数 |
| Recall | 样本中没有明显错误 |
| exact 耗时 | 单独统计，不混入主路径解释 |

---

## 9. Phase 5：Update/Delete 正确性专项测试

当前写入测试主要是 insert，后续必须单独验证 update/delete 覆盖 Base 结果。

### 测试场景

| 场景 | 期望 |
|---|---|
| insert new id | 新 id 可以出现在最终结果 |
| delete base id | base 返回该 id 时最终被过滤 |
| update base id | 使用新向量替换 base 旧向量 |
| insert 后 delete | 最终不可见 |
| 多次 update | 最大 sequence_id 生效 |

### 建议新增测试

```bash
./build/bench_dynamic_correctness \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --case insert_delete_update \
  --topk 10 \
  --output build/sift1m_dynamic_correctness.json
```

### 验收标准

| 项目 | 标准 |
|---|---|
| delete base id | 不会从最终结果复活 |
| update base id | exact truth 和 ANN merge 都使用新版本 |
| tombstone | compaction 后仍然生效 |
| sequence_id | 最新版本优先 |

---

## 10. Phase 6：Delta 规模压力测试

当前 delta 只有 4k 左右，距离 SIFT1M 的 5% 阈值还有很大距离。

需要测试：

```text
delta = 1k / 10k / 50k / 100k
```

### 目的

观察 delta 增长对查询的影响：

```text
delta_search_ms 是否线性增长
merge_ms 是否增长
read_p95/p99 是否恶化
write_qps 是否下降
```

### 实验方式

先预插入 delta，再跑查询：

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --preload_delta_count 10000 \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 0 \
  --recall_sample_rate 0 \
  --enable_compaction 0 \
  --topk 10 \
  --search_width 350 \
  --beam_width 16 \
  --output build/sift1m_delta_10k_read.json
```

依次测试：

```text
preload_delta_count = 1000
preload_delta_count = 10000
preload_delta_count = 50000
preload_delta_count = 100000
```

### 判断标准

| 结果 | 动作 |
|---|---|
| delta < 50k 查询可接受 | brute force delta scan 可以作为 MVP |
| delta 50k 后延迟明显上升 | 需要 delta memory graph |
| delta 100k 后不可接受 | 需要 rebuild 或分层 delta index |

---

## 11. Phase 7：Base 搜索参数调优

当前使用：

```text
search_width = 350
beam_width = 16
```

需要确认 SIFT1M 下它们是否过重。

### 参数矩阵

| search_width | beam_width |
|---:|---:|
| 128 | 8 |
| 256 | 8 |
| 256 | 16 |
| 350 | 16 |
| 512 | 16 |
| 512 | 32 |

### 观察指标

```text
read_qps
read_p95/p99
recall_at_10
graph_reads_per_query
cache_hit_rate
```

### 推荐目标

找到一组比赛展示参数：

```text
Recall 可以接受
read latency 不过高
graph_reads_per_query 可解释
cache_hit_rate 较高
```

第一版建议从：

```text
search_width = 256
beam_width = 16
```

开始对比当前：

```text
search_width = 350
beam_width = 16
```

---

## 12. Phase 8：报告实验矩阵

最终报告建议至少给出以下结果。

### 表 1：纯读扩展性

| read_threads | read_qps | P50 | P95 | P99 |
|---:|---:|---:|---:|---:|
| 1 | | | | |
| 2 | | | | |
| 4 | | | | |
| 8 | | | | |

### 表 2：混合读写性能

| read_threads | write_threads | read_qps | write_qps | read P95 | write P95 |
|---:|---:|---:|---:|---:|---:|
| 4 | 1 | | | | |
| 4 | 2 | | | | |
| 8 | 1 | | | | |

### 表 3：compaction 影响

| compaction | read_qps | read P99 | write P99 | compaction_count |
|---|---:|---:|---:|---:|
| off | | | | |
| on | | | | |

### 表 4：动态 Recall 证据

| recall_samples | Recall@10 | read_sequence range | visible_delta_avg | exact_recall_avg_ms |
|---:|---:|---:|---:|---:|
| | | | | |

### 表 5：delta 增长影响

| delta_count | read_qps | delta_search_avg_ms | merge_avg_ms | read P95 |
|---:|---:|---:|---:|---:|
| 1k | | | | |
| 10k | | | | |
| 50k | | | | |
| 100k | | | | |

---

## 13. 里程碑安排

### Milestone 1：性能拆解完成

交付物：

```text
纯读 baseline 结果
混合读写 no recall 结果
读路径 breakdown 指标
```

验收：

```text
能解释当前 read_qps 低的主要原因
```

### Milestone 2：正确性展示完成

交付物：

```text
动态 Recall evidence JSON
update/delete correctness JSON
read_sequence / visible set 统计
```

验收：

```text
能证明 insert/update/delete 动态语义正确
```

### Milestone 3：Delta 压力测试完成

交付物：

```text
delta 1k / 10k / 50k / 100k 对照结果
delta_search_ms 曲线
是否需要 delta graph 的结论
```

验收：

```text
能说明当前 brute force delta scan 的适用范围
```

### Milestone 4：报告图表完成

交付物：

```text
纯读扩展性图
混合读写 QPS 图
P95/P99 延迟图
dynamic recall 证据表
compaction 影响表
delta 增长影响图
```

验收：

```text
结果能支撑“高并发混合读写动态 ANN 原型”的说法
```

---

## 14. 当前阶段最重要的 Codex 任务

建议给 Codex 的第一条任务不是继续加新算法，而是补指标和跑 baseline。

### Codex Prompt

```text
We have already run the SIFT1M mixed read/write benchmark on an existing SSD packed Vamana index, but read_qps is unexpectedly low. Do not add new ANN algorithms yet.

Goal:
Add read-path timing breakdown and benchmark modes so we can separate pure-read performance, mixed read/write performance, and dynamic recall validation overhead.

Tasks:
1. Extend bench_mixed_rw JSON output with timing breakdown:
   - base_search_ms p50/p95/p99
   - delta_search_ms p50/p95/p99
   - merge_ms p50/p95/p99
   - latest_record_lookup_ms p50/p95/p99
   - dynamic_snapshot_ms p50/p95/p99
   - exact_recall_ms p50/p95/p99
   - reader_lock_wait_ms p50/p95/p99 if available
   - search_mutex_wait_ms p50/p95/p99 if available
2. Add a mode or flags to cleanly separate:
   - pure read: write_threads=0, recall_sample_rate=0
   - mixed no recall: write_threads>0, recall_sample_rate=0
   - recall evidence: recall_sample_rate>0 and recall_max_samples_per_sec set
3. Ensure exact recall computation time is recorded separately and not confused with normal read-path latency.
4. Add benchmark config fields to the output JSON:
   - read_threads
   - write_threads
   - duration_sec
   - search_width
   - beam_width
   - recall_sample_rate
   - enable_compaction
   - git_commit
   - dirty_status if available
5. Do not change the packed graph file format.
6. Do not change the existing index path or rebuild SIFT1M.
7. Keep the current Base + Delta + WAL/MemTable/SSTable path working.

After implementation, provide:
- changed files
- build command
- pure-read benchmark command
- mixed-no-recall benchmark command
- recall-evidence benchmark command
- explanation of each new timing field
```

---

## 15. 结论

下一步的核心不是继续堆新功能，而是把当前已经跑通的 SIFT1M 混合读写系统变成“可解释、可复现、可展示”的实验结果。

优先级应该是：

```text
先拆读路径瓶颈
再分离吞吐实验和 Recall 实验
再验证 update/delete 正确性
再做 delta 增长压力
最后考虑 delta graph / rebuild / NAVIS-inspired 优化
```

当前项目已经完成了最难的第一步：SIFT1M 混合读写闭环跑通。接下来要做的是把结果从“能跑”推进到“能证明性能与正确性”。
