# Fresh Streaming ANN Index 创新点实现计划

## 当前实现状态（2026-06-21）

本计划的“动态层可查询”已经通过 LSM delta + base/delta merge + immutable read view 形成最小闭环：新写入向量进入动态层，查询时按 `read_sequence` 合并可见记录，update/delete 能覆盖 base 结果，并可通过动态 Recall 抽样验证。尚未实现完整 FreshDiskANN 式 Delta memory graph、delete consolidation 和周期性 BatchMerge/rebuild packed graph。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| Delta 可查询层 | 已完成可交付版 | `DynamicWriteManager::search_delta_l2_at` |
| DeleteList/tombstone | 已完成基础语义 | `erase` 记录和 base/delta merge 覆盖 |
| HybridSearcher | 已完成基础语义 | `PackedGraphEngine` 查询 base 后合并 dynamic manager |
| BatchMerge/周期性 rebuild | 未完成 | 下一阶段将 delta 批量吸收到 packed base |
| Delta memory graph | 未完成 | 当前 delta 仍以 L2 扫描为主 |

## 1. 创新点概述

本项目原有方向主要集中在基于 DiskANN/Vamana 的静态图索引优化，包括 SSD 图索引、PQ 压缩、缓存策略、io_uring 异步 I/O、beam batching 等。为了进一步提升项目创新性，计划引入 **面向流式数据的增量 ANN 索引机制**，参考 FreshDiskANN 的核心思想，设计并实现一个简化版的动态更新层，使系统不仅能支持静态向量检索，还能支持新向量的持续插入、逻辑删除和周期性合并。

该创新点的核心目标是：

> 在不频繁重建整个 SSD 图索引的前提下，支持流式插入、懒删除和查询时多索引结果合并，从而提升系统面对动态数据场景时的可用性和工程完整性。

---

## 2. 背景与问题

现有 DiskANN/Vamana 类索引通常适合静态数据集。索引构建完成后，搜索性能较高，但如果数据持续变化，会遇到以下问题：

1. **新数据无法及时被搜索到**  
   如果每次新增向量都重新构建完整图索引，成本过高。

2. **直接修改 SSD 图索引代价大**  
   图索引插入一个点时，不仅要给新点建立邻居，还可能需要修改多个已有节点的反向边。如果这些节点分布在 SSD 的不同页面中，会造成大量随机写。

3. **删除操作会破坏图连通性**  
   如果直接删除节点，图中的边可能指向不存在的节点，影响搜索路径和召回率。

4. **静态索引难以适应流式场景**  
   在日志检索、推荐系统、向量数据库、Agent Memory 等场景中，数据经常持续写入，完全静态的 ANN 索引不够灵活。

因此，本项目计划设计一个轻量级的 **Fresh Streaming ANN Index**，在现有 SSD 图索引之上增加动态更新能力。

---

## 3. 总体设计思路

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

- **BaseIndex**：原有的 SSD 静态图索引，负责存储大规模历史数据。
- **DeltaIndex**：新增的内存级增量图索引，负责接收新插入的向量。
- **DeleteList**：删除标记表，用于记录逻辑删除的节点。
- **Merge Worker**：后台合并模块，将 DeltaIndex 中的数据周期性合并回 BaseIndex。

该设计避免了每次插入都直接修改 SSD 上的大图索引，而是先将更新写入内存增量层，查询时同时搜索 BaseIndex 和 DeltaIndex，最后再进行结果合并。

---

## 4. 创新点定位

本创新点可以命名为：

> **Fresh Streaming ANN Layer for Disk-based Vamana Index**

也可以在论文/报告中描述为：

> 本项目在静态 DiskANN/Vamana 索引的基础上，引入轻量级流式更新层，通过内存增量图索引接收新插入向量，通过 DeleteList 实现懒删除，并在查询阶段融合长期 SSD 索引和增量索引的搜索结果。相比传统静态 ANN 索引，该设计避免了频繁重建整个图索引，使系统能够适应持续写入和在线更新的应用场景，提升了项目在向量数据库、Agent Memory 和在线推荐系统中的工程实用价值。

---

## 5. 模块划分

### 5.1 DeltaIndex：内存增量图索引

DeltaIndex 用于存储新插入的数据点。它可以复用现有 VamanaBuilder 的核心逻辑，但只在内存中维护图结构。

主要功能：

- 支持 `insert(vector, id)`；
- 为新点执行贪心搜索，找到候选邻居；
- 使用 RobustPrune 控制出边数量；
- 将新点加入邻居的反向连接；
- 当节点度数超过限制时重新 prune。

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

---

### 5.2 DeleteList：懒删除机制

DeleteList 用于记录已经被删除但尚未从图结构中物理移除的节点。

主要功能：

- 支持 `mark_deleted(NodeId id)`；
- 查询结果返回前过滤已删除节点；
- 图搜索过程中可以允许删除点参与导航，但不允许进入最终 TopK；
- 合并阶段再统一清理删除点。

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

- 避免每次删除都立即修改图结构；
- 降低随机写和图修复成本；
- 适合批量清理和周期性合并。

---

### 5.3 HybridSearcher：多索引查询合并

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

该模块是第一阶段最容易实现、也最容易展示效果的部分。

---

### 5.4 BatchMerge：简化版后台合并

当 DeltaIndex 中的数据达到一定规模后，需要将其合并进 BaseIndex。

第一阶段不需要完整实现 FreshDiskANN 的 StreamingMerge，可以先实现简化版：

```text
1. 收集 DeltaIndex 中所有新增点
2. 读取 BaseIndex 元信息
3. 重新构建一个新的 BaseIndex
4. 用新 BaseIndex 替换旧 BaseIndex
5. 清空 DeltaIndex 和部分 DeleteList
```

虽然这不是最高效的在线合并方式，但工程上更容易实现，适合课程项目和阶段性实验。

后续可以进一步优化为：

```text
简化重建式合并
        ↓
批量插入式合并
        ↓
Block-level StreamingMerge
```

---

## 6. 实现阶段安排

### 阶段一：完成查询合并框架

目标：

- 保留现有 SSD BaseIndex；
- 新增一个简单 DeltaIndex；
- 查询时同时搜索 BaseIndex 和 DeltaIndex；
- 合并 TopK 结果。

任务：

- [ ] 新建 `delta_graph_index.h/.cpp`
- [ ] 新建 `hybrid_searcher.h/.cpp`
- [ ] 添加 `--enable_delta_index` 参数
- [ ] 添加 `--delta_insert_limit` 参数
- [ ] 实现 BaseIndex + DeltaIndex 的结果合并
- [ ] 输出 base/delta 命中数量统计

验收标准：

- 插入到 DeltaIndex 的新点可以被搜索到；
- BaseIndex 原有搜索不受破坏；
- 查询结果中可以同时出现 base 和 delta 中的数据点。

---

### 阶段二：实现内存 FreshVamana 插入

目标：

- DeltaIndex 不再使用简单线性扫描；
- 改为内存图索引；
- 支持新点增量插入。

任务：

- [ ] 复用 Vamana 的 GreedySearch
- [ ] 复用 RobustPrune
- [ ] 实现新点插入逻辑
- [ ] 实现邻居反向边更新
- [ ] 控制最大出度 `R`
- [ ] 支持 DeltaIndex 内部搜索

建议参数：

```text
delta_R = 32 或 64
delta_L = 64 或 128
delta_alpha = 1.2
```

验收标准：

- DeltaIndex 搜索速度明显优于线性扫描；
- 新增点插入后可以通过图搜索被召回；
- 不同 `R`、`L`、`alpha` 下可以输出 recall/latency 对比。

---

### 阶段三：实现 DeleteList 懒删除

目标：

- 支持逻辑删除；
- 查询结果中过滤删除点；
- 不立即修改 SSD 图结构。

任务：

- [ ] 新建 `delete_list.h/.cpp`
- [ ] 支持 `mark_deleted(id)`
- [ ] 支持 `is_deleted(id)`
- [ ] 查询结果过滤删除点
- [ ] 统计删除点数量
- [ ] 添加 `--delete_ratio` 或测试脚本模拟删除

验收标准：

- 被删除的点不会出现在最终 TopK 中；
- 搜索过程仍然可以正常执行；
- 删除比例升高时能观察到 recall/latency 变化。

---

### 阶段四：实现简化 BatchMerge

目标：

- 当 DeltaIndex 达到阈值后，将其合并回 BaseIndex；
- 避免 DeltaIndex 无限增长；
- 为后续 StreamingMerge 留接口。

任务：

- [ ] 添加 `MergePolicy`
- [ ] 支持按数量触发合并
- [ ] 支持按删除比例触发合并
- [ ] 实现重建式 merge
- [ ] merge 后清空 DeltaIndex
- [ ] merge 后更新 DeleteList 状态

建议触发条件：

```text
delta_size >= 10000
delete_ratio >= 0.05
```

验收标准：

- merge 前新点位于 DeltaIndex；
- merge 后新点进入 BaseIndex；
- merge 前后查询结果基本一致；
- merge 过程有明确日志记录。

---

### 阶段五：实验评估

目标：

验证动态更新机制的有效性。

实验数据集：

```text
SIFT100K：快速功能验证
SIFT1M：主要性能测试
```

实验组设计：

| 实验组 | 说明 |
|---|---|
| Static BaseIndex | 只使用原始静态索引 |
| BaseIndex + Linear Delta | 增量层使用线性扫描 |
| BaseIndex + Graph Delta | 增量层使用内存图索引 |
| BaseIndex + Graph Delta + DeleteList | 加入懒删除机制 |
| BaseIndex + Graph Delta + Merge | 加入周期性合并 |

评估指标：

| 指标 | 含义 |
|---|---|
| Recall@K | 搜索准确率 |
| QPS | 每秒查询数 |
| Avg Latency | 平均查询延迟 |
| P95 Latency | 95 分位延迟 |
| Insert Throughput | 插入吞吐 |
| Delete Throughput | 删除吞吐 |
| Delta Size | 增量索引规模 |
| Merge Time | 合并耗时 |
| Memory Usage | 内存占用 |

---

## 7. 推荐代码结构

建议新增如下文件：

```text
include/agentmem/dynamic/
├── delta_graph_index.h
├── delete_list.h
├── hybrid_searcher.h
├── merge_policy.h

src/agent_aware/dynamic/
├── delta_graph_index.cpp
├── delete_list.cpp
├── hybrid_searcher.cpp
├── merge_policy.cpp

tools/benchmarks/
├── streaming_ann_bench.cpp
├── run_streaming_sift100k.sh
├── run_streaming_sift1m.sh
```

各模块职责：

```text
delta_graph_index      负责内存增量图索引
delete_list            负责懒删除标记
hybrid_searcher        负责 Base + Delta 结果合并
merge_policy           负责合并触发策略
streaming_ann_bench    负责流式插入/删除/查询实验
```

---

## 8. 关键参数设计

| 参数 | 含义 | 建议值 |
|---|---|---|
| `base_R` | BaseIndex 图最大度数 | 32 或 64 |
| `delta_R` | DeltaIndex 图最大度数 | 32 或 64 |
| `search_width` | BaseIndex 搜索宽度 | 128 / 256 / 512 |
| `delta_search_width` | DeltaIndex 搜索宽度 | 64 / 128 |
| `alpha` | RobustPrune 参数 | 1.2 |
| `delta_limit` | DeltaIndex 最大容量 | 10K / 50K / 100K |
| `merge_trigger_ratio` | 合并触发比例 | 5% |
| `delete_trigger_ratio` | 删除清理触发比例 | 5% |
| `topk` | 返回近邻数量 | 10 |

---

## 9. 预期结果

完成该创新点后，项目应具备以下能力：

1. **支持流式插入**  
   新增向量无需重建整个 SSD 索引，即可被查询到。

2. **支持懒删除**  
   删除点不会出现在最终查询结果中，且删除操作开销较低。

3. **支持混合查询**  
   查询结果可以同时来自长期 SSD 索引和内存增量索引。

4. **支持周期性合并**  
   增量索引达到阈值后可以批量合并回长期索引。

5. **具备实验可展示性**  
   可以通过 SIFT100K/SIFT1M 展示静态索引和动态索引的性能差异。

---

## 10. 项目报告中的创新性表述

可以在报告中写为：

> 本项目不仅实现了基于 Vamana/DiskANN 的 SSD 图索引检索，还进一步针对动态数据场景设计了 Fresh Streaming ANN Layer。该模块通过内存增量图索引接收新插入向量，通过 DeleteList 实现懒删除，并在查询阶段融合长期 SSD 索引和增量索引的搜索结果。相比传统静态 ANN 索引，该设计避免了频繁重建整个图索引，使系统能够适应持续写入和在线更新的应用场景，提升了项目在向量数据库、Agent Memory 和在线推荐系统中的工程实用价值。

---

## 11. 风险与简化策略

### 风险一：完整 StreamingMerge 实现难度高

完整 FreshDiskANN 的 StreamingMerge 需要处理 SSD block-level 顺序扫描、反向边 patch、并发查询一致性和崩溃恢复，工程复杂度较高。

简化策略：

```text
第一版使用重建式 BatchMerge
后续再逐步改成 block-level merge
```

---

### 风险二：DeltaIndex 过大会影响内存

如果新插入数据过多，内存中的 DeltaIndex 会持续膨胀。

简化策略：

```text
设置 delta_limit
达到阈值后触发 merge
或者仅在 SIFT100K/SIFT1M 实验中模拟有限更新流
```

---

### 风险三：删除比例过高会影响图搜索质量

懒删除点虽然不会进入最终结果，但仍可能参与导航。如果删除比例过高，搜索路径质量可能下降。

简化策略：

```text
设置 delete_trigger_ratio
超过阈值后触发 consolidate 或 merge
```

---

## 12. 最小可交付版本

如果时间有限，建议优先完成以下最小版本：

```text
1. BaseIndex + DeltaIndex 双索引查询
2. DeltaIndex 先用线性扫描实现
3. DeleteList 过滤最终结果
4. 查询结果合并
5. SIFT100K 上完成插入/删除/查询实验
```

最小版本也可以形成完整创新点：

> 在静态 SSD ANN 索引之外增加轻量级内存增量层，使系统具备基础流式更新能力。

---

## 13. 后续增强方向

后续可以继续扩展：

1. **DeltaIndex 从线性扫描升级为 FreshVamana 图索引**
2. **BatchMerge 从重建式合并升级为局部图合并**
3. **加入 WAL/Redo Log，支持崩溃恢复**
4. **查询时根据 DeltaIndex 大小自适应分配 search_width**
5. **加入冷热分层，将近期数据保留在内存，历史数据放入 SSD**
6. **与 PQ 压缩结合，降低 DeltaIndex 内存占用**
7. **与 graph-aware cache 结合，缓存 BaseIndex 热点页面**

---

## 14. 结论

Fresh Streaming ANN Layer 是对现有 DiskANN/Vamana 静态索引的动态能力扩展。它不改变原有 SSD 图索引的核心结构，而是在系统层面增加增量写入、懒删除、混合查询和周期性合并机制，使项目从“静态近似最近邻检索系统”升级为“支持流式更新的近似最近邻检索系统”。

该创新点实现路径清晰、模块边界明确、实验结果可量化，适合作为本项目后续优化和报告撰写中的重点创新内容。
