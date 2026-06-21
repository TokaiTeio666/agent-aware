# P3 缓存与零拷贝优化计划

## 当前实现状态（2026-06-21）

P3 的关键读路径优化已经部分落地：当前 packed graph 查询路径包含 page cache 命中/淘汰/同页复用统计，支持 `compute_distance_direct`，并能输出 cache、direct distance、page dedup 等指标。独立 BufferPool 管理器、严格 pin/unpin 生命周期和更系统的 2Q 对照实验仍可作为后续工程化增强。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| page cache 指标 | 已完成 | `DiskGraphSearchStats` 中的 cache hit/miss/eviction/promote 指标 |
| 同页复用 | 已完成 | `same_page_reuse` 与 `same_page_node_reuse` |
| 零拷贝距离计算 | 已完成 | `PackedDiskGraphIndex::compute_distance_direct` |
| Graph-Aware 2Q 策略 | 已接入可展示版 | README/benchmark 可配置 `graph-aware-2q`，仍需更完整对照实验 |
| pin/unpin 独立接口 | 未完全拆出 | 当前以查询页会话和内部状态保护为主 |

## 1. 阶段定位

| 阶段 | 时间 | 核心目标 | 交付物 |
|---|---:|---|---|
| P3 缓存与零拷贝 | 第 3–4 周 | 完成 Graph-Aware 2Q 缓存、pin/unpin、compute_distance_direct | 缓存命中率对比、线程安全测试、性能对比报告 |

P3 阶段的目标不是继续扩大搜索宽度，而是减少重复 I/O、保护高价值图节点、降低距离计算路径中的拷贝成本，从而提升 SSD 图搜索的缓存命中率与尾延迟表现。

---

## 2. 创新点与价值

| 创新点 | 价值 |
|---|---|
| Graph-Aware 2Q 缓存 | 将图入度引入缓存淘汰策略，保护 hub 节点，提升随机图遍历缓存命中率 |
| pin/unpin 页面保护 | 搜索过程中临时固定关键 page，避免热点节点在一次 query 内被误淘汰 |
| compute_distance_direct | 直接基于 page 内向量数据计算距离，减少 page → 临时 vector 的内存拷贝 |
| 线程安全缓存接口 | 支持多线程 query 压测，避免缓存结构在并发场景下出现数据竞争 |

---

## 3. 背景问题

当前图搜索的主要瓶颈不只是单次 SSD 读取慢，而是：

1. 搜索路径中存在大量小粒度随机读；
2. hub 节点会被频繁访问，但普通 LRU 容易把它们淘汰；
3. 每次访问节点 page 后，如果再拷贝向量到临时缓冲区计算距离，会增加 CPU 与内存带宽开销；
4. 多线程查询时，缓存命中率、锁竞争和线程安全会直接影响 P95/P99 延迟。

因此，P3 的核心思路是：**让缓存理解图结构，让距离计算尽量贴近 page 原始数据，让并发访问保持正确性。**

---

## 4. 技术方案

### 4.1 Graph-Aware 2Q 缓存

传统 2Q 缓存通常包含：

- `A1in`：新进入缓存的页面队列；
- `Am`：多次命中的主缓存队列；
- `A1out`：被淘汰页面的 ghost 队列，用于判断页面是否值得重新进入主缓存。

本项目在此基础上加入图结构信息：

- 每个节点记录 `in_degree` 或近似访问重要性；
- 高入度节点被视为 hub 节点；
- hub 节点命中后优先进入或保留在 `Am`；
- 淘汰时优先淘汰低入度、低访问频率、未 pin 的页面；
- 支持统计 page hit、miss、eviction、pin count、hub hit ratio。

推荐缓存评分公式：

```text
score(page) = access_score + alpha * log(1 + in_degree) + beta * recent_hit - gamma * cold_age
```

其中：

- `access_score`：历史访问频率；
- `in_degree`：图入度或 hub 权重；
- `recent_hit`：近期是否命中；
- `cold_age`：距离上次访问的时间；
- `alpha/beta/gamma`：可通过实验调参。

---

### 4.2 pin/unpin 机制

在一次 query 的搜索过程中，部分 page 具有短时间强相关性，例如：

- 当前展开节点；
- 候选队列中的高优先级节点；
- medoid / entry point；
- 高入度 hub 节点。

为避免这些 page 在搜索过程中被淘汰，需要实现：

```cpp
pin(page_id);
unpin(page_id);
is_pinned(page_id);
```

基本规则：

1. 被 pin 的 page 不允许被缓存淘汰；
2. pin 需要引用计数，而不是简单 bool；
3. query 结束后必须释放所有 pin；
4. 异常路径也必须保证 unpin，建议使用 RAII Guard；
5. 缓存满且全是 pinned page 时，需要返回明确错误或降级为同步读取。

推荐接口：

```cpp
class PagePinGuard {
public:
    PagePinGuard(GraphAware2QCache& cache, PageId id);
    ~PagePinGuard();
private:
    GraphAware2QCache& cache_;
    PageId id_;
};
```

---

### 4.3 compute_distance_direct

原始路径可能是：

```text
read page → parse vector → copy to temporary buffer → compute distance
```

优化后应变为：

```text
read/cache page → locate vector pointer inside page → compute distance directly
```

目标：

- 减少一次向量内存拷贝；
- 减少临时对象构造；
- 复用已有 SIMD 距离计算函数；
- 保持 page layout 与 O_DIRECT 对齐要求兼容。

推荐接口：

```cpp
float compute_distance_direct(
    const QueryVector& query,
    const PageView& page,
    uint32_t dim
);
```

如果已有 `squared_l2()` 已经包含 AVX/SSE2 分支，则 `compute_distance_direct` 不需要重新写 SIMD，只需要保证传入的是 page 内连续向量地址。

---

## 5. 实施步骤

### 第 3 周：缓存结构与单线程正确性

| 任务 | 说明 | 验收标准 |
|---|---|---|
| 设计缓存接口 | 统一 get/put/pin/unpin/stat 接口 | 编译通过，接口可被 graph search 调用 |
| 实现基础 2Q | 支持 A1in、Am、A1out | 单元测试覆盖命中、淘汰、ghost hit |
| 加入 Graph-Aware 权重 | 引入 in_degree / hub_score | hub page 被淘汰概率明显降低 |
| 接入图搜索 | 替换原有 page cache 或 naive cache | graph engine 能正常跑 SIFT 子集 |
| 统计指标 | hit rate、miss、eviction、hub hit rate | result 文件输出完整指标 |

### 第 4 周：零拷贝、并发与性能验证

| 任务 | 说明 | 验收标准 |
|---|---|---|
| 实现 pin/unpin | 支持引用计数与 RAII guard | 无 pin 泄漏，异常路径安全 |
| 实现 compute_distance_direct | 直接从 page 内读取向量计算距离 | 与原始距离结果一致 |
| 线程安全改造 | mutex/shared_mutex/分片锁 | ThreadSanitizer 或压力测试无数据竞争 |
| 多线程压测 | 1/2/4/8 线程 query | 不崩溃，结果稳定 |
| 性能对比 | 对比 naive cache、LRU、Graph-Aware 2Q | 命中率提升，P95/P99 不劣化 |

---

## 6. 关键实验设计

### 6.1 缓存命中率对比

建议实验组：

| 组别 | 缓存策略 | 目的 |
|---|---|---|
| Baseline | 无缓存 / 原始缓存 | 作为最低基线 |
| LRU | 普通最近最少使用 | 对照传统缓存策略 |
| 2Q | 标准 2Q | 验证 2Q 对随机访问是否有效 |
| Graph-Aware 2Q | 图感知 2Q | 验证 hub 保护是否有效 |

记录指标：

```text
cache_hit_rate
hub_hit_rate
ssd_reads_per_query
visited_per_query
expanded_per_query
avg_latency_ms
p50_latency_ms
p95_latency_ms
p99_latency_ms
recall@10
```

### 6.2 线程安全测试

测试内容：

1. 多线程同时查询；
2. 多线程同时访问同一 hub page；
3. pin/unpin 高频调用；
4. 缓存容量较小时频繁淘汰；
5. query 异常提前退出时检查 pin 是否释放。

推荐测试命令参数：

```bash
BASE_LIMIT=100000 \
QUERY_LIMIT=1000 \
ENGINE=graph \
CACHE_POLICY=graph_aware_2q \
CACHE_PAGES=4096 \
THREADS=4 \
bash scripts/linux/run_sift_local.sh
```

---

## 7. 结果记录模板

| 实验组 | Cache Pages | Recall@10 | Hit Rate | Hub Hit Rate | SSD Reads/Q | Avg ms | P95 ms | P99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| No Cache | 0 |  |  |  |  |  |  |  |
| LRU | 4096 |  |  |  |  |  |  |  |
| 2Q | 4096 |  |  |  |  |  |  |  |
| Graph-Aware 2Q | 4096 |  |  |  |  |  |  |  |

---

## 8. 验收标准

P3 阶段完成后，至少需要达到以下标准：

1. Graph-Aware 2Q 缓存可在 graph engine 中正常启用；
2. 支持配置缓存容量、缓存策略、线程数；
3. `pin/unpin` 无泄漏、无死锁、无错误淘汰；
4. `compute_distance_direct` 与原始距离计算结果一致；
5. 多线程查询稳定运行；
6. 输出缓存命中率、hub 命中率、SSD reads/query、P95/P99 latency；
7. 与 LRU 或普通 2Q 相比，Graph-Aware 2Q 在随机图遍历中缓存命中率更高；
8. Recall@10 不因缓存改造下降。

---

## 9. 风险与应对

| 风险 | 表现 | 应对 |
|---|---|---|
| 缓存锁竞争过高 | 多线程 QPS 不升反降 | 使用分片锁、读写锁或 thread-local 统计 |
| pin 泄漏 | 缓存 page 长期无法淘汰 | 使用 RAII guard，增加 pin_count debug 检查 |
| hub 保护过强 | 普通页面命中率下降 | 限制 hub page 占比，例如不超过 30%–50% |
| 直接距离计算出错 | Recall 下降 | 对比 direct 与 copy-based 距离误差 |
| 缓存容量过小 | 命中率提升不明显 | 做 cache pages 参数扫描 |

---

## 10. 最终产出

P3 结束时建议归档以下文件：

```text
archive/results/p3-cache-baseline.txt
archive/results/p3-cache-lru.txt
archive/results/p3-cache-2q.txt
archive/results/p3-cache-graph-aware-2q.txt
archive/logs/p3-cache-thread-safety.log
docs/design/cache-zero-copy.md
```

最终报告需要重点回答三个问题：

1. Graph-Aware 2Q 是否比普通 LRU / 2Q 更适合图搜索？
2. pin/unpin 是否能保护 query 内关键 page，且没有线程安全问题？
3. compute_distance_direct 是否减少了拷贝开销，并保持 Recall 不变？
