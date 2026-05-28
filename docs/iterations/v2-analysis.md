# AgentMem-Flow V2 分析报告

## 版本信息

- 版本：V2 - Co-Access Packed Page Layout
- 日期：2026-05-28
- 状态：已完成

## 版本目标

V2 的目标是解决 V1 暴露出的 one-node-per-page 随机读放大问题。V1 中每访问一个图节点就读取一个 4 KB page，主 synthetic 实验平均 `SSD Reads / Query = 176.6750`。V2 将多个高共访问概率节点打包进同一个 page，使一次 page read 可以服务多个后续图遍历访问。

本版本同时完成加分项：

- 实现 random、BFS、co-access 三种 packing 策略。
- 实现基于 Agent-style synthetic trace 的 co-access score。
- 增加 cold/warm run 标识和 warmup 支持。

## 对应计划书问题

计划书位置：`docs/PROJECT_PLAN.md`。

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P0：验证方法不完整 | 部分解决 | V2 增加 run type、warmup、pass criteria，并在报告中区分 cold-like 与 warm run |
| P3：one-node-per-page 布局导致随机读放大 | 已解决 | co-access packed layout 将 `SSD Reads / Query avg` 从 176.6750 降到 21.4100 |
| P9：结果缺少配置、日志、环境、随机种子和 commit hash | 部分解决 | V2 已归档 config、log、result、build info；当前尚无 commit，因此记录为 `no commit yet` |

## 验证定义

- Recall@10：对每个 query，计算 graph ANN 返回 Top-10 与 ground truth Top-10 的交集比例，再对所有 query 取平均。
- 1-Recall@10：召回缺失率，定义为 `1 - Recall@10`。
- Ground Truth 来源：synthetic 实验使用 V0 exact brute force。SIFT1M 正式实验仍必须优先使用官方 `.ivecs`。
- Run 类型：本次归档包含 `cold` 和 `warm` 两类标识。由于当前 Windows 环境无法可靠清空系统 Page Cache，`cold` 结果应解释为 cold-like run，不作为严格 cold SSD 结论。
- 当前数据集：V2 归档结果使用 synthetic clustered workload，不是 SIFT。参数为 base_count=2000、query_count=200、dim=64、clusters=32、seed=42。
- 严格 cold start：已补充 WSL/Linux 脚本 `scripts/linux/run_v2_strict_cold_warm.sh`，在每次 cold run 前执行 `sync` 和 `drop_caches`。
- 全局验证规范：`docs/VALIDATION_METHOD.md`。

## 实现范围

新增内容：

- `PackedDiskGraphBuilder`：构建 packed page 图索引。
- `PackedDiskGraphIndex`：读取 packed page 图索引并执行图检索。
- packed graph header、node-to-page directory、page records。
- page-level SoA：page 内分离存储 node ids、degrees、vectors、neighbors。
- `--layout one-node|packed`。
- `--packing random|bfs|coaccess`。
- `--run-type smoke|cold|warm`。
- `--warmup-runs N`。
- `--coaccess-sessions` 和 `--coaccess-trace-length`。
- `scripts/run_v2_compare.ps1`。

未包含内容：

- 全局应用内 page cache。V2 只统计单次 query 内 page read 去重。
- 已提供 WSL/Linux 严格 cold run 脚本，但当前归档结果还未在 WSL ext4 环境下复跑。
- 真实 Agent trace 输入；当前使用 synthetic trace 模拟 Agent 会话路径。
- PQ router；仍沿用 V1 的 resident sampled router。

## 验证方式

构建命令：

`powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

对比脚本：

`powershell -ExecutionPolicy Bypass -File .\scripts\run_v2_compare.ps1`

主实验 workload：

- synthetic dataset
- base_count = 2000
- query_count = 200
- dim = 64
- clusters = 32
- seed = 42
- k = 10
- graph_degree = 16
- search_width = 128
- entry_count = 48
- routing_sample_count = 512
- page_size = 4096
- coaccess_sessions = 96
- coaccess_trace_length = 48

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 可生成 `build/agentmem_flow.exe` |
| 指标输出完整 | 是 | 输出 Recall、1-Recall、latency、SSD reads/query、visited/query、expanded/query |
| packed layout 的 SSD reads/query 低于 one-node-per-page | 是 | co-access 为 21.4100，one-node 为 176.6750 |
| co-access packing 优于 random packing | 是 | co-access 为 21.4100，random 为 111.2350 |
| Recall@10 不明显下降 | 是 | one-node 与 co-access 均为 0.9985 |
| cold/warm 结果已区分 | 是 | 已记录 cold-like 和 warm run；严格 cold 需 Linux 环境补充 |
| 结果、配置、日志、环境信息已归档 | 是 | 已补充 `archive/configs`、`archive/logs`、`archive/build_info` |

## 指标结果

主实验 cold-like 对比：

| 布局 | Packing | Recall@10 | 1-Recall@10 | Avg ms | P95 ms | P99 ms | SSD Reads / Query avg |
|---|---|---:|---:|---:|---:|---:|---:|
| one-node | none | 0.9985 | 0.0015 | 0.9236 | 1.1803 | 1.3063 | 176.6750 |
| packed | random | 0.9985 | 0.0015 | 1.3132 | 1.5153 | 1.8603 | 111.2350 |
| packed | bfs | 0.9985 | 0.0015 | 0.2845 | 0.3712 | 0.4743 | 21.0200 |
| packed | coaccess | 0.9985 | 0.0015 | 0.2729 | 0.3334 | 0.4180 | 21.4100 |

Warm run 对比：

| 布局 | Packing | Recall@10 | Avg ms | P95 ms | P99 ms | SSD Reads / Query avg |
|---|---|---:|---:|---:|---:|---:|
| one-node | none | 0.9985 | 0.9519 | 1.1759 | 1.3683 | 176.6750 |
| packed | coaccess | 0.9985 | 0.2884 | 0.3530 | 0.3763 | 21.4100 |

读放大降低：

```text
one-node reads/query = 176.6750
coaccess reads/query = 21.4100
reduction = 87.88%
```

## 结果分析

V2 达到了主要目标：在 Recall@10 不下降的前提下，co-access packed layout 将平均 page reads/query 从 176.6750 降到 21.4100，读放大下降约 87.88%。这说明将图遍历中高概率连续访问的节点打包到同一 page，确实可以显著降低随机 I/O 次数。

random packing 虽然也把多个节点放进一个 page，但缺乏访问局部性建模，只能降到 111.2350 reads/query，明显弱于 BFS 和 co-access。这证明“打包”本身不够，关键是打包顺序要与图遍历访问路径相关。

BFS packing 在当前 synthetic workload 上的 reads/query 略低于 co-access packing：21.0200 vs 21.4100。原因是当前 synthetic 数据按 cluster 规律生成，exact kNN 图结构较规整，BFS 已经能很好捕捉图邻接局部性。不过 co-access 的 P99 latency 更低：0.4180 ms vs 0.4743 ms，并且明显优于 random。后续可以把 synthetic trace 替换为真实 query path trace，以进一步体现 Agent memory 场景差异。

Warm run 中 co-access packed layout 的 P99 latency 为 0.3763 ms，明显低于 one-node warm run 的 1.3683 ms。由于当前环境无法严格控制 OS Page Cache，这里的 cold/warm 主要用于流程规范和趋势观察，正式论文式结论应在 Linux 上补充 drop cache 后的 cold run。

## 风险与局限

- 当前 cold run 是 cold-like，因为 Windows 环境无法可靠清空系统 Page Cache。
- 如果在 WSL 的 `/mnt/c` 路径下运行，`drop_caches` 可以清 Linux Page Cache，但 I/O 行为仍可能受 Windows 文件系统转接层影响；正式实验更推荐在 WSL ext4 路径下运行。
- co-access trace 是 synthetic graph walk，不是真实 Agent memory trace。
- exact kNN 构图仍是 O(N^2)，大规模 SIFT1M 需要更高效构图或离线索引输入。
- packed page 当前只做单 query 内 page read 去重，还没有全局 page cache。
- BFS 在当前 synthetic 数据上的 reads/query 略优于 co-access，说明 co-access score 还需要结合真实 query trace 继续优化。

## 归档结果

- `archive/results/v2-main-2026-05-28.txt`
- `archive/configs/v2-main-2026-05-28.json`
- `archive/logs/v2-main-2026-05-28.log`
- `archive/build_info/v2-main-2026-05-28.txt`
- `archive/validation/validation-method-2026-05-28.md`

## 下一步

进入 V3：实现 Agent-Aware Cache。V3 应在 V2 packed layout 的基础上增加应用内 page cache，并对比 no cache、LRU cache 和 Agent-aware cache，在 cold/warm workload 下记录 cache hit rate、P95/P99 latency 和 SSD reads/query。
