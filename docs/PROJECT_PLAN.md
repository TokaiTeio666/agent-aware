# AgentMem-Flow 项目计划书

## 1. 项目名称

中文名：AgentMem-Flow：面向 Agent 记忆访问局部性的受限内存 SSD 向量检索 I/O 引擎

英文名：AgentMem-Flow: A Memory-Bounded SSD Vector Retrieval I/O Engine for Agent Memory

## 2. 赛题背景

在大模型 Agent 的实际应用中，长期记忆不仅规模庞大，常常远超物理内存，而且高度动态。Agent 会在交互过程中持续写入新的记忆，也会频繁回忆历史关联信息。

当高维向量被组织成图索引并存储在 SSD 等外存上时，传统操作系统文件 I/O 机制会遇到明显瓶颈：图遍历检索会产生大量随机读，使顺序预取和默认 Page Cache 难以发挥作用；实时写入和后台合并又会放大写 I/O 与查询 P99 延迟。

本项目面向这类 Agent memory 场景，设计一个在严格受限内存下运行的底层 I/O 存储与向量检索引擎。系统目标是在数据集原始向量远大于内存预算时，仍能提供可复现的 Top-K 检索、较高 Recall@10、可控查询延迟，以及实时写入/更新能力。

## 3. 赛题任务与硬约束

系统应在严格受限内存下运行，内存预算按数据集原始向量大小计算：

- 推荐实验预算：原始向量数据大小的 10%-20%。
- 默认达标预算：`memory_budget_ratio <= 0.20`。
- 召回指标：`Recall@10 >= 0.85` 作为最低达标线；开发推荐线为 `Recall@10 >= 0.95`。
- 负载类型：支持高并发 Top-K 查询、实时向量插入/更新、读写混合负载和后台合并。
- 硬件适配：支持 Intel/AMD 多核 CPU 与高速 SSD，Linux 环境优先支持 `O_DIRECT` / `io_uring` 等异步 I/O 能力。
- 集成方式：对上层 Agent 框架提供标准 API，不要求修改 LangChain 等上层逻辑。

本项目的核心评价指标：

- 查询准确性：`Recall@10`、`1-Recall@10`。
- 查询吞吐：QPS。
- 查询延迟：Avg/P50/P95/P99 latency。
- I/O 效率：SSD reads/query、I/O amplification、cache hit rate。
- 动态写入：insert latency、WAL records、delta size、compaction interference。
- 内存合规：resident memory bytes、memory budget ratio、memory budget pass。

## 4. 项目定位

AgentMem-Flow 不是单纯复现 DiskANN、HNSW、Vamana、PQ、LSM 或 `io_uring`，而是围绕 Agent memory 的访问局部性进行系统设计：

- 同一会话连续查询具有会话局部性。
- 相似 query 的图检索路径高度相似。
- 近期写入 memory 更容易被再次访问。
- 历史热点 memory 会被反复召回。
- 读写混合和后台合并会放大查询 P99 延迟。

项目目标是在受限内存下构建一个可验证、可演进的 SSD 向量检索 I/O 引擎，并逐步实现受限内存管理、共访问磁盘布局、Agent-aware cache、Query Path Cache、异步预取、Delta Index、WAL、删除修补和 SLA-aware compaction。

## 5. 问题编号

后续每个大版本分析报告都必须说明解决了哪些计划书问题。

| 编号 | 问题 | 为什么重要 |
|---|---|---|
| P0 | 验证方法不完整，缺少 Recall 定义、cold/warm 区分、pass criteria 和完整归档 | I/O 优化结果容易被质疑不可复现或受系统 Page Cache 干扰 |
| P1 | 缺少精确正确性基线 | 后续 ANN、SSD、cache 优化必须有可靠 ground truth |
| P2 | 缺少 naive SSD graph baseline | 没有 one-node-per-page 对照组，就无法证明 packed layout 和 cache 的收益 |
| P3 | one-node-per-page 布局导致随机读放大 | 图遍历式 ANN 会产生大量随机 page read，是主要 I/O 瓶颈 |
| P4 | 普通 LRU/LFU/2Q 缓存不理解 Agent 访问局部性 | Agent memory 具有时间、语义、会话和路径局部性，需要专门缓存策略 |
| P5 | 相似 query 重复走相似图路径 | Query Path Cache 可以复用入口点、候选节点和历史路径，减少重复随机 I/O |
| P6 | 动态写入频繁修改主图成本高 | Delta Index 可先承接新 memory，避免频繁重写主索引 |
| P7 | 后台 compaction 与查询抢 I/O，放大 P99 | SLA-aware compaction 需要根据查询尾延迟动态限速或暂停 |
| P8 | SIFT1M 等标准数据集验证不能依赖暴力搜索 | SIFT1M 应优先使用官方 `.ivecs`，否则验证成本高且不规范 |
| P9 | 结果缺少配置、日志、环境、随机种子和 commit hash | 缺少这些信息会导致结果无法复现，答辩时难以解释差异 |
| P10 | exact kNN 图构建无法支撑 SIFT100K/SIFT1M | O(N^2) 建图会成为标准数据集验证的主要瓶颈，需要可控召回的近似建图路径 |
| P11 | 删除会破坏图索引连通性和召回 | 动态索引不能只追加插入；删除后的入边/出边修补和批量合并决定长期召回稳定性 |
| P12 | 低延迟目标不能只靠扩大 search width | 需要让构图质量、候选生成、反向边裁剪和搜索停止策略共同减少实际扩展与 SSD 读放大 |
| P13 | 缺少严格内存预算机制 | 如果只口头声明受限内存，不能证明系统满足赛题 10%-20% 内存约束 |
| P14 | 缺少异步 I/O 和下一跳预取闭环 | Agent 图检索的随机 I/O 需要通过非阻塞预取隐藏磁盘延迟 |

## 6. 受限内存机制

### 6.1 预算定义

原始数据集大小定义为：

```text
raw_vector_bytes = vector_count * dim * sizeof(float)
```

搜索运行期内存预算定义为：

```text
memory_budget_bytes = min(user_budget_bytes, raw_vector_bytes * memory_budget_ratio)
```

默认参数：

- `memory_budget_ratio = 0.20`。
- 可选严格档：`memory_budget_ratio = 0.10`。
- 如果用户显式指定 `--memory-budget-bytes`，则以 bytes 上限为准。
- 构图阶段和搜索阶段分别统计；最终达标以搜索运行期 resident memory 为准。

### 6.2 计入预算的常驻内存

以下内容必须计入 resident memory：

- PQ codes 与 PQ codebooks。
- 图索引常驻元数据：page directory、node-to-page 映射、degree/offset 元信息。
- 应用级 page cache / BufferPool。
- Query Path Cache 与 query signature 表。
- Resident router sample 或 centroid/PQ router。
- Delta Index active/sealed segments。
- WAL replay 后恢复到内存中的 delta records。
- tombstone bitmap、visited bitmap、临时 frontier 上限。
- LSH routing/candidate 结构中搜索期仍常驻的部分。

以下内容不应在搜索期全量常驻：

- 原始全精度 base vectors。
- 完整 SSD graph page 内容。
- 完整 SIFT1M base 文件。

原始向量应存放在 SSD 上，只在 rerank、精排或必要距离计算时按页读取。若实验使用内存模式，必须标注为 `memory_mode=true`，并且不能作为受限内存达标证据。

### 6.3 运行时强制机制

计划新增 `MemoryBudgetManager`，所有常驻结构必须通过统一接口登记内存占用：

```text
register_component(name, bytes, required_or_elastic)
update_component(name, bytes)
resident_bytes()
remaining_budget()
assert_within_budget()
```

强制策略：

- 启动时计算 `memory_budget_bytes`。
- 固定必需结构超过预算时，直接 fail fast，并输出超预算组件。
- 弹性结构按剩余预算自动缩容：cache pages、path cache capacity、router sample count、delta active threshold。
- mixed workload 中 delta 超预算时，触发 seal/flush/StreamMerge 或拒绝继续插入。
- search frontier、visited bitmap、rerank buffer 必须设置上限，避免高并发下临时内存膨胀。
- 除非显式传入 `--allow-over-budget-for-debug`，否则 `resident_bytes > memory_budget_bytes` 的结果不得标记为达标。

### 6.4 输出与归档字段

每次实验必须输出：

```text
memory_budget_ratio
memory_budget_bytes
memory_resident_bytes
memory_resident_ratio
memory_budget_pass
memory_bytes_pq_codes
memory_bytes_pq_codebooks
memory_bytes_graph_metadata
memory_bytes_cache
memory_bytes_path_cache
memory_bytes_router
memory_bytes_delta
memory_bytes_tombstone
memory_bytes_temporary_peak
memory_mode
```

报告中必须区分：

- 搜索期 resident memory。
- 构图期 peak memory。
- OS Page Cache 是否参与。
- `pread`、`O_DIRECT`、`io_uring` 的实际生效模式。

## 7. 大版本路线图

旧的 V0-V9 版本保留为迭代证据，但计划书对外合并为五个大版本。

| 大版本 | 合并旧版本 | 目标 |
|---|---|---|
| M0：验证与基线层 | V0、V1、V2 | 建立 exact truth、naive SSD graph baseline、packed layout 和可复现评测闭环 |
| M1：受限内存读优化层 | V3、V4、V7，吸收 PQ/ADC rerank | 在 10%-20% 内存预算下完成缓存、路径复用、PQ 压缩和内存预算强制 |
| M2：异步 I/O 与预取层 | V6 的真实 I/O，新增 `O_DIRECT` / `io_uring` / 下一跳预取 | 绕开低效 Page Cache，使用非阻塞预取隐藏随机 I/O 延迟 |
| M3：动态写入与合并层 | V5、V5.1、V5.2、V8 动态更新部分 | 支持 WAL、Delta Index、replay、删除 tombstone、FreshVamana patch 和 StreamMerge |
| M4：大规模构图与最终评测层 | V8 构图、V9、Final | 支撑 SIFT100K/SIFT1M 构图、early-stop 低延迟搜索和正式 cold/warm 归档 |

### M0：验证与基线层

定位：先建立可信评测闭环，再建立 SSD 图索引对照组。

已实现/必须保留：

- C++ 工程骨架。
- SIFT `fvecs` / `ivecs` 加载。
- synthetic 数据生成。
- exact brute force Top-K。
- Recall@K、1-Recall@K、QPS、Avg/P50/P95/P99 latency。
- one-node-per-page SSD graph baseline。
- packed page layout：`random`、`bfs`、`coaccess`。
- `ssd_reads_per_query`、visited/query、expanded/query、I/O amplification。
- `smoke`、`cold`、`warm` run 类型。

通过标准：

- exact baseline 在 synthetic self-check 上 `Recall@10 = 1.0`。
- SSD graph baseline 能输出完整 I/O 指标。
- packed layout 相比 one-node-per-page 降低 SSD reads/query，Recall@10 不明显下降。
- 每次实验归档配置、日志、环境、随机种子和 commit hash。

解决问题：P0、P1、P2、P3、P8、P9。

### M1：受限内存读优化层

定位：把“受限内存”从文档目标落实为运行时机制，同时用 Agent 访问局部性降低 SSD 随机读。

核心能力：

- `MemoryBudgetManager`。
- `--memory-budget-ratio` 与 `--memory-budget-bytes`。
- page cache / BufferPool 根据剩余预算动态设定容量。
- Agent-aware cache：频率、最近访问、page density、path hotness 共同决定淘汰。
- Query Path Cache：复用历史 seeds、Top-K ids 和可选 frontier 摘要。
- query signature：`routed`、`simhash`、`pq-prefix`、`simhash-pq`。
- PQ8 + ADC 预筛，配合 `rerank_topk` 恢复召回。
- 内存模式与 SSD 模式分开统计，内存模式仅作上界对照。

通过标准：

- `memory_resident_ratio <= 0.20`，严格档可跑 `<= 0.10`。
- `memory_budget_pass=1`。
- `Recall@10 >= 0.85`，推荐实验 `>= 0.95`。
- PQ + rerank 后召回明显高于纯 PQ + ADC。
- cache/path cache hit rate 可观测，且 reads/query 或 P99 有下降。

解决问题：P3、P4、P5、P12、P13。

### M2：异步 I/O 与预取层

定位：针对图遍历随机读，放弃完全依赖 OS 顺序预取和默认 Page Cache，在用户态做下一跳预取与异步 I/O 调度。

核心能力：

- `--io-mode pread|odirect|io_uring`。
- `io_mode_effective` 必须真实反映后端是否生效。
- `O_DIRECT` 使用对齐 buffer，减少 OS Page Cache 干扰。
- `io_uring` 批量提交 SQE，减少 syscall 与阻塞等待。
- beam-style 下一跳预取：在计算当前候选距离时，后台拉取下一批邻居页。
- prefetch queue 受内存预算约束，不能无限堆积。
- 记录 submit/complete、batch size、pending depth、short read/fallback reason。

通过标准：

- Linux 上 `odirect` 或 `io_uring` 至少一种后端真实生效。
- cold run 中 `O_DIRECT` / `io_uring` 相比 `pread` 降低 P99 或 reads stall。
- fallback 时必须输出原因，不能把 fallback 结果标记为 native I/O 成果。
- prefetch 队列内存计入 `memory_resident_bytes`。

解决问题：P0、P3、P9、P12、P14。

### M3：动态写入与合并层

定位：支持 Agent 实时写入、更新和删除，避免每次写入都重写主图索引。

核心能力：

- WAL append。
- WAL replay。
- Delta flat index 与 Delta IVF-flat。
- Main Graph ANN + Delta Top-K merge。
- mixed workload：query + insert + delete。
- tombstone bitmap 和查询过滤。
- FreshVamana delete patch：删除后修补入边/出边并重新裁剪。
- StreamMerge 输出新 LTI index。
- SLA-aware compaction：根据查询 P99 动态暂停、限速或转移合并工作。
- 真实 file compaction I/O，并在后续接入 M2 的异步 I/O 后端。

通过标准：

- `wal_records == insert_count + delete_count`。
- replay 后 delta size 与 WAL 记录一致。
- mixed delete workload 中 `delete_count == tombstone_count`。
- dynamic Recall@10 不低于 0.85，推荐不低于 0.95。
- SLA compaction 的 query P99 低于 aggressive compaction。
- compaction I/O bytes、interference、backlog 均可观测。

解决问题：P6、P7、P11、P13。

### M4：大规模构图与最终评测层

定位：解决 exact kNN 构图无法支撑 SIFT100K/SIFT1M 的问题，并形成最终可答辩的标准实验。

核心能力：

- `exact`、`approx-rp`、`lsh-rp` 三类 graph build policy。
- FreshLSH-Vamana：多表 SimHash LSH 候选生成 + fast top-k prune。
- 批量反向边收集与一次性裁剪。
- 批量 FreshVamana delete patch，避免每个 delete 全图扫描。
- `--search-early-stop` 与 `--search-early-stop-min`。
- SIFT100K / SIFT1M 官方数据集实验脚本。
- cold/warm 对比、受限内存档、低延迟档、高召回档。

通过标准：

- SIFT100K 可现场建图，Recall@10 不低于 0.95。
- SIFT1M full base 使用官方 `.ivecs`，不依赖 brute force truth。
- early-stop 在 Recall@10 达标时降低 P99、SSD reads/query 或 expanded/query。
- 最终报告必须同时给出 memory budget pass、I/O mode effective、cold/warm、设备信息和完整命令。

解决问题：P8、P9、P10、P12、P13、P14。

## 8. 统一实验设计

数据集：

- synthetic smoke：快速验证功能。
- SIFT10K：开发、调参和 feature smoke。
- SIFT100K：主要消融与推荐配置验证。
- SIFT1M：最终展示，必须优先使用官方 `.ivecs`。

Workload：

- 冷启动随机查询。
- 热点查询。
- 相似连续查询。
- Agent session-local 查询。
- recent-memory 查询。
- 读写混合：95% query + 5% insert，80% query + 20% insert/update。
- 删除密集混合负载。
- compaction 干扰场景。

Run 类型：

- smoke run：快速功能验证，不做最终性能结论。
- cold run：清空应用缓存并尽量清空 OS Page Cache，体现磁盘 I/O 压力。
- warm run：预热后测试稳态性能。
- memory-bounded run：必须启用 `memory_budget_ratio` 或 `memory_budget_bytes`，并输出 memory budget pass。

## 9. 指标体系

基础指标：

- Recall@10。
- 1-Recall@10。
- QPS。
- Avg/P50/P95/P99 latency。
- SSD reads/query。
- I/O amplification。
- run type。
- random seed。
- ground truth 来源。

内存指标：

- memory budget ratio。
- memory budget bytes。
- resident memory bytes。
- resident memory ratio。
- memory budget pass。
- memory component breakdown。
- temporary peak memory。
- memory mode。

缓存与路径指标：

- cache hit rate。
- cache requests/hits/misses per query。
- path cache hit rate。
- query signature policy。
- expanded/query。
- visited/query。

I/O 指标：

- requested I/O mode。
- effective I/O mode。
- `O_DIRECT` enabled。
- `io_uring` enabled。
- fallback reason。
- submit/complete count。
- batch size。
- pending depth。

动态写入指标：

- insert latency。
- query latency under write load。
- WAL records / WAL bytes。
- WAL replay inserts/deletes。
- delta active/sealed size。
- delete count。
- tombstone count。
- compaction ops。
- compaction I/O bytes。
- compaction interference。

大规模构图指标：

- graph build policy。
- graph build seconds。
- LSH tables/bits/probe radius/bucket limit。
- RobustPrune alpha。
- search early stop / min expansions。
- actual expanded/query under early-stop。

## 10. 归档要求

每个大版本必须归档：

```text
docs/iterations/mX-analysis.md
archive/results/mX-*.txt
archive/configs/mX-*.json
archive/logs/mX-*.log
archive/build_info/mX-*.txt
archive/validation/validation-method-YYYY-MM-DD.md
```

如果保留旧版本编号，也必须在 M 版本报告中说明来源，例如 `M3 includes V5/V5.1/V5.2/V8-dynamic`。

必须记录：

- git commit hash；如果尚无 commit，记录 `no commit yet`。
- git status。
- 编译时间。
- 编译器版本。
- 操作系统版本。
- CPU / 内存 / 磁盘信息；如果当前权限不可采集，需要明确说明。
- 运行命令。
- 数据集路径。
- 随机种子。
- index 参数。
- memory budget 参数。
- memory budget pass。
- I/O mode requested / effective。
- pass criteria。
- 是否通过。

## 11. 推荐优先级

第一优先级：

- M0：验证与基线层。
- M1：受限内存读优化层，尤其是 `MemoryBudgetManager` 与 `memory_budget_pass`。
- M4：SIFT100K/SIFT1M 标准实验链路。

第二优先级：

- M3：动态写入、删除、WAL replay 和 StreamMerge。
- M2：native `O_DIRECT` / `io_uring` 与下一跳预取。

加分项：

- 更完整的 frontier/path reuse。
- Delta HNSW。
- Graph-aware 2Q BufferPool。
- 与 DiskANN/FreshDiskANN 的同机对比。
- AutoDL Linux SSD 上的 SIFT1M full cold/warm 归档。

## 12. 一句话总结

AgentMem-Flow 的核心路线是：先建立可复现的 exact 和 naive SSD baseline，再在 10%-20% 受限内存下，把 Agent 记忆的时间、语义、会话和路径局部性转化为磁盘布局、缓存策略、路径复用、异步预取、动态写入、删除修补和批量合并优化。
