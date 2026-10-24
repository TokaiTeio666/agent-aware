# Agent-aware 当前问题清单

> 编写日期：2026-10-25
> 审计对象：仓库 `main` 分支，审计前 HEAD `6d7b69d`（收缩预取）
> 审计方式：通读 README / PROJECT_PLAN / docs 全部文档 + 逐行阅读 `src/`、`include/`、`tests/`、`scripts/`，关键结论均标注代码位置；同时回溯 git 历史对比当前代码与实验结果来源。

## 0. 结论先行

| 级别 | 数量 | 概括 |
| --- | ---: | --- |
| P0：提交前必须修 | 8 | 结果不可复现、预算未强制、read view 名不副实、`update` 无语义、预取可致崩溃、写侧单线程、Recall 口径污染、锁等待指标失真 |
| P1：答辩前应补 | 14 | io_uring 优先级与缓冲池、预算无强制、delta 线性扫描、恢复语义、索引格式校验、并发路径、可复现性 |
| P2：工程与文档债 | 7 | 设计文档漂移、路线图堆积、口径自相矛盾、工具链、发布包装、仓库卫生 |

最关键的 5 条，按对评分的影响排序：

1. **结果不可复现、产物不是当前工具链生成的**（§1.1）。
2. **"内存不超过 10%–20%"这一核心约束没有被强制，`estimated_resident_ratio=0.198505` 是按公式算出来的，不是从 RSS 测出来的**（§1.2）。
3. **Graph-Aware 2Q 的"优于 LRU"结论目前没有任何实验支撑**，仓库里唯一一组直接对照结果是反向的（§2.4）。
4. **"immutable read view"只是原子计数器，不是快照**；而且 `write −> version_shards_` 在生产路径里没有回收，内存与单次查询扫描量随写入总量线性增长（§1.3、§2.5）。
5. **"写入磁盘图 / Fresh Streaming ANN"的描述与实现不符**：新写入的邻居字段写了但从不被读取，没有任何搜索会走到它（§2.11）。

---

## 1. P0：提交前必须修

### 1.1 仓库里的结论与当前代码脱节，按 README 复现不出来

**证据**

- README 引用的 `build/search_width_156_baseline.json`、`search_width_128/192_baseline.json` 以及混合读写 JSON 都不在仓库里（`build/`、`logs/`、`indexes/` 全部被 `.gitignore` 忽略）。
- 该实验记录自身声明：binary commit `91406a8`、**dirty worktree**、单个二进制 mtime `1783690363`、`156/192` 各只跑了一轮、WSL2 环境（`docs/experiments/search-width-tuning.md` 第 23–26、75 行）。
- 但提交历史显示：`8670a61` 重写了构建系统；`8670a61..HEAD` 之间 `src/`、`include/` 共 **51 个文件、约 2.1 万行增删**；`sift_search_benchmark.cpp`、`mixed_rw_benchmark.cpp` 几乎整体替换；`DynamicReadView` 被删除；XGBoost 预取、early-trigger、9 份设计文档被删除；`build/` 产物与结果 JSON 均未提交。也就是说，**当前代码与产出这些结果的代码不是同一个可追溯版本**。
- 混合读写报告本身就是 dirty 树（`docs/experiments/mixed-read-write.md` 第 35、98 行），并且同一组实验出现了两套数字：旧 changelog 记录"新 mixed no compaction：read_qps 23.50、P95 167 ms"，README 记录"95/5：read_qps 647.05、P95 7.23 ms"，中间没有版本号也没有代码链接。旧数值的出处 `docs/changelog.md` 已在 `bc2b942` 中删除，只能从 git 历史查看。
- 单轮结果之间的波动大过部分"结论"：`docs/experiments/search-width-tuning.md` 第 75 行记录 `128` 首次运行 Avg 79.72 ms、复跑 49.86 ms；README 却以单轮的 Avg 48.10 ms 对 49.86 ms 得出"`156` 平均延迟降低约 3.5%"。**在无复跑的样本上，这个排序没有统计意义，只能说是候选点。**

**影响**：评审一旦要求按 README 的命令复现，拿到的数字大概率不同，而且无法说明"为什么不同"。

**建议**

1. 用当前 HEAD 的 clean 构建重跑全部主表（SIFT1M 主路径、混合读写四组），每个配置 ≥3 轮、报中位数与区间。
2. 结果 JSON、命令行、`git rev-parse HEAD`、`git status`、`uname -a`、`lsblk` 一并发进仓库（例如 `results/` 目录）。
3. 在 README 每个数字旁标注：产出该数字的 commit、日期、机器。

### 1.2 内存预算没有强制，报告值未计入真实常驻内存

**证据**

- `src/sift_search_benchmark.cpp:809-864` `apply_memory_budget_defaults`：`--memory-budget-ratio` 只用来推算**默认缓存页数**；用户显式传 `--cache-pages` 时超预算只 push 一条 warning（`:859-861`），不截断、不失败。
- 全仓库没有读取 `/proc/*/status`、`getrusage` 或 `mallinfo`，`estimated_resident_bytes` 是模型值。
- 未计入常驻的部分：
  - `QueryPageSession` 每碰到一个节点就复制一份全精度向量（`src/core/query_page_session.cpp:493-505`），`owned_pages_` 在单次查询内只增不减；
  - 每个缓存 `DecodedPage` 同时保存 `bytes` 与原解码节点（含每个节点的 `std::vector` 邻居），多于 `page_size × cache_pages`；
  - `node_in_degrees_` 每节点 4B（`disk_graph_index.cpp:363-380`，懒加载），估算里没有；
  - io_uring 的 `depth` 个对齐缓冲没有计入；
  - `--num-search-threads N` 时每个 worker 各开一份 index，重读全部 PQ codes 与 `node_to_page_`（`sift_search_benchmark.cpp:937-963`），并各自跑一遍全页的入度扫描。
- 另外，页面粒度其实不是"节点页"：`packed_nodes_per_page`（`src/graph/disk_page_codec.cpp:25-34`）在默认 `dim=128`、`degree=32`、`page_size=4096` 下算出来是 0（每节点 4+4+512+128 > 4096），`disk_graph_builder.cpp:88-89` 会直接抛错。也就是说 **README 的示例命令要么用不了，要么用户得自己改成 degree=16 或 page_size=8192**；而 README 的 `--page-size` 只写了 `>=4096`。
- `estimated_resident_ratio` 本身是用默认 6 位有效数字打印的（`json << double` 无 `setprecision`），`0.198505` 的小数位是舍入结果。

**影响**：这是赛题硬约束。"0.1985" 和真实 RSS 可能差得很远（全精度向量、未回收的页、多线程复制都算在预算外），评审用 `ps`/`smem` 一量就会暴露。

**建议**

1. 增加真实测量：进程峰值 RSS（`/proc/self/status` 的 `VmHWM`）+ 各常驻结构分类汇总，进 JSON。
2. 预算变成硬约束：超出时报错或按预算裁剪 cache，而不是 warning。
3. `QueryPageSession` 改为只在查询内保留必要节点或用 RAII 释放，去掉每节点 `vector` 复制；缓存条目存储紧凑化。
4. 修正示例参数（`degree=16` 或 `page_size=8192`），并把 `nodes_per_page > 0` 作为构建前置检查写进 README。

### 1.3 "immutable read view"不提供快照语义；生产路径的版本历史无界增长

**证据**

- 当前 `include/dynamic/dynamic_write_manager.h` 中没有 `DynamicReadView`，`read_sequence` 只是一个 `std::atomic<uint64_t> latest_sequence_`（`dynamic_write_manager.cpp:617-619`）。旧实现确实有过 `atomic shared_ptr<const DynamicReadView>`（`git show b691e77`），在 `8670a61` 中被删除。
- 所以当前的"同一快照"实际上靠**每条记录带 `sequence_id`**：`collect_all_delta_records_at_internal(read_sequence)` 逐 shard 取 `sequence_id <= read_sequence` 的最新版本（`dynamic_write_manager.cpp:663-682`）。语义基本正确，但每次查询都要遍历全部 `version_shards_` 里的**全部节点**，`latest_records_for` 同样是 64 个 shard × top-k 个 id（`:632-660`）。
- **无界增长**：`apply_records`（`:456-463`）只 `push_back`，从不裁剪旧版本。正常写入只会经过 MemTable 和 SSTable，但 `apply_records` 作用在永久的 `version_shards_` 上——flush/compaction 都不清理它。所以：
  - 内存 = O(写过的所有版本 × dim)，不是 O(delta 规模)；
  - `search_delta_l2_at` 和 `select_incremental_neighbors` 每次都要复制、排序全部节点（`:708-750`、`:753-782`）。
- README 与 PROJECT_PLAN 中"`latest_record_lookup` P95 从秒级降至约 0.07 ms"是**旧 read view 实现**的数据，当前代码已无该结构，文档未同步。

**影响**：两条创新点表述（"Immutable Read View 无锁读"、"LSM + delta 层"）与实现不符；长跑（数小时写入）下内存和延迟都会随时间恶化。

**建议**

1. 要么恢复 `DynamicReadView` 并为 SSTable/WAL 重建的版本做裁剪，要么在文档里明确：当前是 `sequence_id` 过滤，不是发布式快照。
2. 版本裁剪：每个 id 只保留"下一个被任何活跃 reader 需要的版本之后"的版本，配合最小活跃读序列。
3. 重新测 `latest_record_lookup`，并注明所用 commit。

### 1.4 `DynamicWriteManager::update` 没有"更新"语义

**证据**：`src/dynamic/dynamic_write_manager.cpp:603-606`

```cpp
bool DynamicWriteManager::update(NodeId node_id, const float* vector,
                                 std::uint32_t dim) {
  return insert(node_id, vector, dim);
}
```

insert 对已存在的 id 不报错，直接追加一条新版本。对**已删除**的 id 再 insert，会以旧 sequence 之后的新版本"复活"它（tombstone 只屏蔽更早的版本），而 delete 后的 insert 语义没有在任何测试里覆盖（`tests/dynamic_write_concurrency.cpp` 只有"删除最后一个 id 后读不到"）。

**影响**：如果上层 Agent 用 delete 表示"遗忘"，后续 insert 不区分 id 复用，记忆恢复语义不明确。

**建议**：明确 `insert` 对已存在 id 的行为（拒绝 / 覆盖 / 复活），写入文档和测试；`update` 与 `insert` 合并为一个带语义参数的接口。

### 1.5 io_uring 启预取时存在缓冲池耗尽的致命路径

**证据**

- 缓冲池容量 = `io_depth`（`src/core/async_page_reader.cpp:345-347`，`get()` 见 `:396-398` 的 `throw "Graph async I/O buffer pool is exhausted"`）。
- 预取侧只按"当前未完成数"算剩余容量（`src/core/query_page_session.cpp:287-292`），没有为 demand 预留：预取占满 `depth-1` 后，demand 的批量读（`demand_batch_limit`，`:435`）申请缓冲即抛异常。
- 单页 demand（`mixed_rw` 的 `read_demand_page`，`async_page_reader.cpp:518`）与预取直接抢同一个池。
- 异常从 `search_one` 一路抛到 benchmark 的 `catch`，进程直接以 `status=failed` 结束。

**影响**：预取"默认关闭"掩盖了它；一旦按 `--enable-prefetch 1` 做正式 A/B（`scripts/agent_aware.py ab` 默认开启、`--prefetch-top-k 2`、`--io-depth 32`），存在整轮实验中途崩掉的可能。

**建议**

1. 预取容量上限改为 `depth − demand_batch_limit()`。
2. 池不足时降级（丢弃本次预取提交并记 skip 计数），绝不抛异常。

### 1.6 写侧只有一把全局流式 `shared_mutex`，无法随 reader 扩展

**证据**

- 读：`collect_all_delta_records_at_internal` 对 64 个 shard 逐个加 `shared_lock`，再逐个解锁（`dynamic_write_manager.cpp:663-682`）。它是在所有 shard 都释放后才返回，因此 N 个 reader 可以并发执行；但每个 reader 每次查询都要复制并排序**全部 delta 节点**，CPU 成本 O(N·写总量)。
- 写：`select_incremental_neighbors`（`:708-750`）在每次写入时都调用同一个函数，在全量 delta 上复制、排序、算 L2；这段计算在 writer 线程同步完成，而 `commit_loop` 是**单线程**批量提交（`:390-453`）。writer 数量增加不会增加写吞吐，只会增加排队时间。
- `wal_group_max_delay_us` 的截止时间在**取出第一条后**才计算（`:423-424`），不能限制队头等待。
- `enable_background_flush` 默认为 `true`（`dynamic_write_manager.h:36`），但关掉它时 `rotate_memtable_locked` 会在提交线程里**同步写完整 SSTable 和 manifest**（`dynamic_write_manager.cpp:489-493`），期间所有提交阻塞。

**影响**：赛题要求"高并发"，而写路径本身是单线程串行 + 每次写 O(delta) 扫描，并发写只会放大延迟，`write_qps` 的扩展性论证无法成立。

**建议**

1. 邻居选择改为近似（LSH/采样/增量图）并移出提交线程；或明确写路径按单 writer 建模，并在报告中如实标注。
2. 读侧加"按 read_sequence 的只读缓存"或 delta 分块摘要，避免每次全量复制。

### 1.7 混合读写默认把 O(N) 全量回算算进读延迟

**证据**：`--recall_sample_rate` 默认 1.0（`src/mixed_rw_benchmark.cpp:59`），`execute_read` 的计时区间包含 `record_recall_sample`（`:1004-1025`），后者会 `snapshot(read_sequence)` 并 `exact_dynamic_topk` 扫完整个 base（`:565-612`、`:817-830`）。README 的 703 QPS 只在 `--recall_sample_rate 0` 下成立。

**影响**：用默认参数复现会得到完全不同的数字；README 有提醒，但参数默认值本身在制造错误口径。

**建议**：默认改为 0，Recall 证据使用显式独立场景（README 已有正确示范）。

### 1.8 锁等待指标恒为 0，`page_read_wait` 含义与文档不符

**证据**

- `search_mutex_wait_us` 在 `lock()` 返回后才计算并写入（`disk_graph_index.cpp:840-844`，`:875` 赋值），拿到锁之前的时间根本没有记录，所以永远测不到；而所有 benchmark 都是"每线程一个 index"，这个锁本身也不会产生竞争。README 里的 `search_mutex_wait_ms` 在混合读写 JSON 中恒为 0。
- `page_read_wait` 记录的是整批 submit → drain 的墙钟时间（`query_page_session.cpp:232-275`），不是单次 I/O 等待；README 第 553 行把它描述为"demand page read wait time"。
- `prefetch_saved_wait_us` 只有累加和输出（`sift_search_benchmark.cpp:759`、`:1331`），全仓库从不赋值。

**影响**：用这些字段做"锁竞争已消除"的论证得不到支持；尾延迟归因会被误导。

**建议**：要么修测量（锁前打点、按 CQE 计时），要么在文档中删除这些字段的解释并标注"不可用"。

---

## 2. P1：答辩前应补

### 2.1 预取始终启用 PQ，`pq_filter_*` 统计与实现不符

- `search_one` 里 `effective_config.adc_enable = true` 是无条件的（`disk_graph_index.cpp:868`），且没有 PQ 会直接抛错（`:619-622`）。
- `add_best` 对每个未访问邻居都无条件入堆（`:791`）；`pq_filter_reject_count` 从不自增；`pq_filter_accept_count` 在每次 ADC 查表时自增（`:119`），并不是"被接受的候选数"。
- 文档（README `--enable-pq`、`docs/roadmap in future/recall-to-io-tail-latency-plan.md` §5.2）仍把 PQ 当作"过滤/拒绝"机制描述。

**建议**：统计改为"ADC 查表次数 / 入堆候选数"，或实现真正的拒绝阈值。

### 2.2 没有提前终止，`search_width` 是唯一预算旋钮

`while (!candidates.empty() && expanded < config.search_width)`（`disk_graph_index.cpp:877-878`）。`docs/design/04-beam-width-io-uring.md` 明确要求保留 early stop / adaptive early stop，实现中不存在。`--search-width` 从 128→156→192 时延迟翻倍（README 表），本质是"扩展越多、延迟越高"的线性关系，没有"简单查询早停"的自适应能力。

### 2.3 多线程路径的隔离方式回避了真正难点

- `agent-aware-search --num-search-threads N`：每个线程各开一份 `PackedDiskGraphIndex`（`sift_search_benchmark.cpp:937-963`），PQ codes、目录、缓存、入度全部 × N，而预算只按 `workers+1` 份目录估算。
- `PackedGraphEngine` 共用一个 index，`search_mutex_` 从进入 `search_one` 一直持到返回，天然串行；`scratch_page_`、`lookup_cached_page` 返回裸指针（`disk_graph_index.cpp:403-421`）在没有该锁时也不安全。
- 结论：**当前没有可工作的"单索引多线程并发检索"路径**。报告里如果出现"高并发"，需要说明"每线程独立索引"这一前提及其内存代价。

### 2.4 Graph-Aware 2Q、"优于 LRU"没有实验支撑

- `docs/design/02-cache-zero-copy.md` 第 244 行列出了 LRU / 2Q / Graph-Aware 2Q 的对照，结果表全部为空；PROJECT_PLAN §10.3 同样只有表头。
- 仓库历史中唯一一组直接对照是 `archive/results/p3-cache-{lru,graph-aware-2q}.txt`（`bc2b942` 已删除，可从 git 历史取出）：synthetic 3000 点、cache 64 页下 `hit_rate` LRU **0.9149** vs graph-aware **0.8718**，QPS LRU **4405** vs **3939**，即 **graph-aware 更差**。
- 当前 `sift_search_benchmark` 支持 `--cache-policy`，但没有任何脚本或报告输出过 LRU/2Q/graph-aware 的 SIFT1M 对照。

**建议**：在 SIFT1M 上按 `none / lru / 2q / graph-aware-2q` 各跑 3 轮，若 graph-aware 不占优，就诚实地把它写成"待验证的启发式"，或者找到它有效的条件（更小缓存、更偏 hub 的 workload）。

### 2.5 恢复、持久化与崩溃一致性

逐条列出，均已确认位置：

| # | 问题 | 位置 | 后果 |
| --- | --- | --- | --- |
| a | 启动时 `open()` 会把全部 SSTable 与全部 WAL 记录**重放到 `version_shards_`**，且每条 SSTable 记录单独 `ifstream` 随机读 | `dynamic_write_manager.cpp:142-196`；`sstable.cpp:173-181, 447-458` | 启动时间 O(全部历史写)，`recovery_time_ms` 度量的是重建，不是 WAL 重放 |
| b | SSTable 的 `.data/.index/.meta` 只 `flush()` 不 `fsync` | `sstable.cpp:410-417` | 断电后 manifest 指向未落盘数据，`SSTableReader::open` 失败，`open()` 直接返回 false（`:153-155`），整个存储无法启动 |
| c | manifest `rename` 不 fsync 目录；rename 失败时先删旧 manifest 再重试 | `manifest.cpp:156-166` | 崩溃窗口内可能新老 manifest 都不存在；`load()` 把缺失 manifest 当成新目录（`:68-72`），静默丢数据 |
| d | WAL 坏记录一律 `break`，无告警、无截断 | `wal.cpp:224-258` | 中间任一记录校验失败会丢弃其后的全部已提交记录，且 `current_sequence()` 仍报告已回放的高位 |
| e | WAL 校验只覆盖 vector 与 neighbors，不覆盖 `deleted`/`node_id`/`sequence_id`/`dim` | `wal.cpp:84-86` | tombstone 位或 node id 翻转不会被发现 |
| f | compaction 后 manifest 只保留输出表，且 `next_manifest.wal_checkpoint` 继承自旧 manifest | `dynamic_write_manager.cpp:829-845` | compaction 不会把 delta 吸收到 base（不重建 packed graph），tombstone 与旧版本永远留在 SSTable；每次重构启动都要重放这个只增不减的历史 |
| g | 后台 flush 失败只计 `flush_failures` 不重试 | `dynamic_write_manager.cpp:582` | 一次写盘失败后该批数据只存在于内存与 WAL；若 WAL 段随后被删，数据丢失 |
| h | 关闭 WAL（`enable_wal=false`）时提交成功不代表可恢复 | `dynamic_write_manager.cpp:338-357` | 缺少文档说明这一模式仅用于对照 |

### 2.6 delta 层只有线性扫描，没有分层/图结构

- `search_delta_l2_at` 是"全量复制 → 过滤 → 全量 L2 → nth_element"（`dynamic_write_manager.cpp:753-782`）。
- `docs/roadmap in future/defense-gap-closure-plan.md` Phase 3/4 的 delta 压测（1k/10k/50k/100k）和 `docs/design/06-fresh-streaming-ann.md` 的 Delta memory graph **都没有做**；README 也没有给出 delta 规模与 `delta_search_ms` 的关系曲线。
- 由于 §1.3 的版本无界增长，实际扫描量等于"写过的不同 id 数"，而不只是"当前 delta 记录数"。

### 2.7 "写入磁盘图"与"Streaming ANN"名不副实

- 写入记录里会算出 `neighbors`（`dynamic_write_manager.cpp:596`、`:708-750`），写入 WAL/SSTable，但读路径**从不使用**：`latest_records_for` 和 `search_delta_l2_at` 只看 `vector` 与 `deleted`；`grep "\.neighbors" src/engine src/core` 无结果。这个字段只是增加了写放大。
- 新写入的节点不会进入 SSD packed graph，query 只能通过 delta 的暴力扫描看到它们；没有周期性 rebuild（`docs/design/05-dynamic-write.md` 自述"未完成"）。
- PROJECT_PLAN 第 4 节说"新节点通过 search_for_construction 和 robust_prune 增量接入 Vamana 图"，与实现不符。
- `--dynamic_graph_degree`、`--neighbor_candidate_limit` 实际只影响写入时被算出来、随后被丢弃的字段。另外 `select_incremental_neighbors` 的候选截断用 `std::hash(node_id ^ node_id)`，对所有记录都是同一个值，等价于随机抽样（`:721-730`），并不是"邻域候选"。

### 2.8 `bench_mixed_rw` 的参数与文档不一致

| 参数 | 文档说法 | 实际 |
| --- | --- | --- |
| `--pq_train_limit` / `--pq_iterations` | README 第 293 行："场景启动前训练一次共享 `PqAdcModel`，训练耗时计入 `pq_training_ms`" | 从索引读取现成 PQ 模型，不训练，`training_ms = 0`；参数解析后被丢弃（`mixed_rw_benchmark.cpp:343-344`、`:728-768`） |
| `entry_count` | "SSD graph entry points" | `sift_search` 固定 `entry_count = 1` 且只用 medoid；`mixed` 传了值但只用单入口（`sift_search_benchmark.cpp:873`、`mixed_rw_benchmark.cpp:784`） |
| `rerank_topk = 100`（默认） | 精排池大小 | 精排只针对已物化的候选页、按 2 页一批读取（`disk_graph_index.cpp:801-806`），100 这个值基本不绑定 |
| `--io_mode odirect` | O_DIRECT 读路径 | 打开失败会静默回退到 pread，只在 JSON 写 `fallback_reason`，不报错 |
| `--enable_compaction 0/1` | 一次测量型 compaction | 仅在手动/后台周期触发，报告中的"compaction 次数"应从 JSON 读取而非推断 |
| `sstable_count` | "SSTable 数量" | `count_sstables()` 统计动态目录下的文件数（`mixed_rw_benchmark.cpp:503`、`:967`），与 manifest 中的表数不完全等价 |
| `--dynamic_graph_degree` / `--neighbor_candidate_limit` | "delta graph 邻居数 / 候选上限" | 只影响写入时计算、随后无任何读取方的 `neighbors` 字段（见 §2.7）；不改变查询行为 |

### 2.9 V3 索引格式校验偏弱，PQ 读取会先分配再校验

- `PQEncoder::read` 先 `resize` codebooks 再检查总大小（`src/core/pq_encoder.cpp:253-260`），畸形 header 可触发超大分配。
- 页面目录解析不校验 `directory_offset` 与记录区的关系（`src/graph/disk_page_codec.cpp:200-216`），`records_offset + page_id × page_size` 没有溢出保护（`disk_graph_index.cpp` 的 `page_offset`），格式为小端原生、依赖 `sizeof(size_t)`。
- 节点缺失时会抛 `"Node is missing from packed page"`（`query_page_session.cpp:105-109`、`disk_page_codec.cpp:156`），单个坏页会让整个查询失败，没有跳过候选的降级路径。
- 相对前面几条，这项优先级低：索引由本仓库自己构建、不接收外部输入，风险主要在"索引文件损坏时的失败方式"而不是安全边界。

**建议**：解析前做完整边界/溢出/对齐校验；单页读取失败记 `stats` 并跳过候选；为格式写 fuzz 用例。

### 2.10 预取实现与其设计文档不一致

- **没有事前预测**：`docs/roadmap in future/CRTP_两阶段页级预取实施计划.md`（1765 行）描述的两层"区域预测 + 拓扑修正"结构不存在；当前实现是每个 batch 之后的 post-expand 排序（`disk_graph_index.cpp:755-790`），以及 `docs/design/03-async-prefetch.md` 描述的"1→2 页自适应限流"。
- **候选窗口被预取器自己截断**：`rank_topology_pages` 只保留前 `scan_limit = 2×beam` 个候选页（`disk_graph_index.cpp:69-72`、`:761-775`），再由 `submit_topology_prefetch` 取前 1–2 个提交，其余页被标为 `prefetch_skip_seen` 后不再参与排序。
- **自适应参数不生效**：前 32 个样本强制 `top_k = 1`（`prefetch_adaptive_controller.cpp:22-23`），`--prefetch-top-k` 未生效；`Decision.required_lead` 返回了但 `submit_topology_prefetch` 从不读取（`query_page_session.cpp:283-299`）；`io_latency_us` 入参被丢弃（`prefetch_adaptive_controller.cpp:37`）；`--prefetch-max-pages-per-query` 在预算耗尽后只走 `limit == 0` 返回，并在 `:299-301` 计入 `demand_priority_blocks_prefetch`，也就是说"预取额度用完"和"demand 优先级保护"在统计里混在一起。
- **控制器无锁共享**：`prefetch_controller_` 是 index 成员，多个 `QueryPageSession` 在无锁情况下读写 `buckets_`（`prefetch_adaptive_controller.cpp:18, 38`）。
- **trace 的 oracle 标签只标已提交页**：`useful` 只对真正提交且后来被使用的页计为 1（`query_page_session.cpp:310, 391, 418`），`scripts/replay_prefetch_policy.py` 据此算 oracle（`:24-26`），因此任何 replay 都会低估其它策略。
- **已预取但未作为 demand 出现的页在非缓存路径下被丢弃**（`query_page_session.cpp:170-199, 531-540`），这些 I/O 只计入提交量。
- A/B 正式验收（`summarize_prefetch_ab.py` 的 11 个门槛）从未运行过：README 自己写"默认关闭、正式 A/B 通过前不开启"，`docs/design/03-async-prefetch.md` 也只写了使用方法。
- **按 README 命令单独开启预取，预取不会发生**：`query_page_session.cpp:31` 把 `config.prefetch_max_pages_per_query`（默认 0）存入 `prefetch_query_limit_`，README 对 0 的解释是"不额外限制"，但 `:293-296` 对 0 的处理是 `query_room = decision.top_k`——每批最多 1–2 页，而且 `decision` 来自控制器。这个行为本质上没有错（0 表示按每批决策），问题在于**README 的参数语义与代码不一致**，并且没有任何文档说明"单独开预取需要先确认 `prefetch_runtime_enabled=true`"。按 README 的参数直接开，很可能看到 `prefetch_submitted = 0` 却找不到原因（io_uring 回退、每批预算、warmup 期 top_k=1 都可能）。§2.12 的脚本可以跑，但它传入的也是同一套默认值。另外，"预取默认关闭、未通过 A/B"这一口径本身是对的，但正因为如此，预取目前不应作为已实现创新点出现在答辩材料里。

（登记待确认）`prefetch_query_limit_` 在 `:31` 直接取 `config.prefetch_max_pages_per_query`，而 `query_room` 只有 `prefetch_query_limit_ == 0` 时才取 `decision.top_k`（`:293-296`），所以非 0 时这一项才生效；请作者确认原意是"每批上限"还是"每次查询总量"，并在 README 与代码里统一。

### 2.11 测试与可观测性不足

- `tests/dynamic_write_concurrency.cpp` 使用 `assert`（33 处），Release/默认构建下 `NDEBUG` 会全部编译掉；只有它和 3 个 C++ 单测、1 个 Python contract smoke。被删除的 8 个单测（`tests/unit/*`，`5a17851` 删除）覆盖的 WAL/SSTable/compaction 语义现在没有测试。
- 没有崩溃注入、没有在 compaction/flush 中途 kill 的恢复测试、没有 `ASan/UBSan` 配置、没有 CI 工作流。
- `dirty_status` 在没有 git 时返回 `"clean"`（`mixed_rw_benchmark.cpp:492-501`），会被误读为"干净构建"。
- 搜索结果 JSON 只在 `sift_search` 记录 `binary_mtime`，不记录编译器、编译选项；构建类型（是否 Release）无人校验。Debug 构建会得到完全不同的数字。
- 日志：核心库无任何日志接口，出现问题时只能抛异常。

### 2.12 `scripts/agent_aware.py ab` 的 cold/hot 口径不一致

- `--cache-condition` 只作为字符串记录进 JSON（`sift_search_benchmark.cpp:286-287`、`:463-465`、`:1062-1063`），本身不改变行为；`cold` / `hot` 的真正差异全部来自 `hot` 分支追加的 `--warmup-query-limit`（`scripts/agent_aware.py:131-133`）。也就是说 `cold` 和 `hot` **不是同一测量口径**：`hot` 的延迟不含预热查询，`cold` 含。
- `summarize_prefetch_ab.py` 把两种条件的轮次分开汇总（按缓存目录），这一点没问题；但 **`--cache-condition hot` 且 `--num-search-threads` 非 1 时程序会直接抛错**（`sift_search_benchmark.cpp:466-469`），脚本默认是单线程所以暂不受影响。
- `ab()` 没有传 `--prefetch-max-pages-per-query`，实际走的是 `agent-aware-search` 的默认 0；结合 §2.10，这个参数本身也没有效果。
- 叠加 §2.10 的预热期间强制 `top_k=1`、§1.5 的缓冲池风险，这条 A/B 路径目前**既不可复现也可能崩溃**。

### 2.13 赛题要求"透明集成"（LangChain 等）没有交付

- 赛题评审要点含"作为底层存储提供标准 API 封装，无需修改上层主流 Agent 框架"。
- 目前只有 C++ 头文件（`StorageEngine` / `PackedGraphEngine` 抽象类），没有 C ABI、Python 绑定、HTTP/gRPC 服务或任何 Agent 框架适配层；PROJECT_PLAN F6 标为"功能目标"但未实现。

**建议**：至少提供 Python 绑定（pybind11）或 C API + 一个最小示例（例如 LangChain `VectorStore` 接口实现），否则这一项会对创新性/完整性评分造成明显失分。

### 2.14 多数据集与规模测试缺失

赛题和评审关注"通用性"。当前所有结果都在 SIFT1M；没有 SIFT100K 的回归、没有 GIST/Deep 等其它维度或分布、没有 base 超过 1M（目录/页大小/uint32 id 上限）的测试。`packed_nodes_per_page` 在 `dim=128, degree=32` 下为 0 也说明参数边界没有系统测试。

---

## 3. P2：工程与文档债

### 3.1 文档体系漂移

| 文档 | 问题 |
| --- | --- |
| `docs/design/01-ssd-storage-path.md` | 第 3 行自述"历史设计"，正文仍引用已删除的 `include/storage/*`、`DiskIndexReader/Writer` |
| `docs/design/04-beam-width-io-uring.md` | 引用已删除的 `naive_disk_graph_index.cpp`；"early stop 不能被破坏"但代码无 early stop |
| `docs/design/02-cache-zero-copy.md` | 原始设计稿与"当前实现状态"混排，`pin/unpin` 独立接口、`PagePinGuard` 实际未实现（自述"未完全拆出"） |
| `PROJECT_PLAN.md` §5.1 / §7 | 描述 `QueryProcessor`、`search_memory_fast`、`LsmWriteManager`、`DiskIndexReader` 等已不存在的名字 |
| `docs/roadmap in future/next-sift1m-mixed-rw-optimization.md` | 第 5 行写"P0/P1 核心任务已经完成"，但同文档后面又列出大量"待做"；`bench_dynamic_correctness`、`--preload_delta_count`、`build/...json` 均不存在 |
| `docs/roadmap in future/defense-gap-closure-plan.md` | 交付物指向 `docs/experiments/param-tuning-and-sift-scale-test.md`、`docs/design/fresh-streaming-ann.md`、`docs/competition/scoring-and-defense.md`，均已被删除 |
| `docs/roadmap in future/CRTP_两阶段页级预取实施计划.md` | 1765 行完整实施计划，没有任何状态标记，未开始也未废弃 |

**建议**：docs 目录重建索引；每篇开头统一给出"状态：已实现 / 部分实现 / 设计稿 / 已废弃 / 规划中 + 对应 commit/PR"。

### 3.2 路线图堆积，缺少单一"当前焦点"

`docs/roadmap in future/` 下四份计划累计约 3600 行，互相引用、优先级不一致（例如 delta memory graph 同时出现在三份计划里，且给出的"几天完成"估计彼此冲突）。没有一份把"提交前剩余窗口 + 必做 / 可选 / 放弃"列清楚。

### 3.3 文档间口径互相矛盾

- README 只给了 `search_width=156` 的单点，PROJECT_PLAN §9 仍写 `ef_search/search_width` 默认 350，`docs/design/04` 又写 `beam_width=0 表示跟随 search_width`。
- 搜索宽度结果表在 README、`docs/experiments/search-width-tuning.md`、PROJECT_PLAN 三处各一份，`128` 的值在 README 是"复跑值"，历史 JSON 是另一套（`docs/experiments/search-width-tuning.md` 第 75 行）。
- `PROJECT_PLAN.md` 第 15 行"高并发纯读：8 reader 下 56.55 read_qps"来自被删除的旧代码，当前 `sift_search` 的并发是每线程独立索引，不能直接对照。

### 3.4 构建与工具链

- `CMakeLists.txt` 未设置 `CMAKE_CXX_FLAGS`，无 `-Wall/-Wextra`、无 `-Werror`、无 ASan/UBSan 选项、未锁定构建类型（只靠用户手动传 `-DCMAKE_BUILD_TYPE=Release`）。
- 无 CI（`git ls-files` 中没有 `.github/workflows`、`Dockerfile`、任何 `*.sh`）；README 说"开发和 CI 可使用 synthetic 数据"，但 CI 不存在。
- 本机环境（Windows 11，无 cmake，MinGW g++ 8.1）无法构建；项目实际上只在作者的 WSL2 上跑过。README 写"GCC/Clang/MSVC 均可"，但 `io_uring`/`O_DIRECT` 之外的 Windows 路径完全没有验证。

### 3.5 发布与提交包装

- 仓库根目录有 21.5 MB 的 `华南师范大学项目答辩.pptx`（二进制大文件进了 git 历史），以及 `.claude/settings.local.json`（个人本地设置，且被跟踪）。
- 当时代码从 `include/agent_aware/*` 迁移到 `include/*` 的提交 `8670a61` 一次性改动了 85 个文件、删除 9 份设计文档与 XGBoost 预取的全部代码；提交信息只有"收缩预取"四个字，无法从中看出删掉了什么、为什么删。
- 提交历史中多次出现 `Zone.Identifier`（macOS 扩展属性）文件，导致在 Windows 上无法 clone（本次克隆时该问题仍在 `main` 上，已在本地单独提交修复，尚未推送）。
- 提交者名在历史里有"深夜打p入""深夜打尸八""深夜打p入"等变体（`464f99b` 就是"修改提交者名字"），答辩前建议统一。

### 3.6 依赖与外部资源

- README 把演示视频放在百度网盘（`pwd=rrxe`），评审未必能访问；SIFT 数据集下载地址是 `http://corpus-texmex.irisa.fr/`（该站点已长期不稳定），`.gitignore` 不包含任何下载脚本。
- `indexes/sift.idx`（数 GB）不入库，复现需要重新构建（`agent-aware-build` 在 1M 点上构建时间未在文档中给出）。

### 3.7 性能数据的呈现

- 尾延迟只报 P95/P99，没有 P999；赛题要求"高并发"，但没有任何并发下的 P999 数据。
- 没有 bytes/query（只有 reads/query），I/O 放大无法换算成带宽压力。
- 混合读写结果中 write QPS 只有 34–145，而 write 事件在 commit 线程上同步 `fdatasync`，需要说明这是"每条记录持久化"的成本，而不是存储引擎上限。

---

## 4. 建议的处理顺序

| 阶段 | 顺序 | 事项 | 耗时估计 | 对应章节 | 产出 |
| --- | ---: | --- | --- | --- | --- |
| 提交前必做 | 1 | 固定 Release 构建 + clean commit，重跑主表（含 3 轮）并入库原始结果 | 1–2 天 | §1.1 | 可追溯的结果目录、README 数据表更新 |
| 提交前必做 | 2 | 真实内存测量 + 预算硬约束 + 修正示例参数（`nodes_per_page > 0`） | 1–2 天 | §1.2 | `rss_peak_bytes`、`memory_budget_pass` |
| 提交前必做 | 3 | 修缓冲池预留、`update` 语义、Recall 默认值、锁等待统计 | 0.5–1 天 | §1.5 §1.4 §1.7 §1.8 | 缺陷修复 + 单测 |
| 提交前必做 | 4 | 文档对齐：去掉或改写与代码不符的表述（read view、增量接入图、PQ 过滤、Graph-Aware 2Q） | 0.5 天 | §1.3 §2.1 §2.4 §2.7 §3.1–3.3 | README / PROJECT_PLAN 修订 |
| 答辩前尽量 | 5 | 版本裁剪 + delta 压测曲线（1k/10k/50k/100k） | 2–3 天 | §1.3 §2.6 | delta 规模对照表 |
| 答辩前尽量 | 6 | LRU / 2Q / graph-aware 对照，决定是否保留该创新点 | 1–2 天 | §2.4 | 缓存对照报告 |
| 答辩前尽量 | 7 | 崩溃/恢复测试 + ASan + CI（哪怕只跑 synthetic smoke） | 1–2 天 | §2.5 §2.11 | CI 绿灯 |
| 加分项 | 8 | Python 绑定或 C API + 最小示例（LangChain `VectorStore` 风格） | 2–3 天 | §2.13 | 示例工程 |
| 加分项 | 9 | 预取正式 A/B（先修 §2.10，再跑五轮） | 2 天 + 机器时间 | §1.5 §2.10 §2.12 | A/B 报告 |

如果时间不够：**第 1–4 项不可省**，第 5、8 项对评分收益最大；第 9 项如果做不完，就维持"预取为实验特性、未通过验收"的现有口径，不要把预取写成创新点。

## 5. 已核实"不是问题"的部分

为避免后续重复排查，以下项目经核对实现正确，不需要改：

- 单页内去重不会跳过节点：同一页的所有节点在 `materialize_page` 中一起物化（`query_page_session.cpp`）。
- 预取不会改变最终 Top-K：预取只填充 `ready_prefetch_pages_`，demand 命中后走同样的 page 读取路径。
- PQ 模型内部一致性：offsets / centroid 数 / 表维度在 `PQEncoder` 中自洽。
- `result.json` 的 QPS 用墙钟计算（`sift_search_benchmark.cpp:996`），预热若显式开启则被排除。
- 页缓存的双检锁查找（`lookup_cached_page` + 加锁复查）在单写者语境下正确。

---

## 附：本次审计的范围与限制

- 只做了静态阅读与 git 历史比对；没有构建、没有运行基准（本机无 cmake / Linux 运行时）。
- 代码行为判断均以具体行号为据；文档中的数据只用于交叉验证，未经实际复跑。
- `docs/experiments/search-width-tuning.md` 的原始 JSON 不在仓库中，其数字只能按文档记录采信，无法核对字段。
