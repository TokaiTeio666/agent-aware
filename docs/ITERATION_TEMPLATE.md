# AgentMem-Flow 版本分析报告模板

每次版本迭代完成后，优先按本模板归档分析报告。当前模板默认采用 `SIFT10K` / `SIFT100K` 正式验证口径。

## 版本信息

- 版本：
- 日期：
- 负责人：
- 状态：

## 版本目标

说明该版本要验证什么问题，以及它在整体路线中的作用。

## 对应计划书问题

参考 `docs/PROJECT_PLAN.md` 中的问题编号，说明本版本解决、部分解决或暴露了哪些问题。

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P? | 已解决 / 部分解决 / 暴露问题 | |

## 验证定义

- Recall@10：对每个 query，计算返回 Top-10 与 ground truth Top-10 的交集比例，再对所有 query 取平均。
- 1-Recall@10：定义为 `1 - Recall@10`。
- Ground Truth 来源：当前正式验证优先使用 `data/sift/` 下的真实 SIFT 文件；若 `sift_groundtruth.ivecs` 对子集越界，必须记录回退到 subset exact brute force truth。
- 数据集：正式验证只使用 `SIFT10K` 或 `SIFT100K`。

## 实现范围

- 新增内容：
- 修改内容：
- 未包含内容：

## 验证方式

- 构建命令：
- 运行命令：
- 数据集：`SIFT10K` / `SIFT100K`
- Ground Truth 来源：
- I/O mode requested：
- I/O mode effective：
- 运行环境：
- 内存预算参数：

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | | |
| 指标输出完整 | | |
| `memory_budget_pass=1` | | |
| `memory_resident_ratio <= 0.20` | | |
| Recall@10 达到版本目标 | | |
| 结果、配置、日志、环境信息已归档 | | |

## 指标结果

| 指标 | 数值 |
|---|---:|
| Recall@10 | |
| 1-Recall@10 | |
| QPS | |
| Avg Latency ms | |
| P95 Latency ms | |
| P99 Latency ms | |
| SSD Reads / Query | |
| memory_budget_bytes | |
| memory_resident_bytes | |
| memory_resident_ratio | |

## 结果分析

解释实验结果说明了什么，该版本是否达到目标，以及对后续版本的影响。

## 风险与局限

记录已知问题、测量偏差、工程债务或后续需要修正的假设。

## 归档结果

- `archive/results/...`
- `archive/logs/...`
- 如有保留：`archive/configs/...`
- 如有保留：`archive/build_info/...`

## 下一步

给出推荐的下一版本实现重点。
