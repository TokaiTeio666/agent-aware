# AgentMem-Flow 版本分析报告模板

每次版本迭代完成后，都必须先按照本模板归档分析报告，再进入下一个版本。

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

- Recall@10：对每个 query，计算返回 Top-10 与 ground truth Top-10 的交集比例，然后对所有 query 取平均。
- 1-Recall@10：召回缺失率，定义为 `1 - Recall@10`。
- Ground Truth 来源：SIFT1M 等标准数据集优先使用官方 `.ivecs`；只有 synthetic 或小规模子集没有官方 ground truth 时，才允许使用 V0 暴力检索生成。
- Run 类型：cold run / warm run / smoke run。

## 实现范围

- 新增内容：
- 修改内容：
- 未包含内容：

## 验证方式

- 构建命令：
- 运行命令：
- 数据集 / workload：
- Run 类型：
- 随机种子：
- Ground Truth 来源：
- 运行环境：

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | | |
| 指标输出完整 | | |
| Recall@10 达到版本目标 | | |
| 版本核心指标达到目标 | | |
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
| Cache Hit Rate | N/A |
| SSD Reads / Query | N/A |
| Insert Latency | N/A |

## 结果分析

解释实验结果说明了什么，该版本是否达到目标，以及对后续版本有什么影响。

## 风险与局限

记录已知问题、测量偏差、工程债务或后续需要修正的假设。

## 归档结果

- `archive/results/...`
- `archive/configs/...`
- `archive/logs/...`
- `archive/build_info/...`

## 下一步

给出推荐的下一版本实现重点。
