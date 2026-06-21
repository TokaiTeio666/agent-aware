# 高并发与混合写入改造计划

## 当前实现状态（2026-06-21）

本计划的最小可交付闭环已经完成，并在 SIFT1M 上跑出可展示结果。当前版本已经具备真实并发读写 benchmark、动态 Recall 抽样、manifest 发布式 compaction、base/delta update/delete 合并语义，以及读侧 immutable snapshot 发布机制。

| 计划项 | 状态 | 当前实现 |
| --- | --- | --- |
| 并发版 `bench_mixed_rw` | 已完成 | 支持 `--duration_sec`、`--read_threads`、`--write_threads`、JSON 输出 |
| 动态 Recall 计算 | 已完成 | 按 `read_sequence` 构造 visible set，exact 回算单独记录 `exact_recall_*_ms` |
| Manifest/compaction 一致性 | 已完成可交付版 | flush/compaction 发布 manifest，recovery 可打开已发布 SSTable 集合 |
| Base/Delta merge 语义 | 已完成 | insert/update/delete/tombstone 可覆盖 base topK |
| Reader snapshot | 已优化 | `DynamicWriteManager` 读侧使用 immutable `DynamicReadView` + atomic `shared_ptr` |
| 后台 compaction | 已完成可运行版 | workload 中可开启 `--compaction_background 1` |
| 周期性 rebuild 主索引 | 未完成 | 下一阶段将 delta 批量吸收到新 packed graph |
| Delta memory graph | 未完成 | 下一阶段用于提升新写入向量近邻质量和大 delta 场景查询性能 |
| 真正异步 flush queue | 未完成 | 当前 flush 仍沿用 manager 路径，后续可拆成后台队列 |

SIFT1M 关键结果：

| 场景 | read_qps | write_qps | read P95 | Recall |
| --- | ---: | ---: | ---: | ---: |
| SSD 主路径 | 2.5989 | - | 501 ms | 0.9940 |
| pure read t8 | 56.55 | 0 | 178 ms | - |
| mixed no compaction immutable view | 23.50 | 217.05 | 167 ms | - |
| mixed compaction immutable view | 28.86 | 221.33 | 178 ms | - |
| dynamic recall evidence | 8.84 | 250.36 | 233 ms | 1.0 |

更完整的实现说明与结果解释见：

- `docs/changelog.md`
- `docs/experiments/sift1m-mixed-rw-immutable-view.md`

## 目标

本文档用于规划 `agent-aware` 在赛题要求下的高并发混合读写能力改造。目标是在现有 SSD packed graph 检索、Graph-Aware 2Q 缓存、io_uring 预取、WAL/MemTable/SSTable 动态写入基础上，补齐以下能力：

1. 真正的多线程读写混合 benchmark，而不是顺序事件循环。
2. 写入路径不阻塞查询主路径，支持后台 flush 与 compaction。
3. 动态写入数据可被查询稳定命中，并能周期性吸收到新的主索引。
4. 输出可用于答辩和报告的 QPS、尾延迟、Recall、写入吞吐、compaction 抖动等指标。

## 当前状态判断

| 模块 | 当前状态 | 主要问题 |
| --- | --- | --- |
| SSD 主检索 | 已有 packed graph + PQ + cache + io_uring | 单线程结果达标，但多线程读验证不足 |
| 动态写入 | 已有 WAL、MemTable、SSTable、flush、manual compaction | flush/compaction 仍偏同步和手动 |
| 查询合并 | 已有 base/delta Top-K merge | update/delete 对 base 结果的覆盖语义还需加强 |
| 混合 benchmark | 已有 `bench_mixed_rw` | 当前是单线程顺序事件循环，不能证明高并发 |
| 动态接入图索引 | delta 可查，Vamana insert 类存在 | 未真正增量修改 SSD packed 主图 |

## 推荐总体方案

采用 **只读 Base 主索引 + LSM Delta 动态层 + 后台整理 + 周期性重建主索引** 的路线。

```text
Reader threads
  -> 每个线程独立 PackedGraphEngine / PackedDiskGraphIndex
  -> search base graph
  -> search delta layer
  -> merge base + delta + tombstone

Writer threads
  -> append WAL
  -> insert active MemTable
  -> 超阈值后 swap immutable MemTable
  -> 后台 flush immutable -> SSTable

Background threads
  -> compaction SSTables
  -> delta 过大时 build new packed graph
  -> manifest 原子切换 active base index
```

不建议第一阶段做“每次写入立即修改 SSD packed Vamana 主图”。该方案工程风险高，涉及磁盘页重写、反向边维护、缓存一致性、并发读写隔离和召回稳定性。比赛交付中更稳妥的是把主索引视为 immutable snapshot，把实时写入放在 delta 层，后台批量吸收。

## Phase 0：并发 Benchmark 先行

优先把评测工具改真实，避免系统能力无法证明。

### 改造点

新增或重写 `bench_mixed_rw` 的并发模式：

```text
main
  load base data
  open shared DynamicWriteManager
  start background maintenance thread
  start N reader threads
  start M writer threads
  run fixed duration or fixed operation count
  stop all threads
  flush and close
  output JSON/CSV
```

### Reader 线程

每个 reader 线程独立创建 `PackedGraphEngine`，不要多个线程共享同一个 `PackedDiskGraphIndex`。当前 packed graph 搜索内部存在搜索级互斥锁，共享同一个 index 会把读并发串行化。

```cpp
PackedGraphEngineConfig config = base_config;
config.dynamic_manager = shared_dynamic_manager;
PackedGraphEngine engine(config);

while (!stop.load()) {
  auto query = pick_query();
  auto result = engine.search_one(query.data(), topk);
  record_read_metrics(result);
}
```

### Writer 线程

writer 线程共享一个 `DynamicWriteManager`。第一阶段可以使用内部 mutex 保证正确性，后续再优化锁粒度。

```cpp
while (!stop.load()) {
  auto vector = generate_insert_vector();
  manager.insert(next_id.fetch_add(1), vector.data(), dim);
  record_write_metrics();
}
```

### 输出指标

| 指标 | 说明 |
| --- | --- |
| `read_qps` | 查询吞吐 |
| `write_qps` | 插入/更新吞吐 |
| `total_qps` | 总操作吞吐 |
| `read_p50/p95/p99/p999_ms` | 查询尾延迟 |
| `write_p50/p95/p99_ms` | 写入尾延迟 |
| `recall_at_10` | 与 exact base/delta merge 对比 |
| `cache_hit_rate` | page cache 命中率 |
| `graph_reads_per_query` | 查询读放大 |
| `wal_bytes_written` | WAL 写入量 |
| `flush_count` | flush 次数 |
| `compaction_count` | compaction 次数 |
| `compaction_bytes` | compaction 处理数据量 |
| `reader_stall_ms` | 因 flush/compaction/锁等待造成的读停顿 |

### 动态 Recall 计算方式

混合读写场景下不能简单使用静态 SIFT ground truth，因为写入线程会不断产生新向量，update/delete 也会改变可见集合。动态 Recall 应以“查询发起时刻可见的数据快照”为准。

#### 定义

对每个被抽样评估的 query：

```text
visible_set(query_time)
  = base snapshot
  + delta records with sequence_id <= read_sequence
  - tombstone records with sequence_id <= read_sequence
  + update records with sequence_id <= read_sequence
```

系统返回：

```text
approx_topk = engine.search_one(query, k)
```

精确答案：

```text
exact_topk = exact_search(base + visible_delta_snapshot, query, k)
```

Recall@K：

```text
Recall@K = |approx_topk ids ∩ exact_topk ids| / K
```

其中 `K` 默认取 10。若可见集合小于 K，则分母使用 `min(K, visible_count)`。

#### read_sequence 获取

建议 `DynamicWriteManager` 暴露单调递增序列号：

```cpp
std::uint64_t current_sequence() const;
DynamicSnapshot snapshot(std::uint64_t read_sequence) const;
```

reader 在查询开始前记录：

```text
read_sequence = manager.current_sequence()
```

随后 base/delta merge、exact 动态真值计算都只看 `sequence_id <= read_sequence` 的记录。这样可以避免 query 执行过程中 writer 新写入的数据污染本次 Recall。

#### 抽样策略

精确搜索全量 base + delta 成本较高，不能每个 query 都做。建议：

| 模式 | 策略 |
| --- | --- |
| 小规模 synthetic | 每个 read 都计算 exact |
| SIFT1M 正式压测 | 每 `N` 个 read 抽样一次，例如 `--recall_sample_rate 0.01` |
| read-heavy 长压测 | 每秒固定抽样上限，例如最多 5 个 query/s |

输出字段建议：

| 字段 | 说明 |
| --- | --- |
| `recall_at_10` | 抽样查询的平均 Recall@10 |
| `recall_samples` | 参与 Recall 计算的 query 数 |
| `recall_sample_rate` | 抽样比例 |
| `exact_base_count` | exact 计算时 base 数量 |
| `exact_delta_visible_count_avg` | exact 计算时平均可见 delta 数量 |
| `exact_deleted_count_avg` | 平均 tombstone 数量 |
| `recall_read_sequence_min/max` | Recall 样本覆盖的 sequence 范围 |

#### update/delete 处理

计算 dynamic exact truth 时必须应用最新版本语义：

1. 对 delta records 按 `node_id` 保留 `sequence_id <= read_sequence` 的最新记录。
2. 若最新记录是 tombstone，则该 `node_id` 不可见。
3. 若最新记录更新了 base 中已有 id，则 exact 中使用 delta 的新向量，而不是 base 旧向量。
4. 若最新记录是新插入 id，则加入 exact 候选集合。
5. approximate 结果中若包含已删除 id，应在 merge 阶段剔除，否则该 query 的 Recall 会自然下降并暴露 bug。

#### 避免 O(N) 精确搜索过慢

SIFT1M 上动态 Recall 的 exact 计算可以分层：

```text
exact_base_topk = static SIFT ground truth 或 base brute force 抽样
exact_delta_topk = brute force visible delta
exact_topk = merge exact_base_topk + exact_delta_topk + base-id update/delete 修正
```

若使用官方 SIFT ground truth 作为 base truth，需要额外检查 ground truth 中的 base id 是否被 delta tombstone/update 覆盖；覆盖后要用更深的 base 候选或临时 brute force 补足 Top-K。为了简单可靠，正式 Recall 抽样可在后台线程对 base 做 brute force，只降低抽样频率。

### 验收标准

| 项目 | 标准 |
| --- | --- |
| 并发真实性 | 至少支持 `--read_threads` 和 `--write_threads` |
| 压测时长 | 支持 `--duration_sec`，建议默认 30 秒 |
| 读写比例 | 支持 read-heavy、balanced、write-heavy 和自定义比例 |
| 输出格式 | JSON 优先，CSV 可选 |
| 复现性 | 输出完整命令、随机种子、git commit、dirty 状态 |

## Phase 1：异步 Flush

目标是让写线程只做轻量前台操作，避免 MemTable 满时直接承担完整 flush 成本。

### 数据结构

```text
active_memtable
immutable_memtables queue
flush_thread
flush_cv
```

### 写入流程

```text
insert/update/delete
  -> append WAL
  -> write active_memtable
  -> if active_memtable should_flush:
       swap active_memtable into immutable queue
       create new active_memtable
       notify flush_thread
  -> return
```

### Flush 线程

```text
flush_thread
  wait immutable queue
  pop immutable
  write SSTable
  save manifest
  rotate/checkpoint WAL
  publish new SSTable reader
```

### 需要注意

1. manifest 保存必须在 SSTable 数据和索引文件落盘后进行。
2. 查询需要同时看 active MemTable、immutable MemTable、SSTables。
3. shutdown 时必须 drain 所有 immutable MemTable。
4. WAL checkpoint 可以先简化为 flush 后轮转空 WAL。

## Phase 2：后台 Compaction 与限速

当前 compaction 是显式 `CompactionJob::run()`。需要变为后台任务，并避免对查询尾延迟造成明显抖动。

### 触发条件

```text
sstable_count >= threshold
or level0_bytes >= threshold
or tombstone_ratio >= threshold
```

### 限速策略

第一版可采用简单 sleep-based throttle：

```text
每处理 X 条 record 或 Y MB 数据
  if active_readers > threshold:
    sleep(compaction_yield_ms)
```

后续可以加入 token bucket：

```text
compaction_bytes_per_sec = configurable
```

### Compaction 发布

```text
build compacted SSTable
fsync data/index/meta
write new manifest
atomically swap sstable reader list
delete old SSTables after no reader references
```

### Manifest 与 Compaction 一致性细节

Compaction 的核心要求是：**任何时刻 reader 只能看到旧版本完整集合或新版本完整集合，不能看到半个 compaction 结果；崩溃恢复后不能丢失已经确认写入的数据。**

#### Manifest 内容建议

```json
{
  "version": 2,
  "next_sequence_id": 123457,
  "next_sstable_id": 42,
  "active_wal": "wal/wal.log",
  "wal_checkpoint_sequence": 120000,
  "sstables": [
    {
      "id": 40,
      "level": 0,
      "base_path": "sstable/sst_000040",
      "min_sequence_id": 118000,
      "max_sequence_id": 119000,
      "record_count": 1000
    }
  ],
  "generation": 17
}
```

关键字段：

| 字段 | 作用 |
| --- | --- |
| `generation` | 每次 manifest 发布递增，用于 reader 判断快照版本 |
| `next_sequence_id` | 保证恢复后 sequence 不回退 |
| `wal_checkpoint_sequence` | 表示小于等于该 sequence 的 WAL 记录已经被 SSTable 覆盖 |
| `sstables[].min/max_sequence_id` | 加速恢复和 compaction 选择 |
| `sstables[].record_count` | 恢复时校验文件完整性 |

#### Flush 发布顺序

```text
1. 写 SSTable data 到临时文件：sst_000123.data.tmp
2. 写 SSTable index 到临时文件：sst_000123.index.tmp
3. 写 SSTable meta 到临时文件：sst_000123.meta.tmp
4. flush/fsync data、index、meta
5. rename tmp -> 正式 data/index/meta
6. fsync sstable 目录
7. 生成新 manifest 内容，包含新 SSTable
8. 写 manifest.tmp
9. fsync manifest.tmp
10. rename manifest.tmp -> manifest.json
11. fsync manifest 所在目录
12. 发布新的 in-memory SSTable reader snapshot
13. 轮转或截断已经 checkpoint 的 WAL
```

若在步骤 1-6 崩溃，manifest 未引用新 SSTable，恢复时忽略临时文件即可。若在步骤 8-11 崩溃，恢复时只认完整 `manifest.json`；`manifest.tmp` 可删除或按校验规则处理。

#### Compaction 发布顺序

```text
1. 选择 input SSTables，记录 input ids 和 manifest generation
2. 扫描 input，按 node_id 保留最新 sequence
3. tombstone 是否保留取决于 safe_delete_sequence
4. 写 compacted SSTable 到临时文件
5. fsync 并 rename 为正式文件
6. 重新读取当前 manifest
7. 若 manifest generation 已变化，检查 input SSTables 是否仍存在
   - 若仍存在：基于当前 manifest 重新生成替换计划
   - 若不存在：放弃本次 compaction 输出，等待下一轮
8. 写新 manifest：加入 compacted table，移除 input tables
9. 原子 rename manifest
10. 发布新的 reader snapshot
11. 延迟删除旧 SSTable 文件
```

#### Reader Snapshot 规则

reader 不应直接遍历可变的 `sstables_` 容器。建议维护不可变快照：

```cpp
struct DynamicReadView {
  std::uint64_t generation;
  std::shared_ptr<const MemTable> active;
  std::vector<std::shared_ptr<const MemTable>> immutables;
  std::vector<std::shared_ptr<const SSTableReader>> sstables;
};

std::shared_ptr<const DynamicReadView> current_view() const;
```

每次 flush/compaction 完成后构造新的 `DynamicReadView` 并用原子方式发布。旧 reader 持有旧 `shared_ptr`，因此旧 SSTable 文件不能立即删除，需要放入 GC 队列，等待没有 reader 引用后再清理。

#### Tombstone 清理规则

Compaction 不应无条件删除 tombstone。只有满足以下条件时才能丢弃：

```text
tombstone.sequence_id <= safe_delete_sequence
and tombstone 对应的旧版本不会再被任何 active reader / WAL recovery / old SSTable snapshot 看到
```

第一版可以保守处理：所有 tombstone 都保留。这样空间效率差一些，但一致性安全。

#### WAL 与 Manifest 恢复规则

恢复时按以下顺序：

```text
1. 读取 manifest.json
2. 打开 manifest 引用的所有 SSTable，并校验 meta/index/data
3. 读取 WAL
4. 只 replay sequence_id > wal_checkpoint_sequence 的 WAL 记录
5. next_sequence_id = max(manifest.next_sequence_id, max_replayed_sequence + 1, max_sstable_sequence + 1)
6. 构造 active MemTable 与 SSTable reader snapshot
```

如果 WAL 中出现 `sequence_id <= wal_checkpoint_sequence` 的记录，可以跳过；这些记录已经由 manifest 中的 SSTable 覆盖。

#### 并发控制建议

| 操作 | 锁策略 |
| --- | --- |
| insert/update/delete | 短持有 write mutex，保护 WAL append 和 active MemTable |
| current_view/read | 无锁或 shared_ptr 原子加载 |
| flush publish | 短持有 publish mutex，替换 read view |
| compaction publish | 使用 manifest generation 做乐观并发校验 |
| file delete | 后台 GC，不在 reader 临界路径删除 |

#### 最小一致性验收

| 场景 | 验收方式 |
| --- | --- |
| flush 中途崩溃 | 删除进程后重启，已 ack 写入可恢复或仍在 WAL |
| compaction 中途崩溃 | 重启后旧 manifest 可用，临时 compacted 文件不影响读取 |
| manifest rename 后崩溃 | 重启后只看到完整新 manifest |
| reader 与 compaction 并发 | reader 不崩溃，结果来自旧快照或新快照之一 |
| tombstone compaction | 删除后的 id 不会在结果中复活 |

### 验收标准

| 项目 | 标准 |
| --- | --- |
| 后台执行 | 查询和写入期间 compaction 可自动触发 |
| 读不崩溃 | compaction 期间 reader 看到一致 snapshot |
| 尾延迟可控 | 与关闭 compaction 相比，P99 不出现数量级恶化 |
| 统计完整 | 输出 compaction 次数、耗时、输入/输出记录数 |

## Phase 3：Base/Delta 合并语义完善

当前 delta search 会过滤 deleted record，但 base Top-K 中如果出现被删除或更新的 id，需要在 merge 时正确处理。

### 需要支持的语义

| 场景 | 正确行为 |
| --- | --- |
| insert new id | delta 结果参与 Top-K |
| update existing base id | 用 delta 新向量覆盖 base 结果 |
| delete existing base id | 从最终结果中移除该 id |
| insert 后 delete | 最终不可见 |
| 多次 update | 最大 sequence_id 生效 |

### 建议接口

给 `DynamicWriteManager` 增加：

```cpp
std::optional<DynamicRecord> latest_record(NodeId id) const;
std::unordered_map<NodeId, DynamicRecord> latest_records_for(
    std::span<const NodeId> ids) const;
```

合并流程：

```text
base_results
  -> 查询这些 base id 的最新 delta record
  -> deleted 则剔除
  -> updated 则重算距离并替换
delta_topk
  -> 加入候选
sort
deduplicate
topk
```

## Phase 4：Delta 层索引优化

当 delta 记录增长后，每次 `search_delta_l2` 扫描全部 delta 会成为瓶颈。

### 第一版

保持全量扫描，但设置 `delta_rebuild_threshold`，超过阈值触发主索引 rebuild。

### 第二版

给 delta 层增加轻量内存索引：

1. 小规模直接 brute force。
2. 中等规模建立内存 Vamana graph。
3. 多个 SSTable 可按 segment 建独立 delta index。

### 推荐阈值

| Delta 规模 | 策略 |
| --- | --- |
| `< 50k` | brute force delta scan |
| `50k - 500k` | memory delta graph |
| `> 5%-10% base` | 后台 rebuild packed base index |

## Phase 5：周期性 Rebuild 与 Manifest 切换

这是比“实时改主图”更稳的动态接入主索引方案。

### 流程

```text
if delta_count >= threshold:
  snapshot current base + visible delta
  build new packed graph index in temp path
  verify index metadata and smoke recall
  write manifest with new active index path
  new readers open new index
  old readers continue old index until finished
  gc old index later
```

### Manifest 示例

```json
{
  "active_index": "indexes/sift1m_snapshot_0002.idx",
  "base_generation": 2,
  "absorbed_delta_sequence": 123456,
  "delta_dir": "build/dynamic",
  "created_at": "..."
}
```

### 验收标准

| 项目 | 标准 |
| --- | --- |
| Rebuild 不阻塞读写 | 后台构建期间前台继续服务 |
| 原子切换 | 新查询只看到完整新索引 |
| Delta 截断安全 | 只清理已经被新 base 吸收的 sequence |
| Recall 保持 | 切换前后 Recall 不明显下降 |

## 实施优先级

| 优先级 | 任务 | 预计收益 |
| --- | --- | --- |
| P0 | 并发版 `bench_mixed_rw` | 立刻补齐高并发证明 |
| P0 | 每 reader 独立 index/engine | 避免搜索锁导致假并发 |
| P1 | base/delta merge 处理 update/delete base id | 保证动态语义正确 |
| P1 | 异步 flush thread | 降低写入尾延迟 |
| P2 | 后台 compaction thread + 限速 | 降低混合负载抖动 |
| P2 | delta 增长阈值与 rebuild 触发 | 防止 delta scan 无限变慢 |
| P3 | 周期性 packed graph rebuild + manifest swap | 形成完整动态索引闭环 |
| P3 | delta memory graph | 提升大 delta 下查询性能 |

## 建议命令形态

### 并发混合读写

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --duration_sec 30 \
  --read_threads 8 \
  --write_threads 2 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --topk 10 \
  --search_width 350 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/mixed_rw_concurrent.json
```

### Compaction 对照

```bash
# 关闭 compaction
./build/bench_mixed_rw --enable_compaction 0 ...

# 开启后台 compaction
./build/bench_mixed_rw --enable_compaction 1 --compaction_background 1 ...
```

## 报告展示建议

最终报告建议至少给出四组对照：

| 对照 | 目的 |
| --- | --- |
| 纯读单线程 vs 纯读多线程 | 证明读并发扩展性 |
| 混合读写无后台 compaction vs 有后台 compaction | 证明尾延迟控制 |
| delta scan vs delta rebuild 后 | 证明动态数据不会无限拖慢查询 |
| LRU vs 2Q vs Graph-Aware 2Q | 证明缓存策略贡献 |

核心图表：

1. read/write QPS 随线程数变化。
2. P50/P95/P99 查询延迟随写入比例变化。
3. compaction 开关对 P99 的影响。
4. delta size 增长对 Recall 和 latency 的影响。
5. cache hit rate 与 graph reads/query 对比。

## 风险与取舍

| 风险 | 影响 | 处理方式 |
| --- | --- | --- |
| 多线程共享 index 被锁串行化 | QPS 无法提升 | 每 reader 一个 index |
| compaction 抢 I/O | P99 抖动 | 限速、读压力高时让步 |
| delta scan 变慢 | 查询延迟随写入量增长 | delta 阈值 rebuild 或 delta graph |
| update/delete base id 语义错误 | Recall/正确性受损 | merge 前查询 latest delta record |
| rebuild 新索引耗时长 | 后台资源占用大 | 只在 delta 达阈值后触发，使用 manifest 原子切换 |
| 文档宣称超过代码能力 | 答辩风险 | 分阶段描述：已实现、正在实现、后续优化 |

## 最小可交付版本

若时间有限，建议至少完成：

1. 并发版 `bench_mixed_rw`。
2. 每 reader 独立 `PackedGraphEngine`。
3. `DynamicWriteManager` 支持后台 flush。
4. compaction 后台线程可以开关，哪怕第一版只做简单限速。
5. merge 正确处理 base id 的 update/delete。
6. 输出一份 30 秒以上的 read-heavy 混合负载结果。

该最小版本即可把当前短板从“顺序原型”提升为“可证明的高并发混合读写系统原型”。
