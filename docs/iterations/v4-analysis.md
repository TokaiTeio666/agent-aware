# AgentMem-Flow V4 分析报告

## 版本信息

- 版本：V4 - Query Path Cache
- 日期：2026-05-28
- 状态：已完成

## 版本目标

V4 的目标是利用 Agent memory 场景中的连续相似查询特征，复用历史查询路径信息，减少重复图遍历。具体做法是为 query 生成 signature，命中后复用历史 seeds 和 Top-K ids，并降低命中查询的 search width。

本版本还新增 Agent-style synthetic workload，用于模拟同一会话内连续查询集中在同一语义簇的情况。

## 对应计划书问题

计划书位置：`docs/PROJECT_PLAN.md`。

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P5：相似 query 重复走相似图路径 | 部分解决 | V4 实现 Query Path Cache，path cache hit rate 达到 0.5533 |
| P4：缓存缺少路径热度 | 部分解决 | V4 产生 path hit 信号，后续可反馈给 Agent-aware cache |
| P0：验证方法不完整 | 部分解决 | V4 增加 Agent-style workload、path cache hit rate 和 pass criteria |
| P9：归档不完整 | 部分解决 | V4 已归档配置、日志、结果和 build info |

## 验证定义

- Recall@10：系统返回 Top-10 与 ground truth Top-10 的交集比例，对所有 query 取平均。
- 1-Recall@10：`1 - Recall@10`。
- Ground Truth 来源：synthetic workload 使用 V0 exact brute force。
- Run 类型：warm run，执行 1 轮 warmup 后记录正式指标。
- Workload：Agent-style synthetic，`session_length = 10`。
- 全局验证规范：`docs/VALIDATION_METHOD.md`。

## 实现范围

新增内容：

- `--synthetic-workload random|agent`。
- `--session-length`。
- query signature：基于 resident routed entry。
- `--path-cache-policy none|reuse`。
- `--path-cache-capacity`。
- `--path-cache-hit-search-width`。
- path cache hit/request 指标。
- 命中时复用历史 seeds 和 Top-K ids。
- 命中时降低 search width。
- `scripts/run_v4_path_compare.ps1`。

未包含内容：

- SimHash/PQ prefix query signature。
- 完整 frontier/path 复用。
- 与 Agent-aware cache 的 path hotness 联动。
- 真实 Agent trace。

## 验证方式

构建命令：

`powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

对比脚本：

`powershell -ExecutionPolicy Bypass -File .\scripts\run_v4_path_compare.ps1`

主实验 workload：

- synthetic dataset
- synthetic_workload = agent
- session_length = 10
- base_count = 2000
- query_count = 300
- dim = 64
- clusters = 32
- seed = 42
- layout = packed
- packing = coaccess
- cache_policy = agent
- cache_pages = 24
- path_cache_capacity = 128
- path_cache_hit_search_width = 96

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 可生成 `build/agentmem_flow.exe` |
| path cache hit rate 可观测 | 是 | hit rate 为 0.5533 |
| Recall@10 不明显下降 | 是 | no path cache 和 path cache 均为 0.9987 |
| graph expanded/query 下降 | 是 | 128.0000 降到 110.2933 |
| graph visited/query 下降 | 是 | 176.3000 降到 155.4767 |
| SSD reads/query 下降 | 是 | 14.7933 降到 13.8400 |
| P99 latency 不升高 | 是 | 0.3477 ms 降到 0.3309 ms |
| 结果、配置、日志、环境信息已归档 | 是 | 已补充 `archive/configs`、`archive/logs`、`archive/build_info` |

## 指标结果

Warm run 对比：

| 变体 | Recall@10 | Path Hit Rate | Avg ms | P95 ms | P99 ms | SSD Reads / Query | Expanded / Query | Visited / Query |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| no path cache | 0.9987 | 0.0000 | 0.1868 | 0.2403 | 0.3477 | 14.7933 | 128.0000 | 176.3000 |
| path cache reuse | 0.9987 | 0.5533 | 0.1753 | 0.2484 | 0.3309 | 13.8400 | 110.2933 | 155.4767 |

改进幅度：

```text
ssd_reads/query reduction = 6.44%
avg_latency reduction = 6.16%
p99_latency reduction = 4.83%
expanded/query reduction = 13.83%
visited/query reduction = 11.81%
```

## 结果分析

V4 达到了最小可实现目标：在 Agent-style 连续查询 workload 下，Query Path Cache 产生 0.5533 的命中率，并在 Recall@10 不下降的前提下降低 graph expansions、visited nodes 和 SSD reads/query。

本版本的 path cache 复用粒度仍较轻：命中后复用历史 seeds 和 Top-K ids，而不是完整 frontier。即便如此，命中查询使用较小 search width 后，平均 expanded/query 从 128.0000 降到 110.2933，说明相似 query 的入口点和候选集合确实具有复用价值。

P95 latency 略有波动，说明当前 signature 和 hit-search-width 还需要更精细调参；但 P99、平均延迟和 reads/query 均下降，且 Recall@10 保持 0.9987。后续 V4+ 可以将 signature 升级为 coarse centroid + PQ prefix，并把 path hotness 反馈给 V3 Agent-aware cache。

## 风险与局限

- 当前 query signature 只基于 resident routed entry，粒度较粗。
- 当前 path cache 只复用 seeds 和 Top-K ids，没有复用完整 candidate frontier。
- 当前 workload 是 synthetic agent workload，不是真实 Agent trace。
- P95 latency 有轻微波动，说明命中后的 search width 还需自适应调整。

## 归档结果

- `archive/results/v4-path-2026-05-28.txt`
- `archive/configs/v4-path-2026-05-28.json`
- `archive/logs/v4-path-2026-05-28.log`
- `archive/build_info/v4-path-2026-05-28.txt`
- `archive/validation/validation-method-2026-05-28.md`

## 下一步

进入 V5：实现 Delta Index + WAL + Top-K Merge，并补充混合读写 workload。若时间允许，再实现 SLA-aware compaction 限速，用于降低读写混合场景下的 P99 抖动。

