# AgentMem-Flow V0 分析报告

## 版本信息

- 版本：V0 - 内存精确向量检索基线
- 日期：2026-05-27
- 状态：已完成

## 版本目标

V0 的目标是建立一个可运行、可复现、可作为正确性参照的精确检索基线。后续 SSD 图索引、缓存优化、动态写入等版本都需要和它对齐 Recall@10、延迟和 QPS 指标。

本版本故意采用暴力精确检索，而不是近似索引。这样后续近似 ANN 版本可以把 V0 的结果作为 ground truth，避免在系统优化过程中失去正确性参照。

## 对应计划书问题

计划书位置：`docs/PROJECT_PLAN.md`。

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P0：验证方法不完整 | 部分解决 | V0 建立了最小验证闭环，输出 Recall@10、QPS 和 latency 指标；后续已补充统一验证规范 `docs/VALIDATION_METHOD.md` |
| P1：缺少精确正确性基线 | 已解决 | V0 实现 exact brute force Top-K，可作为 synthetic 和小规模数据集的 ground truth |
| P8：SIFT1M 不能依赖暴力搜索 | 暴露问题 | V0 可用于小规模 exact truth，但报告明确 SIFT1M 正式实验应使用官方 `.ivecs` |
| P9：结果缺少配置、日志、环境、随机种子和 commit hash | 部分解决 | V0 已补充 `archive/configs`、`archive/logs`、`archive/build_info` 和验证方法快照 |

## 验证定义

- Recall@10：对每个 query，计算系统返回 Top-10 与 ground truth Top-10 的交集比例，再对所有 query 取平均。
- 1-Recall@10：召回缺失率，定义为 `1 - Recall@10`。
- 本版本 ground truth 来源：`exact_self`，仅用于 V0 smoke test 自检。
- 本版本 run 类型：`smoke run`。V0 是纯内存精确检索，不用于验证 cold/warm I/O 行为。
- 全局验证规范：`docs/VALIDATION_METHOD.md`。

## 实现范围

新增内容：

- C++17 项目骨架。
- 内存向量容器 `VectorSet`。
- SIFT `fvecs` base/query 加载器。
- SIFT `ivecs` ground-truth 加载器。
- 聚类 synthetic 数据生成器。
- 基于 squared L2 distance 的精确暴力 Top-K 检索。
- Recall@K、QPS、平均延迟、P50、P95、P99 指标统计。
- PowerShell 构建脚本和 smoke test 脚本。

V0 未包含内容：

- 图 ANN 索引。
- SSD 存储布局。
- Page cache。
- Query path cache。
- Delta index。
- WAL 或 compaction。

## 验证方式

- 构建命令：`powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`
- 运行命令：`powershell -ExecutionPolicy Bypass -File .\scripts\run_v0_smoke.ps1`
- 数据集 / workload：聚类 synthetic 向量，1000 个 base vectors，100 个 queries，维度 32，16 个聚类，Top-10。
- Run 类型：smoke run。
- 随机种子：42。
- Ground Truth 来源：`exact_self`。
- 运行环境：Windows PowerShell，MinGW `g++` 8.1.0。当前环境未安装 CMake，因此验证路径使用直接 `g++` 构建脚本。

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 可生成 `build/agentmem_flow.exe` |
| 指标输出完整 | 是 | 输出 Recall@10、QPS、Avg/P50/P95/P99 latency |
| Recall@10 = 1.0 | 是 | V0 exact self-check 达到 1.0000 |
| 1-Recall@10 = 0 | 是 | 召回缺失率为 0.0000 |
| build/run/archive 流程正常 | 是 | 已归档报告、结果、配置、日志、build info 和验证方法 |

## 指标结果

2026-05-28 按新验证规范补充的 smoke run：

| 指标 | 数值 |
|---|---:|
| Recall@10 | 1.0000 |
| 1-Recall@10 | 0.0000 |
| QPS | 26634.7050 |
| Avg Latency ms | 0.0375 |
| P50 Latency ms | 0.0360 |
| P95 Latency ms | 0.0495 |
| P99 Latency ms | 0.0583 |
| Cache Hit Rate | N/A |
| SSD Reads / Query | N/A |
| Insert Latency | N/A |

## 结果分析

V0 达到了预期目标：项目现在具备一个可运行的精确检索 baseline，并且输出格式已经包含后续版本需要复用的核心指标。

本次 smoke test 中 `truth=exact_self`，因此 Recall@10 为 1.0。这表示检索和指标链路工作正常，但不能作为近似索引的真实召回率证明。后续 V1 及之后版本需要使用 SIFT ground-truth 文件，或者用 V0 暴力检索结果作为对照。

当前 benchmark 规模很小，而且是纯 CPU 内存检索。它的延迟和 QPS 只用于证明评测链路、Top-K 排序和指标输出是正确的，不应作为最终性能结论。

## 风险与局限

- smoke test 数据规模太小，不能代表 SIFT100K/SIFT1M。
- `truth=exact_self` 只适合自检，不适合近似索引 Recall 结论。
- 暴力检索目前是单线程实现，可以作为正确性基线，但不是吞吐性能基线。
- 当前环境没有安装 CMake，因此已验证路径是基于 PowerShell 的 `g++` 构建脚本。

## 归档结果

- `archive/results/v0-smoke-2026-05-27.txt`
- `archive/results/v0-smoke-2026-05-28.txt`
- `archive/configs/v0-smoke-2026-05-28.json`
- `archive/logs/v0-smoke-2026-05-28.log`
- `archive/build_info/v0-smoke-2026-05-28.txt`
- `archive/validation/validation-method-2026-05-28.md`

## 下一步

进入 V1：实现一个 DiskANN-style 的 SSD 图检索 baseline。V1 应复用 V0 的数据加载和指标统计代码，用于比较 Recall@10、延迟和 QPS。
