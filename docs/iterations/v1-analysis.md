# AgentMem-Flow V1 分析报告

## 版本信息

- 版本：V1 - naive SSD 图检索基线
- 日期：2026-05-27
- 状态：已完成

## 版本目标

V1 的目标是建立 AgentMem-Flow 的第一个 DiskANN-style SSD 检索基线。它在 V0 的数据加载和指标体系之上，增加磁盘常驻图索引和 ANN 图遍历能力。

本版本的目标不是直接超过 DiskANN，而是提供一个可测量的 naive SSD baseline。后续 V2 的 Packed Page Layout、V3 的 Agent-Aware Cache 都需要和这个版本对比，证明随机 I/O 次数、尾延迟和读放大是否真正下降。

## 对应计划书问题

计划书位置：`docs/PROJECT_PLAN.md`。

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P0：验证方法不完整 | 部分解决 | V1 报告补充 Recall 定义、run 类型、ground truth 来源和 pass criteria；但 cold/warm 正式实验从 V2 起强制执行 |
| P2：缺少 naive SSD graph baseline | 已解决 | V1 实现 `--engine graph`、磁盘图文件、随机 node page 读取和 ANN 图遍历 |
| P3：one-node-per-page 布局导致随机读放大 | 暴露问题 | V1 保留 naive one-node-per-page 布局，并测得 `SSD Reads / Query avg = 176.6750`，为 V2 packed layout 提供对照 |
| P8：SIFT1M 不能依赖暴力搜索 | 部分解决 | V1 synthetic smoke 使用 V0 exact truth，但报告明确 SIFT1M 正式实验必须使用官方 `.ivecs` |
| P9：结果缺少配置、日志、环境、随机种子和 commit hash | 部分解决 | V1 已补充 config、log、result、build info 和验证方法快照；当前尚无 commit，因此记录为 `no commit yet` |

## 验证定义

- Recall@10：对每个 query，计算 graph ANN 返回 Top-10 与 ground truth Top-10 的交集比例，再对所有 query 取平均。
- 1-Recall@10：召回缺失率，定义为 `1 - Recall@10`。
- 本版本 smoke test ground truth 来源：V0 exact brute force。该方式只用于 synthetic smoke test。
- SIFT1M 验证规则：正式 SIFT1M 实验必须优先使用官方 `.ivecs`，不依赖 V0 暴力搜索。
- 本版本 run 类型：`smoke run`。由于当前 Windows 环境无法可靠清空系统 Page Cache，V1 归档结果不声明为严格 cold run。
- V2/V3 起，涉及 I/O 布局和缓存优化的实验必须正式区分 cold run 与 warm run。
- 全局验证规范：`docs/VALIDATION_METHOD.md`。

## 实现范围

新增内容：

- `NaiveDiskGraphBuilder`：用于实验数据集的精确 kNN 图构建器。
- `NaiveDiskGraphIndex`：二进制图文件读取器和随机 node page 访问接口。
- one-node-per-page 布局：默认每个向量节点和邻接表存入一个固定 4 KB page。
- `--engine graph` 命令行模式。
- 可配置参数：`--graph-degree`、`--search-width`、`--entry-count`、`--page-size`。
- resident sampled router：由 `--routing-sample-count` 控制，从常驻采样向量中为每个 query 选择更合适的图入口点。它是 V1 对后续 PQ/centroid router 的工程替代版本。
- 新增指标：`ssd_reads_per_query`、graph expansions/query、graph visited nodes/query、I/O amplification。

V1 未包含内容：

- 真正的 PQ 压缩。
- Vamana pruning。
- Packed page layout。
- Page cache。
- 异步 I/O。
- Delta index、WAL 或 compaction。

## 验证方式

构建命令：

`powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

Smoke test 命令：

`powershell -ExecutionPolicy Bypass -File .\scripts\run_v1_smoke.ps1`

主 synthetic 实验命令：

`.\build\agentmem_flow.exe --engine graph --synthetic --base-count 2000 --query-count 200 --dim 64 --clusters 32 --k 10 --index build\v1_graph_2k.idx --build-index --graph-degree 16 --search-width 128 --entry-count 48 --routing-sample-count 512`

运行环境：

- Windows PowerShell
- MinGW `g++` 8.1.0
- 当前环境未安装 CMake，因此已验证构建路径是 PowerShell `g++` 脚本。

Run 类型：

- 当前归档结果为 smoke run。
- 由于 graph index 在同一流程中 build 后立即 query，且未清空系统 Page Cache，因此不得把当前结果解释为 cold-run SSD 性能。

Ground Truth 来源：

- synthetic smoke：V0 exact brute force。
- SIFT1M 正式实验：必须使用官方 `.ivecs`。

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 可生成 `build/agentmem_flow.exe` |
| 指标输出完整 | 是 | 输出 Recall@10、QPS、latency、SSD reads/query、expanded/query、visited/query |
| Recall@10 达到 smoke 阈值 | 是 | 2026-05-28 smoke run 为 0.9990，高于 0.95 |
| 1-Recall@10 不超过 smoke 阈值 | 是 | 2026-05-28 smoke run 为 0.0010，低于 0.05 |
| resident router 有明显收益 | 是 | 关闭 router 的 2026-05-27 消融 Recall@10 为 0.5090，启用后为 0.9990 |
| SSD-style I/O 指标可观测 | 是 | `ssd_reads_per_query_avg = 134.2600` |
| 结果、配置、日志、环境信息已归档 | 是 | 已补充 `archive/configs`、`archive/logs`、`archive/build_info` |

## 指标结果

2026-05-28 按新验证规范补充的 V1 smoke run：

| 指标 | 数值 |
|---|---:|
| Recall@10 | 0.9990 |
| 1-Recall@10 | 0.0010 |
| QPS | 1241.2153 |
| Avg Latency ms | 0.7814 |
| P95 Latency ms | 1.0147 |
| P99 Latency ms | 1.3263 |
| SSD Reads / Query avg | 134.2600 |
| SSD Reads / Query P95 | 172.0500 |
| SSD Reads / Query P99 | 176.0400 |
| Graph Expansions / Query | 96.0000 |
| I/O Amplification reads/result | 13.4260 |

V1 主 synthetic 实验：

| 指标 | 数值 |
|---|---:|
| Recall@10 | 0.9985 |
| 1-Recall@10 | 0.0015 |
| QPS | 1086.1053 |
| Avg Latency ms | 0.8676 |
| P95 Latency ms | 1.0223 |
| P99 Latency ms | 1.1907 |
| SSD Reads / Query avg | 176.6750 |
| SSD Reads / Query P95 | 195.2500 |
| SSD Reads / Query P99 | 228.0600 |
| Graph Expansions / Query | 128.0000 |
| I/O Amplification reads/result | 17.6675 |

V1 smoke test：

| 指标 | 数值 |
|---|---:|
| Recall@10 | 0.9990 |
| QPS | 1534.8794 |
| Avg Latency ms | 0.6308 |
| P95 Latency ms | 0.8789 |
| P99 Latency ms | 1.0081 |
| SSD Reads / Query avg | 134.2600 |

同一 smoke workload 上的 router 消融实验：

| 变体 | Recall@10 | Avg Latency ms | SSD Reads / Query avg |
|---|---:|---:|---:|
| 启用 resident router | 0.9990 | 0.6308 | 134.2600 |
| 关闭 resident router | 0.5090 | 0.6470 | 152.8800 |

## 结果分析

V1 达到了预期目标：系统现在已经具备磁盘图索引文件、随机 node page 读取、ANN 图遍历和 SSD-style 指标统计能力。后续版本可以在不改变评测入口的前提下，逐步替换存储布局、缓存策略和 I/O 调度。

实验结果表明，高 Recall@10 强依赖 query-specific entry routing。关闭 resident router 后，同一 smoke workload 的 Recall@10 从 0.9990 降到 0.5090。这说明固定入口点不足以支撑稳定的 SSD ANN 检索，轻量常驻路由层是必要组件。

当前 resident router 会扫描均匀采样的全精度向量。这是 V1 的有意简化：它先保证图检索路径稳定，让 V2 能专注验证存储布局优化，而不是被入口点质量问题干扰。后续版本应将它替换为 PQ code、粗聚类 centroid 或 query signature router，以满足严格内存限制。

one-node-per-page 布局暴露了明显读放大：主 synthetic 实验平均每个 query 需要 176.6750 次 page read。这正是 V2 Co-Access Packed Page Layout 要优化的目标。

## 风险与局限

- 精确 kNN 构图复杂度为 O(N^2)，只适合 SIFT10K、SIFT100K 子集和 synthetic 开发 workload。
- resident router 使用采样全精度向量，而不是真正 PQ code，因此当前内存占用模型还不是最终版本。
- 当前图是 exact kNN graph，不是 Vamana-pruned graph，因此它是 baseline，而不是生产级 DiskANN 复现。
- I/O 使用 `std::ifstream` 随机读取，还没有实现 `O_DIRECT` 或 `io_uring`。
- 当前实验 workload 是聚类 synthetic 数据；后续需要在本地准备 SIFT 数据后补充 SIFT 实验。

## 归档结果

- `archive/results/v1-smoke-router-2026-05-27.txt`
- `archive/results/v1-ablation-no-router-2026-05-27.txt`
- `archive/results/v1-main-synthetic-2k-2026-05-27.txt`
- `archive/results/v1-exact-synthetic-2k-2026-05-27.txt`
- `archive/results/v1-smoke-2026-05-28.txt`
- `archive/configs/v1-smoke-2026-05-28.json`
- `archive/logs/v1-smoke-2026-05-28.log`
- `archive/build_info/v1-smoke-2026-05-28.txt`
- `archive/validation/validation-method-2026-05-28.md`

## 下一步

进入 V2：实现 Co-Access Packed Page Layout。V2 应保持 V1 的图结构和搜索算法不变，只改变磁盘布局，并对比 one-node-per-page 与 packed layout 在 Recall@10、平均延迟、P95/P99、SSD reads/query 上的差异。
