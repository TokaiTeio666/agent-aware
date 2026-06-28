# 06 Fresh Streaming ANN Index 创新点设计文档

## 当前实现状态（2026-06-21）

本计划的“动态层可查询”已经通过 LSM delta + base/delta merge + immutable read view 形成最小闭环：新写入向量进入动态层，查询时按 `read_sequence` 合并可见记录，update/delete 能覆盖 base 结果，并可通过动态 Recall 抽样验证。尚未实现完整 FreshDiskANN 式 Delta memory graph、delete consolidation 和周期性 BatchMerge/rebuild packed graph。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| Delta 可查询层 | 已完成可交付版 | `DynamicWriteManager::search_delta_l2_at` |
| DeleteList/tombstone | 已完成基础语义 | `erase` 记录和 base/delta merge 覆盖 |
| HybridSearcher | 已完成基础语义 | `PackedGraphEngine` 查询 base 后合并 dynamic manager |
| BatchMerge/周期性 rebuild | 未完成 | 后续优化将 delta 批量吸收到 packed base |
| Delta memory graph | 未完成 | 当前 delta 仍以 L2 扫描为主 |

## 设计背景

本项目原有方向主要集中在基于 DiskANN/Vamana 的静态图索引优化，包括 SSD 图索引、PQ 压缩、缓存策略、io_uring 异步 I/O、beam batching 等。为了进一步提升项目创新性，计划引入面向流式数据的增量 ANN 索引机制，参考 FreshDiskANN 的核心思想，设计并实现一个简化版的动态更新层，使系统不仅能支持静态向量检索，还能支持新向量的持续插入、逻辑删除和周期性合并。

现有 DiskANN/Vamana 类索引通常适合静态数据集。索引构建完成后，搜索性能较高，但如果数据持续变化，会遇到以下问题：

1. 新数据无法及时被搜索到；如果每次新增向量都重新构建完整图索引，成本过高。
2. 直接修改 SSD 图索引代价大；图索引插入一个点时，不仅要给新点建立邻居，还可能需要修改多个已有节点的反向边。
3. 删除操作会破坏图连通性；如果直接删除节点，图中的边可能指向不存在的节点，影响搜索路径和召回率。
4. 静态索引难以适应流式场景；日志检索、推荐系统、向量数据库、Agent Memory 等场景中，数据经常持续写入。

## 核心目标

Fresh Streaming ANN Layer 的核心目标是：

```text
在不频繁重建整个 SSD 图索引的前提下，
支持流式插入、懒删除和查询时多索引结果合并，
从而提升系统面对动态数据场景时的可用性和工程完整性。
```

完成该创新点后，系统应具备：

1. 支持流式插入：新增向量无需重建整个 SSD 索引，即可被查询到。
2. 支持懒删除：删除点不会出现在最终查询结果中，且删除操作开销较低。
3. 支持混合查询：查询结果可以同时来自长期 SSD 索引和内存增量索引。
4. 支持周期性合并：增量索引达到阈值后可以批量合并回长期索引。
5. 具备实验可展示性：可以通过 SIFT100K/SIFT1M 展示静态索引和动态索引的性能差异。

## 相关实现文件

建议新增或整理如下文件：

```text
include/dynamic/delta_graph_index.h
include/dynamic/delete_list.h
include/dynamic/hybrid_searcher.h
include/dynamic/merge_policy.h
src/dynamic/delta_graph_index.cpp
src/dynamic/delete_list.cpp
src/dynamic/hybrid_searcher.cpp
src/dynamic/merge_policy.cpp
src/streaming_ann_bench.cpp
```

各模块职责：

```text
delta_graph_index      负责内存增量图索引
delete_list            负责懒删除标记
hybrid_searcher        负责 Base + Delta 结果合并
merge_policy           负责合并触发策略
streaming_ann_bench    负责流式插入/删除/查询实验
```

## 具体设计要求

## 一、整体架构

整体架构分为三层：

```text
查询请求
   │
   ├── 搜索 BaseIndex：SSD 上的长期图索引
   ├── 搜索 DeltaIndex：内存中的增量图索引
   ├── 过滤 DeleteList：删除标记表
   │
   └── 合并 TopK 结果
```

其中：

1. **BaseIndex**：原有的 SSD 静态图索引，负责存储大规模历史数据。
2. **DeltaIndex**：新增的内存级增量图索引，负责接收新插入的向量。
3. **DeleteList**：删除标记表，用于记录逻辑删除的节点。
4. **Merge Worker**：后台合并模块，将 DeltaIndex 中的数据周期性合并回 BaseIndex。

该设计避免了每次插入都直接修改 SSD 上的大图索引，而是先将更新写入内存增量层，查询时同时搜索 BaseIndex 和 DeltaIndex，最后再进行结果合并。

## 二、创新点定位

本创新点可以命名为：

```text
Fresh Streaming ANN Layer for Disk-based Vamana Index
```

也可以在论文/报告中描述为：

```text
本项目在静态 DiskANN/Vamana 索引的基础上，引入轻量级流式更新层，
通过内存增量图索引接收新插入向量，通过 DeleteList 实现懒删除，
并在查询阶段融合长期 SSD 索引和增量索引的搜索结果。
相比传统静态 ANN 索引，该设计避免了频繁重建整个图索引，
使系统能够适应持续写入和在线更新的应用场景，
提升了项目在向量数据库、Agent Memory 和在线推荐系统中的工程实用价值。
```

## 三、DeltaIndex 内存增量图索引

DeltaIndex 用于存储新插入的数据点。它可以复用现有 VamanaBuilder 的核心逻辑，但只在内存中维护图结构。

主要功能：

1. 支持 `insert(vector, id)`。
2. 为新点执行贪心搜索，找到候选邻居。
3. 使用 RobustPrune 控制出边数量。
4. 将新点加入邻居的反向连接。
5. 当节点度数超过限制时重新 prune。

建议数据结构：

```cpp
struct DeltaNode {
    NodeId id;
    std::vector<float> vector;
    std::vector<NodeId> neighbors;
};

class DeltaGraphIndex {
public:
    void insert(NodeId id, Span<float> vector);
    SearchResult search(Span<float> query, SearchConfig config) const;
    size_t size() const;
    void clear();
};
```

## 四、DeleteList 懒删除机制

DeleteList 用于记录已经被删除但尚未从图结构中物理移除的节点。

主要功能：

1. 支持 `mark_deleted(NodeId id)`。
2. 查询结果返回前过滤已删除节点。
3. 图搜索过程中可以允许删除点参与导航，但不允许进入最终 TopK。
4. 合并步骤再统一清理删除点。

建议数据结构：

```cpp
class DeleteList {
public:
    void mark_deleted(NodeId id);
    bool is_deleted(NodeId id) const;
    size_t size() const;
};
```

设计理由：

1. 避免每次删除都立即修改图结构。
2. 降低随机写和图修复成本。
3. 适合批量清理和周期性合并。

## 五、HybridSearcher 查询合并

查询时需要同时搜索 BaseIndex 和 DeltaIndex，然后合并结果。

流程如下：

```text
1. 在 BaseIndex 中搜索 query，得到 base_topk
2. 在 DeltaIndex 中搜索 query，得到 delta_topk
3. 合并两个结果集合
4. 使用 DeleteList 过滤被删除节点
5. 按距离排序，返回最终 TopK
```

接口设计：

```cpp
class HybridSearcher {
public:
    SearchResult search(
        Span<float> query,
        size_t topk,
        const SearchConfig& config
    );
};
```

合并逻辑：

```text
merged = base_results + delta_results
remove ids in DeleteList
sort by distance
return topK
```

该模块是最容易优先实现、也最容易展示效果的部分。

## 六、BatchMerge 后台合并

当 DeltaIndex 中的数据达到一定规模后，需要将其合并进 BaseIndex。

初始版本不需要完整实现 FreshDiskANN 的 StreamingMerge，可以先实现简化版：

```text
1. 收集 DeltaIndex 中所有新增点
2. 读取 BaseIndex 元信息
3. 重新构建一个新的 BaseIndex
4. 用新 BaseIndex 替换旧 BaseIndex
5. 清空 DeltaIndex 和部分 DeleteList
```

虽然这不是最高效的在线合并方式，但工程上更容易实现，适合作为项目迭代实验。

后续可以进一步优化为：

```text
简化重建式合并
        ↓
批量插入式合并
        ↓
Block-level StreamingMerge
```

## 七、实现顺序

步骤一：完成查询合并框架。

1. 保留现有 SSD BaseIndex。
2. 新增一个简单 DeltaIndex。
3. 查询时同时搜索 BaseIndex 和 DeltaIndex。
4. 合并 TopK 结果。
5. 新建 `delta_graph_index` 与 `hybrid_searcher`。
6. 添加 `enable_delta_index` 与 `delta_insert_limit` 参数。
7. 输出 base/delta 命中数量统计。

验收要求：

1. 插入到 DeltaIndex 的新点可以被搜索到。
2. BaseIndex 原有搜索不受破坏。
3. 查询结果中可以同时出现 base 和 delta 中的数据点。

步骤二：实现内存 FreshVamana 插入。

1. DeltaIndex 不再使用简单线性扫描，改为内存图索引。
2. 复用 Vamana 的 GreedySearch。
3. 复用 RobustPrune。
4. 实现新点插入逻辑。
5. 实现邻居反向边更新。
6. 控制最大出度 `R`。
7. 支持 DeltaIndex 内部搜索。

建议参数：

```text
delta_R = 32 或 64
delta_L = 64 或 128
delta_alpha = 1.2
```

验收要求：

1. DeltaIndex 搜索速度明显优于线性扫描。
2. 新增点插入后可以通过图搜索被召回。
3. 不同 `R`、`L`、`alpha` 下可以输出 recall/latency 对比。

步骤三：实现 DeleteList 懒删除。

1. 新建 `delete_list`。
2. 支持 `mark_deleted(id)`。
3. 支持 `is_deleted(id)`。
4. 查询结果过滤删除点。
5. 统计删除点数量。
6. 支持按删除比例模拟删除。

验收要求：

1. 被删除的点不会出现在最终 TopK 中。
2. 搜索过程仍然可以正常执行。
3. 删除比例升高时能观察到 recall/latency 变化。

步骤四：实现简化 BatchMerge。

1. 添加 `MergePolicy`。
2. 支持按数量触发合并。
3. 支持按删除比例触发合并。
4. 实现重建式 merge。
5. merge 后清空 DeltaIndex。
6. merge 后更新 DeleteList 状态。

建议触发条件：

```text
delta_size >= 10000
delete_ratio >= 0.05
```

验收要求：

1. merge 前新点位于 DeltaIndex。
2. merge 后新点进入 BaseIndex。
3. merge 前后查询结果基本一致。
4. merge 过程有明确日志记录。

步骤五：实验评估。

1. 使用 SIFT100K 做快速功能验证。
2. 使用 SIFT1M 做主要性能测试。
3. 对比静态索引、线性 Delta、图 Delta、DeleteList、周期性 Merge。
4. 记录 Recall@K、QPS、Avg/P95 Latency、Insert/Delete Throughput、Delta Size、Merge Time、Memory Usage。

## 八、关键参数

| 参数 | 含义 | 建议值 |
| --- | --- | --- |
| `base_R` | BaseIndex 图最大度数 | 32 或 64 |
| `delta_R` | DeltaIndex 图最大度数 | 32 或 64 |
| `search_width` | BaseIndex 搜索宽度 | 128 / 256 / 512 |
| `delta_search_width` | DeltaIndex 搜索宽度 | 64 / 128 |
| `alpha` | RobustPrune 参数 | 1.2 |
| `delta_limit` | DeltaIndex 最大容量 | 10K / 50K / 100K |
| `merge_trigger_ratio` | 合并触发比例 | 5% |
| `delete_trigger_ratio` | 删除清理触发比例 | 5% |
| `topk` | 返回近邻数量 | 10 |

## 九、实验设计

实验组设计：

| 实验组 | 说明 |
| --- | --- |
| Static BaseIndex | 只使用原始静态索引 |
| BaseIndex + Linear Delta | 增量层使用线性扫描 |
| BaseIndex + Graph Delta | 增量层使用内存图索引 |
| BaseIndex + Graph Delta + DeleteList | 加入懒删除机制 |
| BaseIndex + Graph Delta + Merge | 加入周期性合并 |

评估指标：

| 指标 | 含义 |
| --- | --- |
| Recall@K | 搜索准确率 |
| QPS | 每秒查询数 |
| Avg Latency | 平均查询延迟 |
| P95 Latency | 95 分位延迟 |
| Insert Throughput | 插入吞吐 |
| Delete Throughput | 删除吞吐 |
| Delta Size | 增量索引规模 |
| Merge Time | 合并耗时 |
| Memory Usage | 内存占用 |

## 十、风险与简化策略

完整 StreamingMerge 实现难度高：

```text
第一版使用重建式 BatchMerge
后续再逐步改成 block-level merge
```

DeltaIndex 过大会影响内存：

```text
设置 delta_limit
达到阈值后触发 merge
或者仅在 SIFT100K/SIFT1M 实验中模拟有限更新流
```

删除比例过高会影响图搜索质量：

```text
设置 delete_trigger_ratio
超过阈值后触发 consolidate 或 merge
```

## 十一、最小可交付版本与后续增强

如果时间有限，建议优先完成以下最小版本：

```text
1. BaseIndex + DeltaIndex 双索引查询
2. DeltaIndex 先用线性扫描实现
3. DeleteList 过滤最终结果
4. 查询结果合并
5. SIFT100K 上完成插入/删除/查询实验
```

最小版本也可以形成完整创新点：

```text
在静态 SSD ANN 索引之外增加轻量级内存增量层，
使系统具备基础流式更新能力。
```

后续可以继续扩展：

1. DeltaIndex 从线性扫描升级为 FreshVamana 图索引。
2. BatchMerge 从重建式合并升级为局部图合并。
3. 加入 WAL/Redo Log，支持崩溃恢复。
4. 查询时根据 DeltaIndex 大小自适应分配 search_width。
5. 加入冷热分层，将近期数据保留在内存，历史数据放入 SSD。
6. 与 PQ 压缩结合，降低 DeltaIndex 内存占用。
7. 与 graph-aware cache 结合，缓存 BaseIndex 热点页面。

## 验收标准

最终实现应满足：

1. 新增向量无需重建整个 SSD 索引即可被查询到。
2. 删除点不会出现在最终查询结果中。
3. 查询结果可以同时来自长期 SSD 索引和内存增量索引。
4. 增量索引达到阈值后可以批量合并回长期索引。
5. 插入到 DeltaIndex 的新点可以被搜索到。
6. BaseIndex 原有搜索不受破坏。
7. DeltaIndex 搜索速度明显优于线性扫描。
8. 新增点插入后可以通过图搜索被召回。
9. merge 前后查询结果基本一致。
10. 能通过 SIFT100K/SIFT1M 展示静态索引和动态索引的性能差异。
11. 报告中清晰说明 Fresh Streaming ANN Layer 的创新性和工程价值。
