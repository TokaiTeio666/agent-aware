# 03 异步预取设计文档

## 当前实现状态（2026-06-22）

核心能力已经进入 SSD 查询主路径：`AsyncPageReader` 提供同步读、异步读、按 `io_batch` 切分的批量提交和 io_uring fallback；`QueryPageSession` 负责 frontier/next-hop prefetch、pending/ready page 管理和 useful/wasted 统计；`PackedDiskGraphIndex` 在候选扩展后把真实邻居列表接入 next-hop 预取；`agent_aware_flow` 可通过 `--io-mode`、`--io-depth`、`--io-batch`、`--prefetch-policy`、`--prefetch-width`、`--prefetch-depth`、`--prefetch-fallback-width`、`--page-coalesce` 调整。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| io_uring 接入 | 已完成可回退 | `AsyncPageReader::configure/status` |
| 批量提交 | 已完成 | `AsyncPageReader::batch_submit` |
| 拓扑预取 | 已完成 | `PrefetchPlanner`、`QueryPageSession::submit_next_hop_prefetch` |
| 预取指标 | 已完成 | `prefetch_submitted/useful/wasted`、`io_prefetch_*` |
| 大规模参数扫描 | 部分完成 | SIFT1M 主路径已跑通，完整矩阵见参数调优计划 |

## 1. 实现定位

**设计主题：** 异步预取
**核心目标：** 在现有 SSD 图搜索主路径上接入 `io_uring`，实现批量异步 I/O 提交，并结合图拓扑访问规律进行预取，降低随机读等待时间，提高查询吞吐量与尾延迟表现。

本设计不是单纯替换同步 `pread` 为 `io_uring`，而是要围绕图搜索的访问模式建立一套可评测、可回退、可对照的异步读取机制。重点解决当前图搜索中“大量小粒度随机读、同步等待、I/O depth 不足、下一跳访问缺少提前准备”等问题。

---

## 2. 主要任务

### 2.1 接入 io_uring 异步 I/O 框架

在 SSD 图索引读取路径中引入 `io_uring`，封装统一的异步读接口，避免搜索逻辑直接依赖底层系统调用。

主要工作包括：

- 初始化 `io_uring` 队列；
- 支持异步提交读取请求；
- 支持批量提交 batch submit；
- 支持批量回收完成事件；
- 保留同步读取 fallback 路径；
- 对读取失败、短读、越界读进行错误处理。

预期产出：

- `AsyncPageReader` 或等价模块；
- 同步读 / 异步读可通过参数切换；
- 基础单元测试或小规模功能验证。

---

### 2.2 完成 batch submit 批量提交机制

图搜索过程中会产生多个待访问节点，如果每次只提交一个 I/O 请求，`io_uring` 的优势无法体现。因此需要将多个 page read 合并为批量提交。

主要工作包括：

- 维护 pending read 队列；
- 达到一定数量后批量提交；
- 支持配置 batch size；
- 控制 in-flight I/O 深度；
- 避免重复提交同一 page；
- 统计每次 query 的提交次数、完成次数、平均 batch 大小。

建议配置项：

```text
--async_io=1
--io_backend=uring
--io_depth=32 / 64 / 128
--io_batch_size=8 / 16 / 32
```

预期产出：

- 批量提交逻辑；
- I/O depth 控制逻辑；
- batch size 对 QPS 与延迟影响的实验结果。

---

### 2.3 实现拓扑预取机制

图搜索具有一定的拓扑访问规律：当前扩展节点的邻居、候选队列中距离较近的节点、Hub 节点周边邻居，都可能在后续步骤被访问。因此可以在正式扩展前提前提交部分读取请求。

主要工作包括：

- 根据候选队列选择下一批可能访问节点；
- 对当前扩展节点的邻居进行预取；
- 避免对已访问、已缓存、已提交节点重复预取；
- 控制预取宽度，防止无效 I/O 放大；
- 将预取策略与 page cache/path cache 结合。

建议配置项：

```text
--prefetch=topology
--prefetch_width=8 / 16 / 32
--prefetch_depth=1
--dedup_pages=1
```

预期产出：

- 拓扑预取策略；
- page 去重机制；
- 预取命中率统计；
- 预取带来的 I/O 放大统计。

---

### 2.4 完成 SSD 主路径 QPS 与延迟统计

本设计必须用实验数据证明异步预取是否有效，不能只停留在代码接入层面。

需要统计的核心指标包括：

| 指标 | 含义 | 目标 |
|---|---|---|
| QPS | 每秒处理 query 数 | 相比同步 SSD 基线提升 |
| Avg Latency | 平均查询延迟 | 下降 |
| P50 Latency | 中位数延迟 | 下降或基本持平 |
| P95 Latency | 95 分位延迟 | 明显下降 |
| P99 Latency | 99 分位延迟 | 重点优化对象 |
| SSD Reads / Query | 每个 query 的 SSD 读取次数 | 不显著上升 |
| I/O Amplification | 实际读取量 / 有效访问量 | 可控 |
| Prefetch Hit Rate | 预取命中率 | 越高越好 |
| Recall@K | 检索召回率 | 不低于同步 SSD 基线 |

---

## 3. 实施步骤

### io_uring 接入与 batch submit

1. 梳理当前 SSD page 读取路径；
2. 抽象统一 PageReader 接口；
3. 实现同步读取 backend，作为 fallback；
4. 接入 `liburing`；
5. 实现单 page 异步读；
6. 实现 batch submit；
7. 加入 io depth 控制；
8. 跑通小规模 SIFT 数据集功能验证；
9. 输出同步读与异步读的基础对照结果。

验收标准：

- 异步读取结果正确；
- Recall@K 与同步版本一致或基本一致；
- 无明显崩溃、死锁、重复完成事件问题；
- 能输出基础 QPS 与延迟指标。

---

### 拓扑预取与性能评测

1. 在图搜索候选队列中加入预取触发点；
2. 实现邻居 page 预取；
3. 实现候选队列 top-N 预取；
4. 加入 page 去重与 pending 状态记录；
5. 控制 prefetch width，避免过度读取；
6. 增加预取命中率、无效预取率统计；
7. 对比不同参数组合；
8. 输出最终实验表格与分析结论。

验收标准：

- QPS 相比同步 SSD baseline 有提升；
- P95 / P99 延迟有下降；
- Recall@K 不下降；
- SSD Reads / Query 与 I/O Amplification 不出现不可控增长；
- 能明确说明哪些参数组合有效，哪些参数组合无效。

---

## 4. 实验设计

### 4.1 对照组

| 版本 | 说明 |
|---|---|
| sync-baseline | 同步 SSD 图搜索基线 |
| uring-single | io_uring 单请求异步读 |
| uring-batch | io_uring + batch submit |
| uring-prefetch | io_uring + batch submit + 拓扑预取 |

### 4.2 推荐实验参数

| 参数 | 建议取值 |
|---|---|
| `search_width` | 64 / 128 / 256 / 512 |
| `io_depth` | 32 / 64 / 128 |
| `io_batch_size` | 8 / 16 / 32 |
| `prefetch_width` | 8 / 16 / 32 |
| `top_k` | 10 |
| `query_limit` | 1000 起步，完整实验可扩大 |

### 4.3 实验记录模板

| 版本 | search_width | io_depth | batch_size | prefetch_width | Recall@10 | QPS | Avg Latency | P95 | P99 | SSD Reads / Query | Prefetch Hit Rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sync-baseline | 128 | - | - | - |  |  |  |  |  |  | - |
| uring-batch | 128 | 64 | 16 | 0 |  |  |  |  |  |  | - |
| uring-prefetch | 128 | 64 | 16 | 16 |  |  |  |  |  |  |  |

---

## 5. 风险与应对（立即反馈）

| 风险 | 表现 | 应对方案 |
|---|---|---|
| io_uring 没有提速 | QPS 不升反降 | 检查是否仍是单请求提交；有限度增加 batch size 和 io depth |
| 预取导致更慢 | SSD Reads / Query 明显上升 | 降低 prefetch_width，加入去重和缓存命中判断 |
| 代码复杂度上升 | 搜索逻辑与 I/O 逻辑耦合 | 抽象 PageReader，保持同步 fallback |
| P99 波动大 | 偶发长尾延迟 | 固定实验环境，使用 WSL ext4 或原生 Linux，避免 /mnt/d 路径 |
| Recall 下降 | 候选扩展逻辑被异步化破坏 | 异步只改变读取方式，不改变搜索语义 |

---

## 6. 最终交付物

本设计完成后应交付：

1. `io_uring` 异步读取模块；
2. batch submit 机制；
3. 拓扑预取策略；
4. page 去重与 pending 状态管理；
5. SSD 主路径 QPS、Avg/P50/P95/P99 延迟统计；
6. 同步基线与异步预取版本对照实验表；
7. 设计总结文档，说明性能提升来源与失败参数组合。

---

## 7. 结论写作模板

本设计围绕 SSD 图搜索路径中的随机读瓶颈，引入 `io_uring` 异步 I/O 框架，并实现 batch submit 与拓扑预取机制。实验重点不只是验证 `io_uring` 是否可用，而是验证批量提交、I/O depth 控制和下一跳预取是否能够降低图搜索过程中的同步等待时间。通过对比同步 SSD baseline、batch submit 版本和 topology prefetch 版本，可以量化异步预取对 QPS、平均延迟以及 P95/P99 尾延迟的影响。同时，通过 SSD Reads / Query、I/O Amplification 和 Prefetch Hit Rate 等指标，判断预取是否真正命中后续访问路径，避免将性能提升建立在不可控的 I/O 放大之上。
