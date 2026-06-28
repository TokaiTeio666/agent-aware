# 03 异步预取设计文档

## 当前实现状态（2026-06-22）

核心能力已经进入 SSD 查询主路径：`AsyncPageReader` 提供同步读、异步读、按 `io_batch` 切分的批量提交和 io_uring fallback；`QueryPageSession` 负责 pending/ready page 管理和 ready/pending/unused 统计；`PackedDiskGraphIndex` 只保留 PQ+ADC 候选生成，`PrefetchPlanner` 做 page-level 聚合、XGBoost 收益打分、top-K/threshold/io_depth 限流后再提交；`agent-aware` 可通过 `--io-mode`、`--io-depth`、`--io-batch`、`--prefetch-policy none|xgboost`、`--prefetch-model`、`--prefetch-top-k`、`--prefetch-score-threshold`、`--prefetch-max-inflight`、`--prefetch-trace` 调整。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| io_uring 接入 | 已完成可回退 | `AsyncPageReader::configure/status` |
| 批量提交 | 已完成 | `AsyncPageReader::batch_submit` |
| XGBoost 预取 | 已完成 | `PrefetchPlanner`、`QueryPageSession::submit_next_hop_prefetch` 提交底座；外部只保留 `--prefetch-policy xgboost` |
| 预取指标 | 已完成 | `prefetch_ready_hit/pending_hit/unused`、`io_prefetch_*` |
| 大规模参数扫描 | 部分完成 | SIFT1M 主路径已跑通，完整矩阵见参数调优计划 |

说明：在搜索扩展集合不变时，预取只能把未来 demand read 提前提交；`submitted_reads` 的理论下界等于无预取时的读页集合。要让 `submitted_reads/query` 低于 no-prefetch 基线，需要进一步改变搜索层的候选扩展页集合，例如页感知 beam/coalesced expansion，而不是只调整预取候选。

## 设计背景

本设计用于在现有 SSD 图搜索主路径上接入 `io_uring`，实现批量异步 I/O 提交，并结合图拓扑访问规律进行预取，降低随机读等待时间，提高查询吞吐量与尾延迟表现。

本设计不是单纯替换同步 `pread` 为 `io_uring`，而是要围绕图搜索的访问模式建立一套可评测、可回退、可对照的异步读取机制。重点解决当前图搜索中大量小粒度随机读、同步等待、I/O depth 不足、下一跳访问缺少提前准备等问题。

## 核心目标

异步预取需要完成：

1. 在 SSD 图索引读取路径中引入 `io_uring`，封装统一的异步读接口。
2. 完成 batch submit 批量提交机制，让多个 page read 合并提交。
3. 实现拓扑预取机制，在正式扩展前提前提交部分未来可能访问的 page。
4. 建立 SSD 主路径 QPS、Avg/P50/P95/P99、SSD Reads/Query、I/O Amplification、Prefetch Hit Rate、Recall@K 等指标体系。
5. 保留同步读取 fallback 路径，保证 `io_uring` 不可用时系统仍能运行。

## 相关实现文件

相关源码入口如下：

```text
include/core/async_page_reader.h
src/core/async_page_reader.cpp
include/core/query_page_session.h
src/core/query_page_session.cpp
include/core/prefetch_planner.h
src/core/prefetch_planner.cpp
include/graph/disk_graph_index.h
src/graph/disk_graph_index.cpp
src/sift_search_benchmark.cpp
```

同时阅读项目中已有的 SSD page reader、DirectIO、packed graph、PQ/ADC 候选生成和 benchmark 输出逻辑。

## 具体设计要求

## 一、接入 io_uring 异步 I/O 框架

在 SSD 图索引读取路径中引入 `io_uring`，封装统一的异步读接口，避免搜索逻辑直接依赖底层系统调用。

主要工作包括：

1. 初始化 `io_uring` 队列。
2. 支持异步提交读取请求。
3. 支持批量提交 batch submit。
4. 支持批量回收完成事件。
5. 保留同步读取 fallback 路径。
6. 对读取失败、短读、越界读进行错误处理。

预期产出：

1. `AsyncPageReader` 或等价模块。
2. 同步读 / 异步读可通过参数切换。
3. 基础功能验证能够确认读取结果正确。

## 二、batch submit 批量提交机制

图搜索过程中会产生多个待访问节点，如果每次只提交一个 I/O 请求，`io_uring` 的优势无法体现。因此需要将多个 page read 合并为批量提交。

主要工作包括：

1. 维护 pending read 队列。
2. 达到一定数量后批量提交。
3. 支持配置 batch size。
4. 控制 in-flight I/O 深度。
5. 避免重复提交同一 page。
6. 统计每次 query 的提交次数、完成次数、平均 batch 大小。

建议配置项：

```text
async_io = 1
io_backend = uring
io_depth = 32 / 64 / 128
io_batch_size = 8 / 16 / 32
```

预期产出：

1. 批量提交逻辑。
2. I/O depth 控制逻辑。
3. batch size 对 QPS 与延迟影响的实验结果。

## 三、拓扑预取机制

图搜索具有一定的拓扑访问规律：当前扩展节点的邻居、候选队列中距离较近的节点、hub 节点周边邻居，都可能在后续步骤被访问。因此可以在正式扩展前提前提交部分读取请求。

主要工作包括：

1. 根据候选队列选择下一批可能访问节点。
2. 对当前扩展节点的邻居进行预取。
3. 避免对已访问、已缓存、已提交节点重复预取。
4. 控制预取宽度，防止无效 I/O 放大。
5. 将预取策略与 page cache/path cache 结合。

建议配置项：

```text
prefetch = topology
prefetch_width = 8 / 16 / 32
prefetch_depth = 1
dedup_pages = 1
```

预期产出：

1. 拓扑预取策略。
2. page 去重机制。
3. 预取命中率统计。
4. 预取带来的 I/O 放大统计。

## 四、SSD 主路径统计

本设计必须用实验数据证明异步预取是否有效，不能只停留在代码接入层面。

需要统计的核心指标包括：

| 指标 | 含义 | 目标 |
| --- | --- | --- |
| QPS | 每秒处理 query 数 | 相比同步 SSD 基线提升 |
| Avg Latency | 平均查询延迟 | 下降 |
| P50 Latency | 中位数延迟 | 下降或基本持平 |
| P95 Latency | 95 分位延迟 | 明显下降 |
| P99 Latency | 99 分位延迟 | 重点优化对象 |
| SSD Reads / Query | 每个 query 的 SSD 读取次数 | 不显著上升 |
| I/O Amplification | 实际读取量 / 有效访问量 | 可控 |
| Prefetch Hit Rate | 预取命中率 | 越高越好 |
| Recall@K | 检索召回率 | 不低于同步 SSD 基线 |

## 五、实施步骤

io_uring 接入与 batch submit 阶段：

1. 梳理当前 SSD page 读取路径。
2. 抽象统一 PageReader 接口。
3. 实现同步读取 backend，作为 fallback。
4. 接入 `liburing`。
5. 实现 single page 异步读。
6. 实现 batch submit。
7. 加入 io depth 控制。
8. 跑通小规模 SIFT 数据集功能验证。
9. 输出同步读与异步读的基础对照结果。

拓扑预取与性能评测阶段：

1. 在图搜索候选队列中加入预取触发点。
2. 实现邻居 page 预取。
3. 实现候选队列 top-N 预取。
4. 加入 page 去重与 pending 状态记录。
5. 控制 prefetch width，避免过度读取。
6. 增加预取命中率、无效预取率统计。
7. 对比不同参数组合。
8. 输出最终实验表格与分析结论。

## 六、实验设计

对照组：

| 版本 | 说明 |
| --- | --- |
| sync-baseline | 同步 SSD 图搜索基线 |
| uring-single | io_uring 单请求异步读 |
| uring-batch | io_uring + batch submit |
| uring-prefetch | io_uring + batch submit + 拓扑预取 |

推荐实验参数：

| 参数 | 建议取值 |
| --- | --- |
| `search_width` | 64 / 128 / 256 / 512 |
| `io_depth` | 32 / 64 / 128 |
| `io_batch_size` | 8 / 16 / 32 |
| `prefetch_width` | 8 / 16 / 32 |
| `top_k` | 10 |
| `query_limit` | 1000 起步，完整实验可扩大 |

实验记录模板：

| 版本 | search_width | io_depth | batch_size | prefetch_width | Recall@10 | QPS | Avg Latency | P95 | P99 | SSD Reads / Query | Prefetch Hit Rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| sync-baseline | 128 | - | - | - |  |  |  |  |  |  | - |
| uring-batch | 128 | 64 | 16 | 0 |  |  |  |  |  |  | - |
| uring-prefetch | 128 | 64 | 16 | 16 |  |  |  |  |  |  |  |

## 七、风险与应对

| 风险 | 表现 | 应对方案 |
| --- | --- | --- |
| io_uring 没有提速 | QPS 不升反降 | 检查是否仍是单请求提交；有限度增加 batch size 和 io depth |
| 预取导致更慢 | SSD Reads / Query 明显上升 | 降低 prefetch_width，加入去重和缓存命中判断 |
| 代码复杂度上升 | 搜索逻辑与 I/O 逻辑耦合 | 抽象 PageReader，保持同步 fallback |
| P99 波动大 | 偶发长尾延迟 | 固定实验环境，使用 WSL ext4 或原生 Linux，避免 `/mnt/d` 路径 |
| Recall 下降 | 候选扩展逻辑被异步化破坏 | 异步只改变读取方式，不改变搜索语义 |

## 验收标准

最终实现应满足：

1. 异步读取结果正确。
2. Recall@K 与同步版本一致或基本一致。
3. 无明显崩溃、死锁、重复完成事件问题。
4. 能输出基础 QPS 与延迟指标。
5. QPS 相比同步 SSD baseline 有提升。
6. P95 / P99 延迟有下降。
7. SSD Reads / Query 与 I/O Amplification 不出现不可控增长。
8. 能明确说明哪些参数组合有效，哪些参数组合无效。
9. 交付 `io_uring` 异步读取模块。
10. 交付 batch submit 机制。
11. 交付拓扑预取策略。
12. 交付 page 去重与 pending 状态管理。
13. 交付同步基线与异步预取版本对照实验表。
14. 设计总结文档应说明性能提升来源与失败参数组合。
