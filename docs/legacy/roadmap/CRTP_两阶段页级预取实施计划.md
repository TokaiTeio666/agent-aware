# 区域先验 + 拓扑修正两阶段页级预取实施计划

## 1. 文档目标

本计划用于在现有磁盘图向量检索系统中实现一套兼顾延迟、吞吐、准确率和可扩展性的页级预取机制。

方案名称：

> **Coarse-to-Fine Region-Topology Prefetch，CRTP**  
> 区域先验 + 拓扑修正两阶段页级预取

整体思路：

1. 查询开始时，通过粗量化器快速定位查询相关区域；
2. 对候选区域中的页面代表码进行轻量排序；
3. 提前提交少量高价值页面；
4. 图搜索运行过程中，根据真实 projected frontier 继续补页；
5. demand I/O 始终保持最高优先级；
6. 全量 PQ 扫描和 XGBoost 仅作为可选增强，不进入默认关键路径。

---

## 2. 背景与问题

当前预取方案主要包括：

- next-hop 邻居预取；
- pre-beam；
- candidate-heap lookahead；
- topology post-expand；
- XGBoost 页访问预测；
- 全量 PQ 扫描驱动的投机预取；
- coarse cell 到 page 的离线倒排。

现有实验已经暴露出一个核心问题：

```text
prefetch submitted: 879
ready at demand:     0
pending at demand:   841
unused:              38
submit_to_demand_us: 7.58 μs
```

这说明：

- 选中的页面不一定完全错误；
- 主要问题是提交发生得太晚；
- 预取页在 demand 到来时仍处于 pending；
- 异步 I/O 没有获得足够的重叠窗口；
- 继续单纯提升模型精度不能解决 lead time 不足。

另一方面，直接执行全量 PQ scan 会带来：

- 每查询扫描全部 PQ code；
- 查询启动串行延迟；
- 高并发内存带宽压力；
- 数据规模增长后不可扩展；
- Top-N 维护成本；
- 预取收益可能无法覆盖扫描成本。

因此需要一种介于“全量 PQ 扫描”和“纯 topology 在线预取”之间的方案。

---

## 3. 设计目标

### 3.1 主要目标

- 提高 `ready_at_demand_rate`；
- 降低 `pending_at_demand_rate`；
- 降低 `demand_read_wait_us`；
- 改善 Avg、P95 和 P99 查询延迟；
- 不显著降低高并发 QPS；
- 不降低 Recall@10；
- 不让投机 I/O 挤占 demand I/O；
- 保持 V3 page layout 不变。

### 3.2 次要目标

- 支持按负载自适应降级；
- 支持 topology-only 模式；
- 支持未来接入 XGBoost 重排；
- 支持未来接入全量 PQ aggressive 模式；
- 所有新增功能均可通过命令行参数独立开关；
- 所有阶段均可单独 A/B 验证。

### 3.3 不做的内容

本阶段暂不做：

- 修改 V3 page layout；
- 真正取消已提交的 io_uring 请求；
- 强依赖训练数据的在线学习；
- 将全量 PQ scan 设为默认路径；
- 一次性提交 20～50 个 speculative pages；
- 同时修改搜索宽度、布局、预取策略和缓存策略。

---

## 4. 总体架构

```text
                         ┌──────────────────────────┐
                         │       Offline Build      │
                         │                          │
                         │ coarse centroids         │
                         │ node → coarse cell       │
                         │ cell → pages index       │
                         │ page representative code │
                         │ optional history stats   │
                         └────────────┬─────────────┘
                                      │
                                      ▼
Query
  │
  ├──────────────────────► Layer 1: Global Region Predictor
  │                        - nearest coarse cells
  │                        - page representative ADC
  │                        - page-level ranking
  │                        - submit 4～8 low-priority pages
  │
  ▼
Graph Search
  │
  ├─ batch expansion
  │      │
  │      ▼
  │  Layer 2: Topology Correction
  │      - projected frontier
  │      - page aggregation
  │      - submit 0～2 medium-priority pages
  │
  ├─ demand read
  │      └─ highest-priority I/O
  │
  └─ search converges

Shared components:
  - cache state
  - query page state
  - submitted page dedup
  - I/O budget
  - adaptive controller
  - metrics
```

---

## 5. 核心设计原则

### 5.1 Layer 1 负责“早”

Layer 1 在图搜索开始前运行，提供全局查询先验。

它不扫描全部数据库向量，而只处理：

- `nlist` 个 coarse centroids；
- `nprobe` 个候选 cell；
- 候选 cell 内的 page representative codes。

目标是：

> 在较低 CPU 成本下，尽早提交少量目标区域页面。

### 5.2 Layer 2 负责“准”

Layer 2 使用图搜索已经产生的真实候选：

- 当前 candidate heap；
- 本轮新增邻居；
- 尚未展开节点；
- projected frontier。

目标是：

> 修正 Layer 1 的区域预测偏差，并覆盖中间路由页。

### 5.3 Demand 永远优先

优先级固定为：

```text
Demand I/O             > Topology correction > Global region prefetch
```

任何预取都不得影响真实 demand 请求。

### 5.4 预取单位必须是 page

所有候选节点先映射为 `page_id`，再做：

- 去重；
- 聚合；
- 打分；
- 排序；
- 预算控制。

不能按 node 数量直接提交 I/O。

### 5.5 自适应依据必须是净收益

不能只看 `useful_rate`。

控制器重点观察：

- `ready_at_demand_rate`；
- `pending_at_demand_rate`；
- `coverage`；
- `saved_wait_us`；
- `prediction_cost_us`；
- `queue_interference_us`；
- `net_saved_us`。

---

## 6. 离线索引设计

## 6.1 粗量化器

离线训练 coarse centroids：

```text
nlist = 1024 / 2048 / 4096
```

初始推荐：

```text
SIFT1M:
nlist = 4096
```

每个节点分配到最近的 coarse cell：

```cpp
cell_id = nearest_coarse_centroid(vector);
```

离线保存：

```cpp
std::vector<uint32_t> node_to_cell;
```

---

## 6.2 Cell 到 Page 倒排

构建：

```cpp
cell_to_pages[cell_id]
```

每个 entry 不仅保存 `page_id`，还应保存用于查询时页级排序的轻量信息。

```cpp
struct PageEntry {
    uint32_t page_id;

    // 页面代表向量或其 PQ code
    PageRepCode representative_code;

    // 当前 page 中属于该 cell 的节点数
    uint16_t local_node_count;

    // 页内有效节点比例
    float cell_density;

    // 可选：历史有用率
    float historical_useful_rate;

    // 可选：历史平均提前量
    float historical_lead_time_us;
};
```

---

## 6.3 页面代表码

候选方案：

### 方案 A：页面均值向量

```text
page vectors
    ↓ mean
page representative vector
    ↓ PQ encode
page representative code
```

优点：

- 能代表页面整体分布；
- 页内多个相似节点时效果更稳定。

缺点：

- 离线构建稍复杂；
- 对离群节点不敏感。

### 方案 B：页面 medoid

选择页面内到其他节点平均距离最小的节点作为代表。

优点：

- 代表码直接复用现有节点 PQ code；
- 无需额外编码格式。

缺点：

- 代表单个节点，可能遗漏多模态页。

### 方案 C：页面双代表

每页保存两个 representative codes。

优点：

- 能覆盖一个页面中的两个局部簇；
- 查询相关性更强。

缺点：

- 查询时计算量翻倍。

初始版本使用：

```text
方案 A 或方案 B
```

不建议第一版直接使用双代表。

---

## 6.4 紧凑存储

建议布局：

```cpp
struct CellPageRange {
    uint32_t begin;
    uint32_t count;
};

std::vector<CellPageRange> cell_ranges;
std::vector<PageEntry> page_entries;
```

避免：

```cpp
std::vector<std::vector<PageEntry>>
```

原因：

- 连续内存；
- cache locality 更好；
- mmap 更方便；
- 序列化简单；
- 查询扫描更快。

---

## 6.5 离线历史统计

历史访问频率不能作为主排序依据，只作为辅助项。

可保存：

```text
historical_useful_rate
historical_ready_rate
historical_avg_saved_wait_us
historical_query_count
```

历史统计来源：

- 训练 query trace；
- baseline graph-search demand trace；
- 预取开启后的运行日志。

默认权重应低于查询距离项。

---

## 7. 查询执行流程

## 7.1 Layer 1：Global Region Predictor

流程：

```text
query
  ↓
计算 query 到 coarse centroids 的距离
  ↓
选择 top-nprobe cells
  ↓
扫描这些 cells 的 page representative codes
  ↓
按 page 聚合和打分
  ↓
选择 top 4～8 pages
  ↓
低优先级批量提交
  ↓
图搜索立即开始
```

---

## 7.2 Layer 1 页分数

推荐分数：

\[
Score_{global}(p)=
-\alpha D_{rep}(q,p)
+\beta CellRankScore(p)
+\gamma Density(p)
+\delta HistoricalUseful(p)
-\mu QueuePressure
\]

其中：

```text
D_rep(q,p)
```

表示 query 到页面代表码的近似距离。

```text
CellRankScore(p)
```

表示页面所属 coarse cell 的排名贡献。

```text
Density(p)
```

表示该页面中属于命中 cell 的节点密度。

```text
HistoricalUseful(p)
```

只用于轻量 tie-break。

初始权重建议：

```text
distance_weight = 1.0
cell_rank_weight = 0.2
density_weight = 0.1
history_weight = 0.05
```

权重最终以 A/B 实验为准。

---

## 7.3 Layer 1 伪代码

```cpp
void QueryPageSession::global_region_prefetch(
    const float* query)
{
    ScopedTimer timer(metrics_.global_predict_us);

    auto cells = coarse_index_.nearest_cells(
        query,
        config_.nprobe
    );

    page_score_buffer_.clear();

    for (size_t cell_rank = 0;
         cell_rank < cells.size();
         ++cell_rank) {

        const uint32_t cell_id = cells[cell_rank].cell_id;

        const auto range = cell_ranges_[cell_id];

        for (uint32_t i = 0; i < range.count; ++i) {
            if (timer.elapsed_us() >=
                config_.global_time_budget_us) {
                metrics_.global_budget_timeout++;
                break;
            }

            const auto& entry =
                page_entries_[range.begin + i];

            if (is_known_or_cached(entry.page_id)) {
                continue;
            }

            float rep_distance =
                page_rep_distance(query,
                                  entry.representative_code);

            float score =
                -config_.distance_weight * rep_distance
                +config_.cell_rank_weight /
                    static_cast<float>(cell_rank + 1)
                +config_.density_weight *
                    entry.cell_density
                +config_.history_weight *
                    entry.historical_useful_rate;

            accumulate_page_score(
                entry.page_id,
                score,
                PrefetchSource::GLOBAL_REGION
            );
        }
    }

    auto pages = select_top_global_pages(
        config_.global_candidate_limit
    );

    uint32_t submitted = 0;

    for (const auto& page : pages) {
        if (submitted >= config_.max_global_pages) {
            break;
        }

        if (!budget_.can_submit_global()) {
            break;
        }

        if (submit_prefetch_if_needed(
                page.page_id,
                PrefetchSource::GLOBAL_REGION)) {
            submitted++;
        }
    }

    metrics_.global_pages_submitted += submitted;
}
```

---

## 7.4 Layer 2：Topology Correction

触发时机：

```text
每次 batch expand 结束后
```

候选来源：

- candidate heap 前若干项；
- 本轮新加入候选；
- 尚未 expanded 的候选；
- projected frontier。

处理流程：

```text
frontier candidates
    ↓
node → page
    ↓
页级去重
    ↓
页内候选聚合
    ↓
按 rank 和 candidate count 打分
    ↓
补提交 0～2 pages
```

---

## 7.5 Layer 2 页分数

推荐分数：

\[
Score_{topology}(p)=
\sum_{v \in p}
\frac{1}{rank(v)+1}
+\lambda CandidateCount(p)
+\eta BestDistanceScore(p)
-\mu QueuePressure
\]

其中：

- 候选 rank 越靠前，贡献越高；
- 同一页候选越多，页内复用价值越高；
- 当前 I/O 压力越大，提交越保守。

---

## 7.6 Layer 2 伪代码

```cpp
void QueryPageSession::topology_correction(
    const ProjectedFrontier& frontier)
{
    ScopedTimer timer(metrics_.topology_predict_us);

    topology_page_scores_.clear();

    size_t rank = 0;

    for (const auto& candidate :
         frontier.top(config_.frontier_scan_width)) {

        PageId page =
            layout_.node_to_page(candidate.node_id);

        if (is_known_or_cached(page)) {
            rank++;
            continue;
        }

        auto& score = topology_page_scores_[page];

        score.rank_score +=
            1.0f / static_cast<float>(rank + 1);

        score.candidate_count++;

        score.best_distance =
            std::min(score.best_distance,
                     candidate.distance);

        rank++;
    }

    auto pages = sort_topology_pages(
        topology_page_scores_
    );

    uint32_t submitted = 0;

    for (const auto& page : pages) {
        if (submitted >=
            adaptive_.topology_pages_per_batch()) {
            break;
        }

        if (!budget_.can_submit_topology()) {
            break;
        }

        if (submit_prefetch_if_needed(
                page.page_id,
                PrefetchSource::TOPOLOGY)) {
            submitted++;
        }
    }

    metrics_.topology_pages_submitted += submitted;
}
```

---

## 8. 统一页面状态管理

仅使用 `submitted_pages` 集合不够，需要记录状态、来源和时间。

```cpp
enum class QueryPageState {
    NOT_REQUESTED,
    GLOBAL_PENDING,
    TOPOLOGY_PENDING,
    DEMAND_PENDING,
    READY,
    USED,
    UNUSED
};

enum class PrefetchSource {
    NONE,
    GLOBAL_REGION,
    TOPOLOGY,
    FULL_PQ_OPTIONAL,
    DEMAND
};

struct QueryPageRecord {
    QueryPageState state;
    PrefetchSource source;

    uint64_t submit_ts_us;
    uint64_t ready_ts_us;
    uint64_t first_demand_ts_us;
    uint64_t first_use_ts_us;

    bool demand_seen;
    bool used;
};
```

查询级结构：

```cpp
std::unordered_map<PageId, QueryPageRecord>
    query_page_table_;
```

---

## 9. 页面提交逻辑

```cpp
bool QueryPageSession::submit_prefetch_if_needed(
    PageId page,
    PrefetchSource source)
{
    if (cache_.contains(page)) {
        auto& record = query_page_table_[page];
        record.state = QueryPageState::READY;
        record.source = source;
        return false;
    }

    auto it = query_page_table_.find(page);

    if (it != query_page_table_.end() &&
        it->second.state !=
            QueryPageState::NOT_REQUESTED) {
        metrics_.dedup_hits++;
        return false;
    }

    const IoPriority priority =
        priority_of(source);

    if (!io_.submit_async(page, priority)) {
        metrics_.submit_failures++;
        return false;
    }

    auto& record = query_page_table_[page];

    record.source = source;
    record.submit_ts_us = now_us();

    record.state =
        source == PrefetchSource::GLOBAL_REGION
        ? QueryPageState::GLOBAL_PENDING
        : QueryPageState::TOPOLOGY_PENDING;

    return true;
}
```

---

## 10. Demand 到达时的处理

```cpp
PageHandle QueryPageSession::get_page_for_demand(
    PageId page)
{
    auto it = query_page_table_.find(page);

    if (cache_.contains(page)) {
        mark_ready_hit(page);
        return cache_.get(page);
    }

    if (it == query_page_table_.end()) {
        return submit_and_wait_demand(page);
    }

    auto& record = it->second;

    record.demand_seen = true;
    record.first_demand_ts_us = now_us();

    switch (record.state) {
        case QueryPageState::READY:
            metrics_.ready_at_demand++;
            return cache_.get(page);

        case QueryPageState::GLOBAL_PENDING:
        case QueryPageState::TOPOLOGY_PENDING:
            metrics_.pending_at_demand++;
            io_.promote_priority(page,
                                 IoPriority::DEMAND);
            record.state =
                QueryPageState::DEMAND_PENDING;
            return io_.wait(page);

        default:
            return submit_and_wait_demand(page);
    }
}
```

如果底层无法真正提升已提交 io_uring 请求的优先级，则：

- 软件层停止继续提交低优先级预取；
- demand 请求仍进入保留 slot；
- 记录该限制对实验结果的影响。

---

## 11. I/O 预算设计

默认：

```text
io_depth = 32
```

建议初始分配：

```text
Demand reserve:           16 slots
Topology pending limit:    8 slots
Global pending limit:      4～8 slots
```

不采用固定：

```text
Layer 1 = 50%
```

原因：

- speculative I/O 不确定；
- SSD 内部调度不一定识别软件逻辑优先级；
- 大量 Layer 1 请求可能阻塞真实 demand；
- 预取目标是减少等待，不是占满设备队列。

---

## 12. I/O 预算规则

```cpp
bool IoBudget::can_submit_global() const
{
    if (pending_demand_ > 0) {
        return false;
    }

    if (total_pending_ >=
        io_depth_ - demand_reserve_) {
        return false;
    }

    return pending_global_ <
           max_global_pending_;
}
```

```cpp
bool IoBudget::can_submit_topology() const
{
    if (total_pending_ >=
        io_depth_ - demand_reserve_) {
        return false;
    }

    return pending_topology_ <
           max_topology_pending_;
}
```

优先级：

```text
Demand            = High
Topology          = Medium
Global Region     = Low
Full PQ Optional  = Lowest
```

---

## 13. 自适应控制

## 13.1 核心指标

### Useful Rate

\[
UsefulRate=
\frac{used\ prefetch\ pages}
{submitted\ prefetch\ pages}
\]

### Ready Rate

\[
ReadyRate=
\frac{ready\ before\ demand}
{used\ prefetch\ pages}
\]

### Coverage

\[
Coverage=
\frac{prefetch\ covered\ demand\ pages}
{all\ demand\ pages}
\]

### Net Saved Time

\[
NetSavedUs=
SavedDemandWaitUs
-
GlobalPredictionCostUs
-
TopologyPredictionCostUs
-
EstimatedQueueInterferenceUs
\]

---

## 13.2 Layer 1 控制逻辑

```text
如果 net_saved_us <= 0：
    收缩 max_global_pages

如果 useful_rate 低且 coverage 低：
    收缩 nprobe 或 global_candidate_limit

如果 useful_rate 高但 ready_rate 低：
    说明选页较准，但提交仍不够早或队列过载

如果 queue interference 高：
    降低 max_global_pending

如果连续多个窗口 net_saved_us 为负：
    暂时关闭 Layer 1，退化为 topology-only
```

---

## 13.3 Layer 2 控制逻辑

初始：

```text
前 32 个 query：
topology_pages_per_batch = 1
```

之后：

```text
ready_rate 高、unused 低：
    可增加到 2

pending_at_demand 高：
    不一定增加页数，先检查 lead time

waste_rate 高：
    降回 1 或 0

demand pending 高：
    暂停 topology prefetch
```

---

## 13.4 联动逻辑

```text
Layer 1 coverage 高：
    Layer 2 更保守

Layer 1 coverage 低：
    Layer 2 承担更多补漏

Layer 1 waste 连续升高：
    降低 nprobe 或 max_global_pages

系统高负载：
    关闭 Layer 1，只保留 topology

系统低负载且收益稳定：
    可开启 aggressive 模式
```

---

## 14. 全量 PQ Scan 的定位

全量 PQ scan 不作为默认模式。

仅在以下条件满足时开启：

```text
dataset_size <= threshold
CPU load 较低
memory bandwidth 充足
历史 coverable_wait_us 显著高于 pq_scan_us
I/O queue 空闲
```

模式：

```text
normal:
    region + topology

high_load:
    topology only

low_load_aggressive:
    full PQ + topology

fallback:
    topology only
```

全量 PQ scan 的收益判断：

\[
CoveredWaitUs >
PQScanUs +
TopNUs +
QueueInterferenceUs
\]

若不满足，禁止开启。

---

## 15. XGBoost 的定位

XGBoost 不负责生成候选，只负责候选页重排。

候选来源：

```text
Global Region candidates
+
Topology candidates
```

可用特征：

```text
page representative distance
coarse cell rank
cell density
frontier best rank
frontier node count
same-page candidate count
historical useful rate
historical ready rate
cache state
pending I/O pressure
source type
```

默认关闭 XGBoost，先验证两阶段规则模型。

---

## 16. 配置参数

建议新增：

```text
--prefetch-policy region-topology
--coarse-index-path <path>
--cell-page-index-path <path>

--coarse-nlist 4096
--coarse-nprobe 8

--global-prefetch-enable true
--global-time-budget-us 500
--global-candidate-limit 64
--global-max-pages 4
--global-max-pending 4

--topology-prefetch-enable true
--topology-frontier-width 32
--topology-pages-per-batch 1
--topology-max-pages-per-batch 2
--topology-max-pending 8

--io-depth 32
--demand-reserve 16

--prefetch-adaptive-enable true
--prefetch-warmup-queries 32

--full-pq-aggressive-enable false
--xgb-page-rerank-enable false
```

---

## 17. 建议模块划分

```text
src/
├── prefetch/
│   ├── query_page_session.h
│   ├── query_page_session.cpp
│   ├── global_region_prefetcher.h
│   ├── global_region_prefetcher.cpp
│   ├── topology_prefetcher.h
│   ├── topology_prefetcher.cpp
│   ├── io_budget_controller.h
│   ├── io_budget_controller.cpp
│   ├── adaptive_prefetch_controller.h
│   ├── adaptive_prefetch_controller.cpp
│   ├── prefetch_metrics.h
│   └── prefetch_metrics.cpp
│
├── quantization/
│   ├── coarse_quantizer.h
│   ├── coarse_quantizer.cpp
│   ├── page_representative.h
│   └── page_representative.cpp
│
├── index/
│   ├── cell_page_index.h
│   ├── cell_page_index.cpp
│   ├── cell_page_index_builder.h
│   └── cell_page_index_builder.cpp
│
└── search/
    ├── graph_search.cpp
    └── projected_frontier.h
```

实际文件名根据现有仓库结构调整。

---

## 18. 指标设计

## 18.1 Layer 1 指标

```text
global_predict_us
coarse_distance_us
coarse_topn_us
page_rep_scan_us
global_candidate_pages
global_pages_submitted
global_pages_ready_at_demand
global_pages_pending_at_demand
global_pages_used
global_pages_unused
global_page_useful_rate
global_page_ready_rate
global_page_coverage
global_submit_to_demand_us
```

---

## 18.2 Layer 2 指标

```text
topology_predict_us
frontier_candidates_scanned
frontier_unique_pages
topology_pages_submitted
topology_pages_ready_at_demand
topology_pages_pending_at_demand
topology_pages_used
topology_pages_unused
topology_page_useful_rate
topology_page_ready_rate
topology_submit_to_demand_us
```

---

## 18.3 统一指标

```text
prefetch_dedup_hits
prefetch_submit_failures

demand_page_reads
demand_read_wait_us
demand_ready_hits
demand_pending_hits

nodes_used_per_loaded_page
same_page_node_reuse
page_fill_utilization
candidate_node_page_ratio

query_latency_avg
query_latency_p50
query_latency_p95
query_latency_p99
qps
recall_at_10

cpu_utilization
memory_bandwidth
io_queue_depth_avg
io_queue_depth_p95
```

---

## 18.4 净收益指标

```text
saved_demand_wait_us
prediction_cost_us
estimated_queue_interference_us
net_saved_us
```

建议：

```text
net_saved_us
```

作为自适应控制的主要反馈信号。

---

## 19. 实施阶段

# Phase 0：基线冻结

目标：

- 固定 V3 layout；
- 固定搜索参数；
- 固定缓存预算；
- 固定测试数据；
- 固定 query 集；
- 固定 io_depth；
- 保存当前 baseline。

记录：

```text
Recall@10
Avg
P50
P95
P99
QPS
demand_read_wait_us
page reads/query
cache hit rate
```

验收标准：

- baseline 可稳定复现；
- 连续三轮结果波动可接受；
- 所有后续实验使用同一配置。

---

# Phase 1：指标与页面状态统一

实现：

- `QueryPageSession`；
- `QueryPageRecord`；
- page 状态机；
- source attribution；
- submit/ready/demand/use 时间戳；
- aggregate 和 JSON 输出。

验收标准：

- 能区分 global、topology、demand；
- 能统计 ready/pending/unused；
- 能计算 submit-to-demand；
- 重复提交为 0；
- 不改变 baseline 性能超过容许波动。

---

# Phase 2：Topology-only

实现：

- projected frontier；
- 页级聚合；
- 每 batch 0～2 页；
- I/O 预算；
- demand reserve。

实验：

```text
topology_pages_per_batch = 0 / 1 / 2
frontier_scan_width = beam / 2beam / 4beam
max_topology_pending = 4 / 8
```

验收标准：

- `ready_at_demand > 0`；
- `pending_at_demand` 比 pre-beam 下降；
- P95 不恶化；
- waste 可控；
- demand I/O 不被挤压。

---

# Phase 3：离线 Coarse Index

实现：

- coarse centroid 训练；
- node-to-cell 分配；
- cell-to-page 倒排；
- page representative 构建；
- 紧凑序列化；
- mmap/load。

验收标准：

- 索引构建成功；
- node/page/cell 数量一致；
- 可随机抽样验证；
- 查询时可正确加载；
- 内存占用可接受。

---

# Phase 4：Global Region Layer

实现：

- nearest coarse cells；
- page representative ADC；
- 页级打分；
- global time budget；
- 提交 4～8 页；
- 与 topology 共享去重和状态。

实验：

```text
nlist = 1024 / 2048 / 4096
nprobe = 2 / 4 / 8
global_max_pages = 2 / 4 / 8
global_time_budget_us = 200 / 500 / 1000
```

验收标准：

- global predictor 不明显拖慢搜索启动；
- prediction cost 可控；
- global coverage 有正收益；
- net_saved_us 为正；
- P95/P99 至少不恶化。

---

# Phase 5：两层联动

实现：

- Layer 1 和 Layer 2 共享 page table；
- 联动预算；
- Layer 1 命中时 Layer 2 自动跳过；
- Layer 1 失效时 Layer 2 承担补漏；
- high-load topology-only 降级。

验收标准：

- 无重复 I/O；
- demand 优先级正确；
- 两层叠加优于至少一个单层方案；
- P95/P99 改善；
- QPS 不出现明显下降。

---

# Phase 6：自适应控制

实现：

- 滑动窗口统计；
- `net_saved_us` 控制；
- 动态调整 global pages；
- 动态调整 topology pages；
- 自动关闭 Layer 1；
- 高负载降级。

初始窗口：

```text
warmup_queries = 32
control_window = 64
```

验收标准：

- workload 改变时能自动收缩；
- 负收益时能关闭 Layer 1；
- 高负载时 demand 延迟不被放大；
- 控制器无频繁震荡。

---

# Phase 7：可选增强

依次验证：

1. 双页面代表码；
2. XGBoost 页级重排；
3. 全量 PQ aggressive mode；
4. 多查询共享 page；
5. page popularity 衰减；
6. 并发 query 去重；
7. 更细粒度 I/O 优先级。

这些功能不得与基础方案同时首次上线。

---

## 20. A/B 实验矩阵

必须按以下顺序比较：

```text
A. No Prefetch
B. Existing Pre-beam
C. Topology-only
D. Global Region-only
E. Global Region + Topology
F. E + Adaptive Controller
G. F + Optional XGBoost
H. F + Optional Full PQ Aggressive
```

每组固定：

```text
dataset
query set
layout
cache budget
beam width
search width
entry point
io_depth
thread count
concurrency
```

---

## 21. 测试场景

### 单查询低并发

```text
concurrency = 1
```

重点：

- Avg；
- P95；
- P99；
- prediction cost；
- ready rate；
- net saved time。

### 中并发

```text
concurrency = 4 / 8
```

重点：

- QPS；
- queue pressure；
- demand interference；
- global scan 并发开销。

### 高并发

```text
concurrency = 16 / 32
```

重点：

- 是否自动降级；
- topology-only 是否更稳；
- 内存带宽；
- I/O 队列争用；
- P99。

### 冷缓存

重点：

- Layer 1 是否真正隐藏 SSD 等待；
- 首次查询收益；
- speculative waste。

### 热缓存

重点：

- 是否自动减少预取；
- 是否发生无意义计算；
- prediction cost 是否超过收益。

---

## 22. 成功标准

建议最低验收目标：

```text
Recall@10 不下降
Avg 不恶化
P95 改善 ≥ 8%
P99 改善 ≥ 5%
demand_read_wait_us 下降 ≥ 10%
ready_at_demand_rate 显著高于 pre-beam
net_saved_us > 0
高并发 QPS 下降 < 5%
```

更理想目标：

```text
P95 改善 10%～20%
P99 改善 8%～15%
demand_read_wait_us 下降 15%～30%
global prediction cost < 500 μs
topology prediction cost < 100 μs/batch
```

所有目标必须以实际实验为准，不能提前写成已实现结果。

---

## 23. 失败判定

出现以下任一情况，应停止扩大 Layer 1：

```text
global_predict_us > saved_demand_wait_us
global_page_coverage 长期低于 10%
global_page_useful_rate 长期低于 20%
demand P99 因队列争用恶化
高并发 QPS 下降超过 10%
memory bandwidth 成为主要瓶颈
global page ready rate 长期接近 0
```

此时默认退化为：

```text
Topology-only
```

---

## 24. 风险与应对

### 风险 1：页面代表码不足以代表整页

应对：

- 比较 mean、medoid、双代表；
- 记录页内方差；
- 高方差页降低分数。

### 风险 2：cell 内页面过多

应对：

- 增大 `nlist`；
- 限制扫描时间；
- 离线按 coarse residual 再分桶；
- page representative 连续存储。

### 风险 3：Layer 1 挤压 demand

应对：

- demand reserve 至少 50%；
- pending demand 时停止 Layer 1；
- Layer 1 默认只提交 4 页；
- 高并发关闭 Layer 1。

### 风险 4：Topology 仍然太晚

应对：

- batch expand 后立即触发；
- frontier 扫描保持轻量；
- 只聚合前 `2 × beam`；
- 不做复杂模型推理。

### 风险 5：自适应震荡

应对：

- 使用滑动窗口；
- 增加冷却周期；
- 调整一次后至少保持一个窗口；
- 采用分级参数，不连续微调。

---

## 25. 推荐初始参数

SIFT1M 初始基线：

```text
nlist = 4096
nprobe = 4

global_time_budget_us = 500
global_candidate_limit = 64
global_max_pages = 4
global_max_pending = 4

frontier_scan_width = 2 × beam_width
topology_pages_per_batch = 1
topology_max_pages_per_batch = 2
topology_max_pending = 8

io_depth = 32
demand_reserve = 16

warmup_queries = 32
control_window = 64
```

第一轮不要直接使用：

```text
global_max_pages = 20～40
nprobe = 16+
full PQ scan
XGBoost rerank
```

---

## 26. 最终交付物

代码：

- coarse quantizer；
- cell-page index builder；
- page representative builder；
- global region prefetcher；
- topology prefetcher；
- unified query page session；
- I/O budget controller；
- adaptive controller；
- metrics aggregator。

数据：

- coarse centroid 文件；
- cell-to-page 索引；
- page representative codes；
- baseline trace；
- A/B 实验结果。

文档：

```text
docs/prefetch/
├── CRTP_DESIGN.md
├── CRTP_IMPLEMENTATION.md
├── CRTP_METRICS.md
├── CRTP_AB_TEST.md
└── CRTP_RESULTS.md
```

实验输出：

```text
CSV
JSON
aggregate summary
latency histogram
ready/pending/unused breakdown
coverage/useful/ready comparison
```

---

## 27. 最终总结

本方案不将任何单一预测器视为完整答案，而是进行职责拆分：

```text
Coarse region predictor：
    提供低成本、全局、提前的区域先验

Page representative：
    将 coarse cell 缩小到查询相关页面

Topology correction：
    根据真实图搜索路径进行在线补漏

Unified I/O budget：
    防止 speculative I/O 挤压 demand

Adaptive controller：
    根据净收益动态启停和调整预算
```

最终主路径：

```text
query
  ↓
coarse cell selection
  ↓
page representative ranking
  ↓
4～8 pages speculative prefetch
  ↓
graph search
  ↓
projected frontier correction
  ↓
0～2 pages per batch
  ↓
demand-first I/O
```

该方案的核心目标不是“提交更多页面”，而是：

> **在较低预测成本下，让少量高价值页面更早进入 I/O，并用真实拓扑搜索状态持续修正预测，从而减少 demand 等待而不牺牲吞吐。**
