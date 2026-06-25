# agent-aware 项目计划书

> 项目名称：agent-aware  
> 项目说明：面向 Agent 记忆的向量检索系统 I/O 优化  
> 英文说明：I/O Optimization of Vector Retrieval Systems for Agent Memory  
> 适用赛题：2026 年全国大学生计算机系统能力大赛操作系统设计赛 OS 功能挑战赛道  
> 编写日期：2026-06-21  
> 最后更新：2026-06-25

## 当前实现状态（2026-06-25）

项目已从最初计划推进到 SIFT1M 全链路可展示版本：SSD packed Vamana 主路径、PQ/ADC、Graph-Aware cache 统计、io_uring/pread fallback、XGBoost 学习型预取排序、early trigger 提前触发、LSM 动态写入、manifest/compaction/recovery、真实并发 mixed RW benchmark 和动态 Recall 抽样均已形成闭环。

| 能力 | 当前状态 | 说明 |
| --- | --- | --- |
| SSD 主路径 | 已完成 SIFT1M 验证 | Recall@10 `0.9940`，resident ratio `0.199992` |
| 高并发纯读 | 已完成 baseline | 8 reader 下 `56.55 read_qps`，P95 `178 ms` |
| 混合读写 | 已完成最小闭环 | 4 reader + 1 writer 下 `23.50~28.86 read_qps`，`217~221 write_qps` |
| 动态写入一致性 | 已完成基础版 | WAL/MemTable/SSTable/manifest/compaction/recovery 均有测试覆盖 |
| 读侧锁优化 | 已完成 | `DynamicWriteManager` 使用 immutable read view 发布，`latest_record_lookup` P95 从秒级降至约 0.07ms |
| XGBoost 预取排序 | 已完成闭环 | PQ+ADC 候选聚合、page-level 特征构造、XGBoost text-dump 推理、top-K/threshold/inflight 限流、trace 采集、离线 replay、在线 A/B（详见 `docs/design/XGBOOST_PREFETCH_PLAN.md`） |
| Early trigger 提前触发 | 已完成 P0（pre-beam） | `--prefetch-early-trigger pre-beam` 在 beam selection 前触发预取，ready_hit_rate 提升、pending_hit_rate 下降（详见 `docs/design/07-prefetch-early-trigger-plan.md`） |
| I/O 尾延迟量化 | 待推进 | reads/query、bytes/query、候选放大等指标补全和分阶段优化（详见 `docs/roadmap/recall-to-io-tail-latency-plan.md`） |
| 后续重点 | 待推进 | 异步 flush queue、Delta memory graph、周期性 rebuild packed graph、系统参数矩阵、真实 NVMe 复测（详见 `docs/roadmap/defense-gap-closure-plan.md`） |

详细阶段计划和结果文档统一维护在 `docs/README.md`，评分映射和答辩材料组织见 `docs/competition/scoring-and-defense.md`。

## 1. 项目概述

agent-aware 面向大模型 Agent 的长期记忆场景，设计一套在内存受限环境下运行的动态向量检索底层 I/O 存储引擎。项目目标不是单纯把所有向量加载到内存获得高 QPS，而是在数据规模远大于物理内存、读写请求持续并发的条件下，通过用户态缓存、图拓扑感知预取、异步 I/O 和日志结构写入路径，降低随机 SSD 访问带来的性能损耗。

系统以 DiskANN/Vamana 图索引为检索骨架，以 Product Quantization 压缩编码作为内存常驻的近似过滤层，将全精度向量下沉到 SSD，通过 O_DIRECT 绕过操作系统 Page Cache，并由 Graph-Aware 2Q BufferPool 接管缓存淘汰策略。读路径使用 io_uring 批量异步读取和下一跳预取来隐藏 I/O 延迟；写路径借鉴 LSM-Tree，将实时随机写转化为 WAL 与 SSTable 的顺序追加写，并通过后台 Compaction 保持读写混合负载下的延迟稳定性。

## 2. 背景与问题定义

大模型 Agent 在真实应用中会持续生成并检索长期记忆。这类记忆通常以高维向量形式存储，规模增长快、访问模式高度动态，并且具备以下特征：

| 挑战 | 表现 | 对传统系统的影响 |
| --- | --- | --- |
| 高维向量规模大 | 全精度向量数据远超可用内存 | 全内存索引不可持续 |
| 图遍历随机读 | HNSW、Vamana 等图索引搜索会访问非连续节点 | OS 顺序预取失效，Page Cache 命中率低 |
| 实时写入频繁 | Agent 交互会不断产生新记忆 | 直接更新磁盘索引会产生随机写和写放大 |
| 读写混合并发 | Top-K 检索与插入/更新同时发生 | 查询延迟容易抖动，尾延迟不可控 |
| 内存约束严格 | 赛题要求约为数据集大小的 10%-20% | 必须精确管理常驻内存内容 |

因此，本项目的核心问题可以定义为：在内存不超过数据集大小 20% 的约束下，如何面向图索引向量检索的随机访问模式，重新设计缓存、预取和写入路径，使系统同时具备较高召回率、可接受的 QPS、稳定的尾延迟和可持续的动态更新能力。

## 3. 建设目标

### 3.1 功能目标

| 编号 | 目标 | 说明 |
| --- | --- | --- |
| F1 | 支持 Top-K 向量检索 | 提供基于 DiskANN/Vamana 图遍历的近似最近邻查询 |
| F2 | 支持动态插入和更新 | 新向量经 LSM 写入路径落盘，并增量接入图索引 |
| F3 | 支持内存受限运行 | 搜索时仅保留 PQ codes、codebooks、图导航数据和有限缓存 |
| F4 | 支持 SSD 常驻全精度向量 | 全精度向量通过 4KB 固定记录布局存储于 SSD |
| F5 | 支持读写混合评测 | 提供单线程、多线程、SSD 模式、内存对照模式和混合负载 benchmark |
| F6 | 支持透明集成 | 对上层 Agent 框架暴露标准化存储与检索 API |

### 3.2 性能目标

| 指标 | 目标值 | 说明 |
| --- | --- | --- |
| 内存占用 | 数据集大小的 10%-20% | 赛题核心约束，默认以不超过 20% 为验收标准 |
| Recall@10 | 不低于 85% | 当前路线目标为 SIFT 数据集上稳定达到 98% 以上 |
| 查询吞吐 | SSD 模式下尽可能提升 QPS | 重点比较 O_DIRECT、BufferPool、io_uring、PQ 过滤的增益 |
| 查询延迟 | 输出 avg、P95、P99、P99.9 | 用尾延迟评估读写混合负载稳定性 |
| 缓存命中率 | Graph-Aware 2Q 显著优于 LRU | 以 hub 节点保护和 I/O 次数下降作为证据 |
| 写入开销 | 随机写转顺序追加写 | 用 WAL、SSTable、Compaction 统计写放大和后台延迟 |

### 3.3 工程目标

| 编号 | 目标 | 说明 |
| --- | --- | --- |
| E1 | 模块边界清晰 | core、io、buffer、compaction、engine、benchmark 分层实现 |
| E2 | 可复现实验 | 固定随机种子、提供 SIFT 与合成数据双路径 |
| E3 | 可退化运行 | io_uring 不可用时回退到同步 pread/pwrite |
| E4 | 线程安全 | BufferPool pin/unpin、LSM 读写和图增量更新需有并发保护 |
| E5 | 文档完整 | README、架构文档、测试文档、实验报告和答辩材料保持一致 |

## 4. 总体技术路线

项目采用“压缩导航在内存，全精度数据在 SSD”的双层设计：

1. 内存层保存 PQ codes、PQ codebooks、图邻接表、VisitedBitmap 和有限容量 BufferPool。
2. SSD 层保存每个节点的 4KB 固定记录，包含 NodeID、全精度向量、邻居列表和邻居 PQ codes。
3. 查询时先用 PQ ADC 进行低成本近似距离过滤，再对少量候选节点发起 SSD 全精度读取。
4. BufferPool 采用 Graph-Aware 2Q 策略，对图中高入度 hub 节点给予更高留存优先级。
5. io_uring 批量提交下一批候选节点读取请求，实现 CPU 距离计算和 SSD I/O 重叠。
6. 写入时先进入 WAL 和 MemTable，达到阈值后 flush 为 SSTable，后台执行 Compaction。
7. 新节点通过 search_for_construction 和 robust_prune 增量接入 Vamana 图，避免简单追加破坏图连通性。

## 技术架构

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                         Agent-Aware Architecture                             │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Benchmark / Agent API                                                       │
│  agent-aware, bench_mixed_rw, StorageEngine                                  │
│                                │                                             │
│                                ▼                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │ Engine Layer                                                           │  │
│  │ PackedGraphEngine: base graph search + dynamic delta merge             │  │
│  └───────────────────────────────┬────────────────────────────────────────┘  │
│                                  │                                           │
│              ┌───────────────────┴───────────────────┐                       │
│              ▼                                       ▼                       │
│  ┌───────────────────────────────┐       ┌───────────────────────────────┐   │
│  │ In-Memory Components          │       │ Dynamic Write Layer           │   │
│  │ <= 20% resident budget        │       │ DynamicWriteManager           │   │
│  │                               │       │                               │   │
│  │  PQ Codes                     │       │  WAL append                   │   │
│  │  Graph Nav Data               │       │  MemTable / immutable view    │   │
│  │  PQ Codebooks                 │       │  SSTable + Manifest           │   │
│  │  Graph-Aware 2Q BufferPool    │       │  Compaction / recovery        │   │
│  │  VisitedBitmap                │       │                               │   │
│  └───────────────┬───────────────┘       └───────────────┬───────────────┘   │
│                  │                                       │                   │
│                  │ PQ ADC filter + exact rerank          │ updates/deletes   │
│                  │ pread / O_DIRECT / io_uring           │ delta records     │
│                  ▼                                       ▼                   │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │ SSD-Resident Components                                                │  │
│  │ Packed Vamana graph index                                              │  │
│  │ Disk node records: fixed 4KB pages                                     │  │
│  │ [NodeID][FullVector][NeighborIDs][NeighborPQCodes][Padding]            │  │
│  └───────────────────────────────┬────────────────────────────────────────┘  │
│                                  │                                           │
│                                  ▼                                           │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │ Query Runtime                                                          │  │
│  │ EntrySelector -> beam search -> QueryPageSession -> PrefetchPlanner    │  │
│  │ Async batch read + topology-aware next-hop prefetch                    │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

## 项目结构

```text
agent-aware/
├── CMakeLists.txt                         # 顶层构建入口，生成库和 benchmark 可执行文件
├── README.md                              # 项目入口、构建运行、实验结果和参数说明
├── PROJECT_PLAN.md                        # 总体计划、技术路线和验收标准
├── include/                               # 对外头文件
│   ├── agent_aware.h                      # 聚合入口头
│   ├── core/                              # 基础类型、距离计算、PQ、预取和异步 page reader
│   │   ├── types.h
│   │   ├── brute_force.h
│   │   ├── pq_encoder.h
│   │   ├── async_page_reader.h
│   │   ├── query_page_session.h
│   │   └── prefetch_planner.h
│   ├── data/                              # SIFT/fvecs/ivecs 读取与 synthetic 数据生成
│   ├── graph/                             # Vamana 构图、packed page 编码、SSD graph index
│   ├── storage/                           # 4KB disk record、disk index reader/writer、LSM 基础组件
│   ├── dynamic/                           # WAL、MemTable、SSTable、Manifest、Compaction
│   └── engine/                            # StorageEngine / PackedGraphEngine 对外接口
├── src/                                   # C++ 实现
│   ├── core/                              # PQ ADC、SIMD/L2、io_uring/pread/O_DIRECT 读路径
│   ├── data/                              # 数据集加载实现
│   ├── graph/                             # 图构建、entry selection、packed graph search
│   ├── storage/                           # 磁盘 record 与索引读写实现
│   ├── dynamic/                           # 动态写入、flush、recovery、compaction
│   ├── engine/                            # base graph + delta merge 查询引擎
│   ├── sift_search_benchmark.cpp          # 构建为 agent-aware，输出 SSD 检索 JSON
│   └── mixed_rw_benchmark.cpp             # 构建为 bench_mixed_rw，输出混合读写 CSV/JSON
├── scripts/
│   ├── run_sift1m_once.sh                 # SIFT1M 一键构建/运行/归档
│   ├── run_agent_aware_sift.py            # 参数矩阵实验运行器
│   └── plot_sift_matrix.py                # 参数矩阵结果汇总与图表生成
└── docs/
    ├── README.md                          # 文档中心索引
    ├── changelog.md                       # 当前版本能力边界、验证命令、已知限制
    ├── competition/                       # 赛题要求、评分映射、答辩清单
    ├── design/                            # SSD 存储、缓存、异步预取、动态写入设计
    ├── experiments/                       # SIFT1M、混合读写、参数调优实验报告
    └── roadmap/                           # 后续优化计划、任务拆分和冲刺路线
```

## 5. 系统架构设计

### 5.1 分层结构

| 层级 | 模块 | 职责 |
| --- | --- | --- |
| API 层 | StorageEngine、QueryProcessor | 封装检索、插入、更新、配置与统计接口 |
| 核心算法层 | PQEncoder、GraphNavData、VamanaBuilder、VisitedBitmap、SIMD Distance | 提供向量编码、图构建、图遍历和距离计算能力 |
| 查询执行层 | diskann_search_enhanced、search_memory_fast | 实现 SSD 合规路径和内存对照路径 |
| I/O 层 | DiskIndexReader、DiskIndexWriter、IoEngine | 负责 4KB 记录读写、O_DIRECT、io_uring 和同步回退 |
| 缓存层 | BufferPoolManager、TwoQueueEvictionPolicy | 管理用户态缓存、pin/unpin、图入度感知淘汰 |
| 写入合并层 | MemTable、WAL、SSTableManager、LsmWriteManager、CompactionManager | 实现追加写、flush、读回和后台合并 |
| 评测层 | benchmark.cpp、SIFT loader、synthetic data | 生成/加载数据，执行性能、召回率和混合负载测试 |

### 5.2 内存预算设计

以 1M 个 128 维 float 向量为例，全精度向量约 512 MB。系统搜索时常驻内存控制如下：

| 内容 | 估算占用 | 是否常驻 | 作用 |
| --- | ---: | --- | --- |
| PQ codes | 约 8 MB | 是 | 每向量 8 字节，用于 ADC 预过滤 |
| PQ codebooks | 约 128 KB | 是 | 每子空间 256 个中心，用于查询距离表 |
| 图邻接表 | 约 65 MB | 是 | Vamana 导航数据，控制 max_degree |
| VisitedBitmap | N/8 级别 | 是 | 替代 unordered_set，降低搜索临时内存 |
| BufferPool | 数据集 5%-15% | 是 | 缓存 SSD 节点页，受总预算动态约束 |
| 全精度向量 | 约 512 MB | 否 | 常驻 SSD，通过 O_DIRECT 按需读取 |

该设计使查询时内存占用稳定在 10%-20% 区间内，并且保留足够缓存空间给热点节点和当前搜索路径。

## 6. 核心模块方案

### 6.1 PQ 编码与 ADC 预过滤

PQEncoder 将 128 维向量划分为 8 个子空间，每个子空间训练 256 个聚类中心。每个向量最终编码为 8 字节 PQ code，常驻内存。

查询时，系统先基于 query 构建 PQDistanceTable，然后对候选邻居计算 ADC 近似距离。ADC 距离明显偏大的节点会被过滤，避免无意义的 SSD 全精度读取。

验收重点：

| 验收项 | 标准 |
| --- | --- |
| 编码正确性 | encode/decode 基础功能可用，维度与 code size 校验严格 |
| 内存收益 | PQ codes 与 codebooks 的占用符合预算 |
| 检索收益 | 开启 PQ 阈值过滤后 SSD 读取次数下降，Recall@10 仍达标 |

### 6.2 Vamana 图索引

图索引采用 DiskANN 风格的 Vamana 结构。构建阶段先生成初始图，再通过 medoid 入口点、search_for_construction 和 robust_prune 优化邻居质量。搜索阶段使用贪心图遍历和 beam-style batch 读取。

增量插入时，新节点不会简单 append，而是执行：

1. 从入口点出发搜索候选邻居。
2. 使用 robust_prune 选择高质量邻居。
3. 添加反向边。
4. 对超过 degree 限制的旧节点重新 prune。

验收重点：

| 验收项 | 标准 |
| --- | --- |
| 图连通性 | 增量插入后新旧节点均可被搜索访问 |
| 召回率 | SIFT 10K、100K、1M 上 Recall@10 不低于 85% |
| 内存占用 | max_degree 默认按内存预算选择，图邻接表不突破总预算 |

### 6.3 SSD 磁盘布局

每个节点在 SSD 上占用固定 4KB 记录：

```text
[NodeID][Padding][FullVector][NumNeighbors][NeighborIDs][NeighborPQCodes][Padding]
```

固定 4KB 记录的优势：

| 优势 | 说明 |
| --- | --- |
| O_DIRECT 友好 | 页面天然对齐，减少对齐处理复杂度 |
| 地址计算简单 | offset = node_id * 4096 |
| SIMD 友好 | 向量数据按固定偏移对齐，可直接计算距离 |
| 批量读取简单 | 候选节点可以组织为批量读请求 |

### 6.4 Graph-Aware 2Q BufferPool

传统 LRU 不理解图索引访问模式，容易淘汰图中的 hub 节点。Graph-Aware 2Q 在 warm/hot 队列基础上加入节点入度信息，高入度节点在淘汰时获得保护。

缓存策略：

| 机制 | 作用 |
| --- | --- |
| warm queue | 新读取页面先进入 warm 队列，避免一次性访问污染 hot 队列 |
| hot queue | 被重复访问的页面晋升到 hot 队列 |
| in-degree 保护 | 高入度 hub 节点更不容易被淘汰 |
| pin/unpin | 查询期间保护页面，避免多线程下被提前驱逐 |
| compute_distance_direct | 命中缓存时直接从 page buffer 计算距离，减少 memcpy |

验收重点：

| 验收项 | 标准 |
| --- | --- |
| 命中率 | Graph-Aware 2Q 显著高于 LRU |
| 线程安全 | 多线程查询无 use-after-free、无脏读 |
| 零拷贝收益 | compute_distance_direct 路径可统计并验证 |

### 6.5 io_uring 异步 I/O 与拓扑预取

查询遍历图时，当前 batch 的距离计算和下一 batch 的 SSD 读取可以重叠。IoEngine 提供统一接口，在支持 USE_IOURING 时使用 io_uring，否则回退到同步 pread/pwrite。

策略：

| 策略 | 说明 |
| --- | --- |
| batch submit | 多个候选节点读取请求一次性提交，减少 syscall |
| beam_width | 控制每轮读取候选数，默认 8，大规模数据可调至 16-32 |
| topology-aware prefetch | 根据当前节点邻居集合提前拉取下一跳 |
| sync fallback | io_uring 不可用时自动同步读写，保证可运行 |
| 多线程策略 | 多线程 SSD 搜索优先使用同步 BufferPool 读取，避免共享 async buffer 风险 |

### 6.6 LSM 写入路径

写路径采用 LSM-Tree 思想，将随机写转为顺序追加写。读回顺序为 Active MemTable、Immutable MemTable、SSTable，保证新数据优先。删除操作通过 tombstone 表示，后台 Compaction 清理过期版本。

验收重点：

| 验收项 | 标准 |
| --- | --- |
| 写入正确性 | insert、update、delete、get 语义正确 |
| 持久性 | WAL 可用于恢复未 flush 的写入 |
| 读写隔离 | 查询线程与写线程并发运行时无崩溃和明显延迟抖动 |
| 合并稳定性 | Compaction 不破坏图导航和最新版本读取 |

### 6.7 XGBoost 学习型预取排序

在基础拓扑预取之上，引入 Learning-to-Rank 思路，将预取点选择建模为搜索状态下的 page-level ranking 问题。整体链路：

```text
PQ+ADC 候选生成 → page-level 候选聚合 → 构造排序特征 → XGBoost 预取收益排序 → top-K/threshold/io_depth 限流 → async prefetch → ready_hit/pending_hit/unused 评估
```

`PrefetchPlanner` 负责候选页聚合、特征构造、XGBoost text-dump 推理打分和限流提交。支持 `--prefetch-policy none|xgboost`、`--prefetch-model`、`--prefetch-top-k`、`--prefetch-score-threshold`、`--prefetch-max-inflight`、`--prefetch-trace` 等命令行开关。离线 replay 脚本可对比 XGBoost 策略与 oracle 上界的差距。

详见专项计划：`docs/design/XGBOOST_PREFETCH_PLAN.md`

### 6.8 Early Trigger 提前触发

针对预取提交太晚导致 `pending_hit` 偏高的问题，引入 pre-beam frontier window 提前触发点。在 beam selection 前从 candidate heap 取 top window 候选进行 XGBoost 打分和预取提交，使高置信 page 在 demand 前至少提前一个搜索批次被提交。

支持 `--prefetch-early-trigger off|entry-warmup|pre-beam|rerank|all`，默认 `pre-beam`。

详见专项计划：`docs/design/07-prefetch-early-trigger-plan.md`

## 7. 读写流程设计

### 7.1 SSD 检索主路径

1. 初始化 VisitedBitmap，构建 query 的 PQDistanceTable。
2. 从 entry point 开始图搜索。
3. 获取当前候选节点邻居。
4. 对邻居 PQ codes 执行 ADC 预过滤。
5. 将保留下来的候选按 PQ 距离排序，截断到 beam_width。
6. 优先从 BufferPool 读取候选节点页面。
7. 缓存未命中时通过 O_DIRECT 和 io_uring/pread 读取 4KB 记录。
8. 通过 compute_distance_direct 计算全精度 L2 距离。
9. 更新候选队列与结果堆。
10. 提交下一批候选预取，重复直到候选耗尽。
11. 返回 Top-K 结果，并输出 I/O、缓存、延迟统计。

### 7.2 内存对照路径

内存路径 search_memory_fast 直接使用全精度内存向量和 SIMD 计算距离。该路径只用于验证算法正确性和展示性能上限，不作为赛题合规成绩，因为它违反内存不超过 20% 的约束。

### 7.3 混合负载路径

混合负载 benchmark 同时启动查询线程和写入线程：

| 线程类型 | 行为 |
| --- | --- |
| 查询线程 | 循环执行 diskann_search_enhanced，记录 QPS、avg、P99、P99.9 |
| 写入线程 | 生成新向量，执行 LsmWriteManager.insert，并增量接入 Vamana 图 |
| 后台线程 | 执行 MemTable flush、SSTable Compaction、旧版本清理 |

## 8. 实施计划

### 8.1 阶段划分

| 阶段 | 周期 | 目标 | 主要产出 | 状态 |
| --- | --- | --- | --- | --- |
| P0 需求澄清 | 第 1 周 | 明确赛题约束、评审指标和基础架构 | 需求说明、指标表、模块边界 | ✅ 已完成 |
| P1 基础检索链路 | 第 1-2 周 | 完成数据加载、PQ 训练、图构建、内存搜索 | PQEncoder、VamanaBuilder、Recall 验证 | ✅ 已完成 |
| P2 SSD 存储链路 | 第 2-3 周 | 完成 4KB 记录布局、O_DIRECT 读写、DiskIndexReader/Writer | SSD index 文件、同步读写测试 | ✅ 已完成 |
| P3 缓存与零拷贝 | 第 3-4 周 | 完成 Graph-Aware 2Q、pin/unpin、compute_distance_direct | 缓存命中率对比、线程安全测试 | ✅ 已完成 |
| P4 异步预取 | 第 4-5 周 | 接入 io_uring，完成 batch submit 和拓扑预取 | SSD 主路径 QPS、延迟统计 | ✅ 已完成 |
| P5 动态写入 | 第 5-6 周 | 完成 MemTable、WAL、SSTable、Compaction 和增量插入 | 混合读写 benchmark | ✅ 已完成 |
| P6 调优与文档 | 第 6-7 周 | 参数调优、SIFT 大规模测试、报告和答辩材料 | README、架构文档、实验报告、演示脚本 | 🔄 进行中 |
| P7 学习型预取 | 第 7+ 周 | XGBoost 预取排序、early trigger、离线 replay、在线 A/B | `PrefetchPlanner`、trace、模型训练脚本、预取统计（详见 `docs/design/XGBOOST_PREFETCH_PLAN.md` 和 `docs/design/07-prefetch-early-trigger-plan.md`） | ✅ 已完成闭环 |
| P8 I/O 尾延迟优化 | 后续 | 量化各阶段 I/O 放大、候选裁剪、布局与缓存治理、自适应预算 | reads/query、bytes/query、P99 降低（详见 `docs/roadmap/recall-to-io-tail-latency-plan.md`） | 🔜 待推进 |
| P9 答辩补强 | 答辩前 | 参数矩阵曲线、真实 NVMe 复测、delta 退化治理 | 矩阵实验、环境报告、delta 优化（详见 `docs/roadmap/defense-gap-closure-plan.md`） | 🔜 待推进 |

### 8.2 任务分解

| 任务 | 负责人建议 | 输入 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| 赛题与竞品分析 | 文档/算法成员 | `docs/competition/problem.md`、DiskANN、Milvus 资料 | 背景与创新点说明 | 评审指标映射完整 |
| PQ 编码器 | 算法成员 | 训练向量、维度配置 | PQ codes、codebooks、ADC 表 | Recall 不因过滤低于目标 |
| Vamana 图索引 | 算法成员 | 全精度向量 | 图邻接表、entry point | 构建成功，搜索可达 |
| SSD 布局 | 系统成员 | 向量、图、PQ codes | 4KB 对齐记录文件 | O_DIRECT 读写成功 |
| BufferPool | 系统成员 | 访问序列、入度信息 | Graph-Aware 2Q 缓存 | 命中率优于 LRU |
| io_uring 引擎 | 系统成员 | 批量读请求 | 异步提交与等待接口 | 与同步路径结果一致 |
| LSM 写路径 | 存储成员 | 插入/更新请求 | WAL、MemTable、SSTable | 读回正确，后台 flush 可用 |
| 混合负载 benchmark | 测试成员 | 查询集、写入比例 | QPS、延迟、Recall、内存报告 | 可复现实验输出 |
| 文档与答辩 | 全体 | 实验结果、架构图 | 报告、PPT、演示脚本 | 评审关注点覆盖完整 |

## 9. 关键参数计划

| 参数 | 默认值 | 调优范围 | 影响 |
| --- | ---: | --- | --- |
| dimension | 128 | 随数据集 | 向量维度 |
| k | 10 | 1-100 | Top-K 返回规模 |
| max_degree | 16 | 16-64 | 图质量、内存占用、搜索分支 |
| ef_construction | 200 | 100-500 | 构图质量与构建时间 |
| ef_search | 350 | 100-800 | Recall、延迟和 I/O 次数 |
| pq_m | 8 | 8-16 | PQ 压缩率和近似精度 |
| pq_k | 256 | 128-256 | codebook 精度和训练成本 |
| beam_width | 8 | 8-32 | 批量读取深度和 I/O 并行度 |
| cache_capacity | auto | 5%-15% dataset | 命中率和内存预算 |
| queue_depth | 128 | 32-512 | io_uring 并发深度 |
| mixed_writes | 500 | 按场景变化 | 写入压力 |
| mixed_duration_ms | 5000 | 5s-60s | 混合负载稳定性 |

## 10. 测试与验收方案

### 10.1 数据集

| 数据集 | 用途 | 说明 |
| --- | --- | --- |
| SIFT 10K | 快速回归 | 本地快速验证 Recall、QPS、缓存命中率 |
| SIFT 100K | 中等规模评估 | 观察图规模增大后的 I/O 行为 |
| SIFT 1M | 赛题主验证 | 验证 20% 内存约束下的整体效果 |
| Synthetic | 稳定开发 | SIFT 不存在时自动回退，用于 CI 和功能测试 |

### 10.2 功能测试

| 测试项 | 内容 |
| --- | --- |
| PQ 单元测试 | train、encode、decode、ADC table、内存统计 |
| 图索引测试 | 构建、搜索、增量插入、robust_prune、entry point |
| 磁盘布局测试 | pack/unpack、4KB 对齐、O_DIRECT 读写、offset 计算 |
| BufferPool 测试 | 命中、淘汰、入度保护、pin/unpin、多线程访问 |
| io_uring 测试 | batch submit、wait、fallback、一致性 |
| LSM 测试 | insert、update、delete、get、flush、SSTable 查找、Compaction |
| Benchmark 测试 | SSD 模式、内存对照模式、混合读写模式 |

### 10.3 性能测试

| 场景 | 指标 |
| --- | --- |
| SSD 单线程查询 | Recall@10、QPS、avg、P95、P99、缓存命中率、I/O 次数 |
| SSD 多线程查询 | QPS、尾延迟、锁竞争、BufferPool 安全性 |
| 内存对照查询 | 算法性能上限、Recall 一致性 |
| PQ 过滤开关对比 | SSD 读取次数、Recall 损失、QPS 增益 |
| BufferPool 策略对比 | Graph-Aware 2Q vs LRU 命中率与延迟 |
| io_uring 开关对比 | 同步读 vs 异步批量读的吞吐和尾延迟 |
| 混合读写负载 | QPS、P99/P99.9、写入吞吐、Compaction 影响 |

### 10.4 验收命令

```bash
# 构建
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 单元测试
./agent_aware_tests

# P5 混合读写 benchmark
./bench_mixed_rw --num_operations 10000 --topk 10
./bench_mixed_rw --num_operations 100000 --topk 10

# 自定义读写比例
./bench_mixed_rw --num_operations 10000 --read_ratio 0.95 --write_ratio 0.05

# SIFT1M 主路径脚本入口
cd ..
bash scripts/linux/run_sift1m_once.sh
```

### 10.5 最终验收标准

| 类别 | 标准 |
| --- | --- |
| 合规性 | SSD 主路径内存占用不超过数据集大小 20% |
| 准确性 | Recall@10 不低于 85%，目标达到 98% 以上 |
| I/O 优化 | 能证明 O_DIRECT、BufferPool、PQ 过滤、io_uring 各自产生收益 |
| 动态写入 | 支持并发插入/更新，写入后可被查询路径访问 |
| 稳定性 | 混合读写下无崩溃，P99/P99.9 有统计且无严重抖动 |
| 可复现 | README 和脚本可复现实验结果 |
| 文档 | 架构、实现、测试、实验报告齐全 |

## 11. 风险与应对

| 风险 | 影响 | 应对方案 |
| --- | --- | --- |
| WSL2 或虚拟化环境 I/O 延迟偏高 | SSD 模式 QPS 偏低 | 明确标注环境限制，在真实 NVMe 上复测；报告中同时给出 I/O 次数和缓存命中率 |
| io_uring 多线程共享 buffer 不安全 | use-after-free 或数据竞争 | 单线程使用 io_uring 完整链路，多线程使用同步 BufferPool 读并通过 pin 保护 |
| PQ 过滤阈值过激 | Recall 下降 | 以 Recall 为硬约束，动态调高 ef_search、beam_width 或阈值 |
| BufferPool 过大突破内存预算 | 赛题不合规 | cache_capacity 根据数据集大小自动计算，输出 Memory Report |
| Compaction 影响查询尾延迟 | 混合负载 P99 上升 | 后台限速、批量合并、查询优先级高于 Compaction |
| 增量插入破坏图连通性 | 新向量召回下降 | 使用 robust_prune、反向边和 re-prune，定期抽样验证可达性 |
| SIFT 数据下载失败 | 无法完成真实数据验证 | 保留 synthetic fallback，但最终报告需补充真实数据结果 |
| 文档与代码不一致 | 答辩风险 | 每次性能调优后同步 README、架构文档和实验报告 |
| XGBoost 推理开销抵消预取收益 | CPU 开销过高 | 限制候选池大小、使用轻量 text-dump 模型、批量推理 |
| 预取 cache pollution | 高分但低复用 page 污染 BufferPool | cache pressure guard、unused 反馈降级、限流阈值 |
| 模型数据泄漏 | 训练集与测试集 query 重叠 | 按 query_id 切分 train/validation/test |
| 预取触发太晚 | ready_hit 低于 pending_hit | early trigger 提前触发点、trace lead_steps 诊断 |
| Delta 规模增长导致 linear scan 退化 | 大 delta 下查询 P95 上升 | delta memory graph 或分块 immutable delta，设置 rebuild 阈值 |

## 12. 创新点总结

| 创新点 | 价值 |
| --- | --- |
| Graph-Aware 2Q 缓存 | 将图入度引入缓存淘汰，保护 hub 节点，提升随机图遍历缓存命中率 |
| PQ ADC 与 SSD 读取协同 | 在内存中用 8 字节 PQ code 预判候选价值，减少全精度 SSD 访问 |
| 4KB 对齐零拷贝距离计算 | O_DIRECT 记录布局与 SIMD 对齐结合，命中缓存时直接从 page buffer 计算 |
| 拓扑感知 io_uring 预取 | 根据图邻居关系预取下一跳，使 CPU 计算与 SSD I/O 重叠 |
| LSM 写入与图增量插入结合 | 同时解决实时写入持久化和图索引可检索问题 |
| Immutable Read View 无锁读 | 通过 atomic `shared_ptr<const DynamicReadView>` 发布读快照，消除 reader 与 writer/flush/compaction 的锁竞争，`latest_record_lookup` P95 从秒级降至约 0.07ms |
| XGBoost 学习型预取排序 | 将预取建模为 page-level ranking 问题，通过 PQ/图结构/缓存/I/O 多维特征 + XGBoost 排序，在限流下提高 ready_hit、降低 pending_hit 和 unused |
| Early trigger 提前触发 | 在 beam selection 前提前触发预取，解决预取"发得太晚"的核心瓶颈，提高 ready_before_demand 比例 |
| 合规双轨评测 | SSD 路径作为赛题主路径，内存路径仅作性能上限对照，避免混淆评审指标 |

## 13. 交付物清单

| 交付物 | 内容 |
| --- | --- |
| 源代码 | core、io、buffer、compaction、engine、data、benchmark、tests |
| 构建脚本 | CMakeLists、依赖说明、Release/ASAN 构建方式 |
| 测试程序 | 单元测试、SSD benchmark、内存对照 benchmark、混合负载 benchmark |
| 数据脚本 | SIFT1M 下载脚本、合成数据 fallback |
| 技术文档 | README、项目计划书、赛题说明、评分映射、阶段计划、版本说明、SIFT1M 实验报告 |
| 答辩材料 | 方案介绍、创新点、性能图表、风险说明、演示流程 |

## 14. 评审指标映射

| 评审项 | 权重 | 本项目响应 |
| --- | ---: | --- |
| 性能指标 | 25% | 提供 Recall@10、QPS、avg/P99/P99.9、缓存命中率、内存占比、混合负载统计 |
| 创新性 | 25% | Graph-Aware 2Q、PQ ADC 与 SSD 读取协同、io_uring 拓扑预取、LSM 动态写入 |
| 代码质量 | 25% | 分层模块、统一 Error、线程安全保护、同步 fallback、可复现实验 |
| 文档完整性 | 25% | 从赛题需求、架构、实现、测试到实验报告形成完整闭环 |

## 15. 结论

agent-aware 的计划重点是围绕赛题的真实约束构建系统，而不是绕开内存限制追求纯内存性能。通过“PQ 压缩导航 + SSD 全精度向量 + 用户态图感知缓存 + io_uring 异步预取 + LSM 顺序写入”的组合，系统能够在 10%-20% 内存预算下保持高召回率，并对高并发读写混合负载提供可解释、可复现、可调优的 I/O 优化能力。

后续工作应集中在三点：一是扩大 SIFT1M 与真实 NVMe 环境测试，二是完善 Compaction 与图连通性的一致性验证，三是将实验结果整理为图表化报告和答辩演示材料。

## 16. 专项计划文档索引

本项目的详细设计和路线图分散在以下专项计划文档中，与本文档形成"总-分"关系：

| 文档 | 主题 | 状态 |
| --- | --- | --- |
| `docs/design/XGBOOST_PREFETCH_PLAN.md` | 学习型预取排序：PQ+ADC 候选聚合、page-level 特征、XGBoost ranking、trace 采集、离线 replay、在线 A/B、评价体系 | 已落地闭环 |
| `docs/design/07-prefetch-early-trigger-plan.md` | 预取提前触发点：pre-beam / post-expand / entry warmup / rerank lookahead、两阶段预取、trace 字段、限流规则 | P0（pre-beam）已落地 |
| `docs/roadmap/recall-to-io-tail-latency-plan.md` | I/O 放大与尾延迟优化：候选裁剪、数据布局、缓存治理、自适应预算、P99/P999 治理、分阶段排期 | 待推进 |
| `docs/roadmap/defense-gap-closure-plan.md` | 答辩前补强：参数矩阵曲线、真实 NVMe/比赛环境复测、delta 增长压力测试、delta 查询优化路线 | 待推进 |
| `docs/design/ssd-storage-path.md` | SSD 4KB record 布局、DiskIndexReader/Writer、O_DIRECT、packed graph 文件格式 | 已落地 |
| `docs/design/cache-zero-copy.md` | Graph-Aware 2Q BufferPool、同页复用、compute_distance_direct | 已落地 |
| `docs/design/async-prefetch.md` | AsyncPageReader、QueryPageSession、io_uring fallback、PQ+ADC/XGBoost 预取底座 | 已落地 |
| `docs/design/dynamic-write.md` | LSM 写入路径：WAL/MemTable/SSTable/manifest/compaction/recovery | 已落地 |
| `docs/roadmap/dynamic-write-task-breakdown.md` | 动态写入任务拆分与验收清单 | 大部分已完成 |
| `docs/design/fresh-streaming-ann.md` | Streaming ANN 创新路线：LSM delta + immutable read view、Delta memory graph、周期性 rebuild | 部分已完成 |
