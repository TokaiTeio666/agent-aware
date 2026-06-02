# AgentMem-Flow 项目计划书

## 1. 项目名称

中文名：AgentMem-Flow：面向 Agent 记忆访问局部性的自适应分层向量检索 I/O 引擎

英文名：AgentMem-Flow: An Agent-Aware Adaptive Hierarchical Vector Retrieval I/O Engine

## 2. 项目定位

AgentMem-Flow 面向大模型 Agent 长期记忆系统中的向量检索 I/O 优化。项目不是单纯复现 DiskANN、HNSW、Vamana、PQ、LSM 或 io_uring 的已有组合，而是围绕 Agent memory 的访问局部性进行系统设计：

- 同一会话连续查询具有会话局部性。
- 相似 query 的图检索路径高度相似。
- 近期写入 memory 更容易被再次访问。
- 历史热点 memory 会被反复召回。
- 读写混合和后台合并会放大查询 P99 延迟。

项目目标是在受限内存下构建一个可验证、可演进的 SSD 向量检索 I/O 引擎 baseline，并逐步实现共访问磁盘布局、Agent-aware cache、Query Path Cache、Delta Index、WAL 和 SLA-aware compaction。

## 3. 计划书问题编号

后续每个版本分析报告都必须说明解决了哪些计划书问题。

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

## 4. 当前 Baseline

### V0：内存精确检索基线

定位：提供正确性上界和评测闭环。

已实现：

- C++17 工程骨架。
- 内存向量容器 `VectorSet`。
- SIFT `fvecs` / `ivecs` 加载。
- synthetic 数据生成。
- squared L2 精确暴力 Top-K。
- Recall@K、1-Recall@K、QPS、Avg/P50/P95/P99 latency 指标。

解决问题：P1，部分解决 P0/P9。

### V1：naive SSD graph baseline

定位：建立后续 I/O 优化的磁盘图检索对照组。

已实现：

- exact kNN graph builder。
- `NaiveDiskGraphIndex` 二进制图索引。
- one-node-per-page 固定 4 KB page 布局。
- `--engine graph` 图检索模式。
- resident sampled router。
- `ssd_reads_per_query`、visited/query、expanded/query、I/O amplification 指标。

解决问题：P2，并暴露 P3。

### V2：Co-Access Packed Page Layout

定位：将 one-node-per-page 升级为 packed page layout，降低图遍历过程中的随机 page read 次数。

已实现：

- `--layout one-node|packed`。
- `--packing random|bfs|coaccess`。
- packed page 文件格式。
- node id 到 packed page 的 directory。
- page-level SoA：page 内部分离存储 node id、degree、vector block、neighbor block。
- random packing、BFS packing、co-access packing 三种策略。
- Agent-style synthetic trace co-access score。
- `--run-type cold|warm|smoke` 和 `--warmup-runs`。

解决问题：P3，部分解决 P0/P9。

### V3：Agent-Aware Cache

定位：在 V2 packed layout 上增加应用级 page cache，验证 Agent memory 局部性对缓存策略的收益。

已实现：

- `--cache-policy none|lru|agent`。
- `--cache-pages` 控制全局 packed page cache 容量。
- 跨 query 的 global page cache。
- LRU eviction baseline。
- Agent-aware eviction score：综合访问频率、最近访问时间和 page 内节点密度。
- page cache hit rate、hits/query、misses/query、requests/query 指标。

解决问题：部分解决 P4，继续缓解 P3，部分解决 P0/P9。

局限：当前 Agent-aware score 仍是最小可落地版本，尚未显式接入 session id、semantic cluster id 或 path hotness。

### V4：Query Path Cache

定位：利用相似连续 query 的路径局部性，复用历史入口点和 Top-K 候选，减少重复图遍历。

已实现：

- Agent-style synthetic workload：`--synthetic-workload agent`。
- `--session-length` 控制连续相似查询长度。
- `--path-cache-policy none|reuse`。
- `--path-cache-capacity`。
- `--path-cache-hit-search-width`。
- query signature：支持 resident routed entry、SimHash、PQ prefix 和 SimHash+PQ hybrid。
- path cache 命中后复用历史 seeds 和 Top-K ids，并降低 search width。
- path cache hit rate、hits/query、requests/query 指标。

解决问题：部分解决 P5，并为 P4 提供 path hotness 信号。

局限：当前 PQ prefix 使用 resident sample 训练小码本，仍是轻量 baseline；复用对象是 seeds 和 Top-K ids，不是完整 frontier。

### V5：Delta Index + WAL + SLA-aware Compaction

定位：把系统推进到动态读写场景，支持实时插入、查询时 Main + Delta Top-K merge，并验证后台合并对 P99 的影响。

已实现：

- `WalWriter`：插入先写 WAL。
- WAL replay：`--wal-replay` 启动时解析 WAL insert record 并恢复 Delta。
- `DeltaFlatIndex`：active delta + sealed delta，支持精确搜索。
- `DeltaIvfFlatIndex`：小型 IVF-flat Delta ANN，使用 farthest-point 初始化和周期性 k-means rebuild，可用 flat delta 计算 delta recall。
- Delta IVF 训练参数：`--delta-ivf-train-iterations`、`--delta-ivf-rebuild-interval`。
- Main Graph ANN 结果与 Delta exact 结果 Top-K merge。
- mixed workload：`--workload-mode mixed`、`--operation-count`、`--write-ratio`。
- delta index policy：`--delta-index-policy flat|ivf-flat`。
- compaction policy：`none`、`aggressive`、`sla`。
- SLA-aware compaction：把合并工作移到写入路径，并在近期 P99 超预算时跳过。
- insert latency、WAL records/replay records、delta active/sealed size、delta search latency、delta recall、compaction ops、query compaction interference 指标。

解决问题：部分解决 P6/P7，继续补强 P0/P9。

局限：

- Delta IVF-flat 是最小 ANN baseline，不是完整 HNSW；大规模 delta 仍可继续升级为 Delta HNSW 或分层 delta segment。
- compaction 目前是 active-to-sealed 内存合并，尚未真正重写 Main Graph SSD 文件。
- 当前 compaction I/O 干扰使用可控时间片模拟，正式实验应在 Linux/WSL 下替换为真实后台文件 I/O。

## 5. 已完成扩展与未来规划

### V5.1：WAL Replay

目标：补齐崩溃恢复语义。

已完成：

- 解析 V5 WAL 文件。
- 程序启动时通过 `--wal-replay` replay insert record。
- replay 后继续以 append 模式写 WAL，避免覆盖已有记录。
- 输出 `wal_replay_records`、`wal_replay_bytes`、`wal_replay_delta_size`。
- 本地脚本：`scripts/run_v5_1_wal_replay.ps1`。

通过标准：

- WAL replay 后 `wal_records == delta_total_size`。
- replay 前后同一 workload Recall@10 一致。

### V5.2：Delta HNSW 或 IVF-flat

目标：降低大规模 delta 的查询成本。

已完成：

- 保留 Delta flat 作为正确性对照。
- 增加小型 `DeltaIvfFlatIndex`。
- 输出 `delta_search_ms_*`、`delta_exact_search_ms_*` 和 `delta_recall_at_10`。
- 本地脚本：`scripts/run_v5_2_delta_ann_compare.ps1`。

通过标准：

- Delta ANN Recall@10 不明显低于 flat。
- delta search latency 随 delta size 增长更慢。
- IVF 训练成本通过 insert latency 可观测。

### V6：AutoDL 部署版与真实文件 I/O Compaction

目标：将 V5 的 compaction 时间片模拟替换为真实文件读写，并将最终实验环境固定为 AutoDL Linux 服务器。

已完成：

- `--compaction-io-mode time|file`。
- `--compaction-io-path`。
- `--compaction-io-bytes-per-vector`。
- Linux 下 file compaction 使用 `open/write/fsync/close`。
- `compaction_io_bytes` 指标。
- AutoDL synthetic 实验脚本。
- AutoDL SIFT 官方 `.ivecs` 实验模板。
- AutoDL 环境采集、drop caches 和证据打包脚本。

通过标准：

- SLA 策略在真实 I/O 干扰下仍降低 P99。
- compaction backlog 和 query latency 都可观测。
- `compaction_io_bytes > 0`。
- AutoDL warm/cold 结果完整归档。

局限：

- 当前是真实文件 I/O compaction，不是完整 io_uring/O_DIRECT 后台合并。
- V6 本身仍没有近似建图器；该限制在 V8 中通过 `approx-rp` 原型开始补齐。

### V7：Query Signature Policy 对比

目标：将 V4 Query Path Cache 的签名策略独立归档验证，明确不同 signature 对路径复用命中率和 I/O 放大的影响。

已完成：

- `scripts/run_v7_signature_compare.ps1`。
- 对比 `routed`、`simhash`、`pq-prefix`、`simhash-pq`。
- 记录 path cache hit rate、SSD reads/query、expanded/query、visited/query。
- 补齐 V7 结果、配置、日志、build info 和分析报告。

通过标准：

- Recall@10 不明显下降。
- path cache hit rate 可观测。
- SSD reads/query 相比 no path cache 下降。
- V7 归档完整。

局限：

- 当前 V7 是 synthetic agent warm/smoke；正式 SIFT cold/warm 结论仍需后续运行。
- Path cache 仍复用 seeds 和 Top-K ids，不是完整 frontier。

### V8：FreshVamana + LSM-style StreamMerge

目标：支持删除密集的动态更新，并替换 SIFT100K/SIFT1M 上不可承受的 exact kNN 建图路径。

已完成：

- `--graph-build-policy exact|approx-rp`。
- 近似随机投影候选建图：`--approx-projections`、`--approx-window`、`--approx-random-samples`、`--approx-candidate-limit`。
- FreshVamana-style RobustPrune：`--robust-prune-alpha`。
- 删除 patch：删除节点后连接入点和出点，并对超度数节点运行 RobustPrune。
- WAL insert/delete record 和 replay counters。
- `--delete-ratio` mixed workload。
- tombstone 查询过滤。
- `--compaction-policy stream-merge`。
- `--stream-merge-index` 输出新的 LTI index。
- WSL/SIFT 本地脚本默认使用 approx-rp + BFS packing。

通过标准：

- `approx-rp` 可以完成 SIFT10K FreshVamana 回归。
- mixed delete workload 中 `delete_count == tombstone_count`。
- `wal_records == insert_count + delete_count`。
- `stream_merge_ops > 0` 并写出新 LTI index。
- dynamic Recall@10 不低于 0.95。

局限：

- 当前是 FreshVamana + LSM-style StreamMerge 原型，不是完整 FreshDiskANN 生产级 block merge。
- StreamMerge 写出新 LTI 后不在同一进程热切换；下一次运行需使用 merged index。
- SIFT100K/SIFT1M 正式 cold/warm 还需要在 WSL2/AutoDL 上完成。

### V9：FreshLSH-Vamana SIFT1M 构图与低延迟搜索

目标：在保留 FreshVamana 删除语义的同时，替换 V8 中仍偏重的 `approx-rp + RobustPrune` 建图路径，让项目能支撑 SIFT100K/SIFT1M 现场构建，并通过自适应停止降低查询扩展数。

已完成：

- `--graph-build-policy lsh-rp`。
- 多表 SimHash LSH 候选生成：`--lsh-tables`、`--lsh-bits`、`--lsh-probe-radius`、`--lsh-bucket-limit`。
- 构图阶段使用快速 top-k prune，避免对大候选集做完整 RobustPrune 两两距离比较。
- 反向边改为批量收集后一次性裁剪，减少反复插入和距离重算。
- FreshVamana 删除 patch 改为批量构建 incoming edges，不再每个 delete 全图扫描一次。
- `--search-early-stop` 与 `--search-early-stop-min`，将固定 search width 改为“上限 + 最小探索 + frontier 自适应停止”。
- WSL/SIFT 和 AutoDL SIFT helper 默认切到 `lsh-rp`，并开启稳健 early-stop 档。

通过标准：

- SIFT10K 建图时间显著低于 V8 `approx-rp + RobustPrune`，Recall@10 不低于 0.95。
- SIFT100K 可以现场建图并保持 Recall@10 不低于 0.95。
- early-stop 档能在 Recall@10 不低于 0.95 时降低 P99 和 SSD reads/query。
- FreshVamana + StreamMerge 删除 smoke 仍通过。

局限：

- 当前 V9 是 FreshLSH-Vamana 原型，还没有 PQ 压缩、O_DIRECT、io_uring 和 SSD-resident vector offload。
- SIFT1M 需要在 WSL2 ext4 或 AutoDL SSD 上正式跑 full base + 官方 `.ivecs`；本地 Windows 已验证 SIFT100K。
- `lsh_bits` 需要随规模调节：SIFT10K 建议 10-12，SIFT100K/SIFT1M 默认 14 起步。

### Final：SIFT100K/SIFT1M 标准实验

目标：补齐正式数据集验证。

必须做：

- SIFT100K 开发实验。
- SIFT1M 优先使用官方 `.ivecs` ground truth。
- cold/warm 正式对比。
- 与 naive SSD layout、LRU、Graph-Aware Cache、DiskANN-style baseline 做消融对比。

## 6. 统一实验设计

数据集：

- synthetic smoke：快速验证功能。
- SIFT10K / SIFT100K：开发和消融实验。
- SIFT1M：最终展示，优先使用官方 `.ivecs`。

Workload：

- 冷启动随机查询。
- 热点查询。
- 相似连续查询。
- Agent session-local 查询。
- recent-memory 查询。
- 读写混合：95% query + 5% insert，80% query + 20% insert/update。
- compaction 干扰场景。

Run 类型：

- smoke run：快速功能验证，不做最终性能结论。
- cold run：清空应用缓存并尽量清空 OS Page Cache，体现磁盘 I/O 压力。
- warm run：预热后测试稳态性能。

## 7. 指标体系

必须记录：

- Recall@10。
- 1-Recall@10。
- QPS。
- Avg Latency。
- P95 Latency。
- P99 Latency。
- SSD Reads / Query。
- I/O Amplification。
- run type。
- random seed。
- ground truth 来源。

V3 之后增加：

- Cache Hit Rate。
- Path Cache Hit Rate。
- Query Signature Policy。

V5 之后增加：

- Insert Latency。
- Query Latency under Write Load。
- WAL Records / WAL Bytes。
- Delta Active Size / Delta Sealed Size。
- Compaction Ops。
- Compaction Interference。

V8 之后增加：

- Graph Build Policy。
- RobustPrune Alpha。
- Delete Count。
- Tombstone Count。
- WAL Replay Inserts / Deletes。
- StreamMerge Ops / Vectors / Inserted / Deleted / Seconds。

V9 之后增加：

- LSH Tables / Bits / Probe Radius / Bucket Limit。
- Search Early Stop / Min Expansions。
- Actual Expanded / Query under early-stop。

## 8. 归档要求

每个版本必须归档：

```text
docs/iterations/vX-analysis.md
archive/results/vX-*.txt
archive/configs/vX-*.json
archive/logs/vX-*.log
archive/build_info/vX-*.txt
archive/validation/validation-method-YYYY-MM-DD.md
```

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
- pass criteria。
- 是否通过。

## 9. 推荐优先级

必须完成：

- V0：精确检索和评测闭环。
- V1：naive SSD graph baseline。
- V2：Packed Page Layout。
- V3：Agent-Aware Cache。
- V4：Query Path Cache。
- V5：Delta Index + WAL + SLA-aware Compaction 最小闭环。
- V7：Query signature policy 对比归档。
- V8：FreshVamana + LSM-style StreamMerge 原型。
- V9：FreshLSH-Vamana SIFT1M 构图和低延迟搜索档。
- 验证规范和归档规范。

加分项：

- V5.1 WAL replay。
- V5.2 Delta HNSW。
- V6 真实后台 I/O compaction。
- SIFT1M 官方 `.ivecs` 完整实验。

## 10. 一句话总结

AgentMem-Flow 的核心路线是：先建立可复现的 exact 和 naive SSD baseline，再逐步把 Agent 记忆的时间、语义、会话和路径局部性转化为磁盘布局、缓存策略、路径复用、动态写入、删除修补和批量合并优化。
