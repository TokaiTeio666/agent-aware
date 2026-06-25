# Version Notes

## 2026-06-25：Plan 文档体系完善

### 目标

统一五份 plan 文档的交叉引用、更新实现状态标记、补充新落地能力（XGBoost 预取排序、early trigger）到主计划。

### 文档变化

| 文档 | 变化 |
| --- | --- |
| `PROJECT_PLAN.md` | 更新实现状态至 2026-06-25；新增 XGBoost 预取排序（§6.7）和 early trigger（§6.8）模块方案；阶段划分增加 P7/P8/P9 并标注完成状态；创新点新增 Immutable Read View、XGBoost ranking、early trigger；风险新增 ML 相关项；新增 §16 专项计划文档索引 |
| `docs/design/XGBOOST_PREFETCH_PLAN.md` | 新增关联文档头；更新落地状态至 2026-06-25；P0-P3 标注已完成、P4 标注进行中 |
| `docs/design/07-prefetch-early-trigger-plan.md` | 新增关联文档头和更新日期；P0 补充已落地代码位置表 |
| `docs/roadmap/recall-to-io-tail-latency-plan.md` | 新增关联文档头和更新日期 |
| `docs/roadmap/defense-gap-closure-plan.md` | 新增关联文档头和更新日期；更新当前背景反映 XGBoost/early trigger 已落地；优先级表增加说明 |
| `docs/README.md` | 新增 recall-to-io-tail-latency-plan 和 defense-gap-closure-plan 索引条目；更新 early-trigger 状态为 P0 已落地 |

## 2026-06-21：Plan/README 文档体系整理

### 目标

把外部阶段计划稿纳入项目 `docs/`，并让 README、总计划、阶段计划、实验报告之间形成可追踪索引。

### 文档变化

| 文档 | 变化 |
| --- | --- |
| `README.md` | 新增文档导航，串联阶段计划、结果报告和后续优化计划 |
| `PROJECT_PLAN.md` | 新增当前实现状态，标注 SIFT1M 版本已完成能力和后续重点 |
| `docs/README.md` | 新增统一文档索引、阶段完成度、结果文件和后续优先级 |
| `docs/competition/scoring-and-defense.md` | 新增 90 分评分映射、答辩顺序和提交前验收清单 |
| `10_*` ~ `23_*` | 纳入阶段计划并补充当前实现状态 |
| `docs/experiments/param-tuning-and-sift-scale-test.md` | 纳入参数调优与 SIFT 规模测试计划 |
| `docs/design/fresh-streaming-ann.md` | 纳入 streaming ANN 创新点计划，并标明已完成/未完成边界 |
| `docs/design/beam-width-io-uring.md` | 纳入 beam width + io_uring 批量读取语义归档 |

本文档记录当前可展示版本的能力边界、关键改动、验证结果和遗留工作。时间以 Asia/Shanghai 为准。

## 2026-06-21：SIFT1M Mixed RW Immutable Read View

### 版本目标

上一版已经跑通 SIFT1M SSD 主图上的混合读写闭环，但 `latest_record_lookup` 在混合写入压力下出现秒级尾延迟。该字段表示：SSD base graph 返回 topK 后，查询动态层确认这些 base id 是否被 update/delete 覆盖的耗时。

本版本目标是把 `DynamicWriteManager` 的读侧 snapshot/lookup 从全局 `mutex_` 中拆出，避免 reader 和 writer/flush/compaction 排队。

### 核心改动

| 模块 | 改动 |
| --- | --- |
| `DynamicWriteManager` | 新增 immutable `DynamicReadView`，用 atomic `shared_ptr<const DynamicReadView>` 发布读快照 |
| `current_sequence()` | 使用 atomic sequence，无需进入 manager mutex |
| `latest_record()` / `latest_records_for()` | 从 read view 按 `read_sequence` 查询，不再持锁扫描 memtable/SSTable |
| `snapshot()` | 从 read view 构造动态可见集，用于动态 Recall |
| `search_delta_l2_at()` | 从 read view 收集 visible delta 后计算 L2 |
| `open()` | 恢复时加载 manifest SSTable 和 WAL 记录，构造 read view |
| `bench_mixed_rw` | 输出读路径分段耗时、benchmark config、git 状态和 JSON 结构化结果 |

### 读路径语义

每次查询开始时：

1. `PackedGraphEngine` 读取 `read_sequence = dynamic_manager.current_sequence()`。
2. SSD base graph 完成 ANN 查询。
3. 根据 base topK ids 调用 `latest_records_for(ids, read_sequence, include_deleted=true)`。
4. 调用 `search_delta_l2_at(query, dim, topk, read_sequence)` 搜索可见 delta。
5. `merge_base_and_delta_l2` 应用 update/delete/insert 语义并返回最终 topK。

read view 中保存当前进程可见的动态记录历史，查询时只读取 `sequence_id <= read_sequence` 的版本，因此 query 执行过程中 writer 新写入的记录不会污染本次动态 Recall。

### SIFT1M 结果摘要

| 场景 | read_qps | write_qps | read P95 | latest_record_lookup P95 | 说明 |
| --- | ---: | ---: | ---: | ---: | --- |
| 旧 mixed no compaction | 2.03 | 148.68 | 5934 ms | 5871 ms | 读侧抢锁/扫描导致瓶颈 |
| 新 mixed no compaction | 23.50 | 217.05 | 167 ms | 0.071 ms | immutable read view 后主路径恢复到百毫秒级 |
| 旧 mixed compaction | 2.42 | 148.38 | 4168 ms | 4105 ms | compaction 仍受 lookup 阻塞影响 |
| 新 mixed compaction | 28.86 | 221.33 | 178 ms | 0.073 ms | 后台 compaction 可与读写并行运行 |
| dynamic recall evidence | 8.84 | 250.36 | 233 ms | 0.073 ms | Recall@10 = 1.0，27 个样本 |

关键结果文件：

| 文件 | 用途 |
| --- | --- |
| `build/sift1m_mixed_rw_no_recall_no_compaction.json` | immutable view 优化前 no-compaction 对照 |
| `build/sift1m_mixed_rw_no_recall_compaction.json` | immutable view 优化前 compaction 对照 |
| `build/sift1m_mixed_rw_no_recall_no_compaction_immutable_view.json` | 优化后 mixed no-compaction |
| `build/sift1m_mixed_rw_no_recall_compaction_immutable_view.json` | 优化后 mixed compaction |
| `build/sift1m_dynamic_recall_immutable_view.json` | 优化后动态 Recall evidence |

### 已验证命令

构建：

```bash
cmake --build build --target bench_mixed_rw test_dynamic_insert -j$(nproc)
```

测试：

```bash
./build/agent_aware_tests
./build/test_disk_record
./build/test_disk_index_rw
./build/test_entry_selector
./build/test_memtable
./build/test_dynamic_wal
./build/test_sstable
./build/test_compaction
./build/test_dynamic_insert
```

### 代价与取舍

| 项目 | 变化 |
| --- | --- |
| 读路径锁竞争 | 大幅降低，reader 不再进入 manager 全局 mutex |
| `latest_record_lookup` | P95 从秒级降到约 0.07ms |
| recovery | 恢复时需要扫描 SSTable/WAL 构造 read view，`recovery_time_ms` 从亚毫秒级上升到约 28-34ms |
| 内存 | read view 保存动态历史记录，适合当前原型和中小 delta；大 delta 需要进一步做分层/索引 |
| 持久化 MVCC | 当前 view 在进程内维护历史；重启后通过 SSTable/WAL 重建当前动态历史 |

### 当前限制

1. Delta 搜索仍是 linear scan，`delta_search_p95_ms` 在 6k-7k delta 下约 8-12ms。
2. `DynamicReadView` 每次写入发布完整历史副本，写入吞吐仍可展示，但大 delta 下需要改为分段 immutable chunks 或 shared delta table。
3. 周期性 rebuild packed graph 仍未实现。
4. 后台 flush 队列还未完全异步化，flush 仍在 manager 内部路径完成。

## 2026-06-20：SIFT1M Mixed RW Breakdown

### 目标

把性能实验和正确性实验分离，并在 `bench_mixed_rw` 中加入读路径分段指标。

### 新增指标

| 字段 | 说明 |
| --- | --- |
| `base_search_*_ms` | SSD packed graph 主图查询耗时 |
| `latest_record_lookup_*_ms` | base id 动态覆盖记录查询耗时 |
| `delta_search_*_ms` | delta 层向量搜索耗时 |
| `merge_*_ms` | base/delta merge 耗时 |
| `exact_recall_*_ms` | 动态 Recall exact truth 回算耗时 |
| `search_mutex_wait_*_ms` | packed graph search mutex 等待 |
| `page_read_wait_*_ms` | demand page read 等待 |

### 结论

纯读 baseline 证明 SSD graph 主路径本身可扩展：

| reader | read_qps | P95 |
| ---: | ---: | ---: |
| 1 | 12.76 | 91 ms |
| 2 | 24.64 | 105 ms |
| 4 | 42.61 | 119 ms |
| 8 | 56.55 | 178 ms |

混合读写慢点主要集中在 `latest_record_lookup`，而不是 base graph、I/O 或 exact Recall。

## 2026-06-20：SIFT1M SSD 主路径验证

复用已有 `indexes/sift1m_vamana_pq100_p4096_sm.idx` 跑 SIFT1M 主路径：

| 指标 | 值 |
| --- | ---: |
| base_count | 1,000,000 |
| query_count | 100 |
| dim | 128 |
| Recall@10 | 0.9940 |
| QPS | 2.5989 |
| avg latency | 384.78 ms |
| P95 | 501.005 ms |
| P99 | 507.288 ms |
| resident ratio | 0.199992 |
| I/O mode | io_uring |
| cache policy | graph-aware-2q |

结果文件：

```text
logs/sift_bench/codex_sift1m_once_20260620-022610/result.json
```
