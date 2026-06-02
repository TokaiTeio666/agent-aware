# AgentMem-Flow V7 分析报告

## 版本信息

- 版本：V7 - Query Signature Policy 对比
- 日期：2026-05-29
- 状态：已完成本地 synthetic warm/smoke 验证

## 版本目标

V7 的目标是把 V4 中已经具备的 Query Path Cache 签名能力单独归档成一次可复现对比，验证 `routed`、`simhash`、`pq-prefix` 和 `simhash-pq` 四种 query signature 对路径复用命中率、SSD reads/query 和尾延迟的影响。

该版本不声称解决 SIFT 大规模性能问题，只用于确认 Agent-style 连续相似查询下，签名策略确实能让路径缓存更稳定地命中。

## 对应计划书问题

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P5：相似 query 重复走相似图路径 | 部分解决 | 对比四种 signature，验证路径缓存命中率和图扩展数下降 |
| P4：缓存不理解 Agent 访问局部性 | 部分解决 | signature 将相似 query 映射到可复用路径，给 path hotness 提供 key |
| P0：验证方法不完整 | 部分解决 | 补齐 V7 pass criteria、结果归档和本地 warm run 标注 |
| P9：归档不完整 | 部分解决 | 补齐 V7 result/config/log/build_info/archive report |

## 实现范围

新增内容：

- `scripts/run_v7_signature_compare.ps1`。
- V7 归档配置、日志、环境信息和分析报告。

继承内容：

- V4 Query Path Cache。
- `--query-signature-policy routed|simhash|pq-prefix|simhash-pq`。
- `--simhash-bits`。
- `--pq-prefix-subspaces`、`--pq-prefix-centroids`、`--pq-prefix-train-iterations`。

未包含内容：

- SIFT100K/SIFT1M 正式 cold/warm 对比。
- 跨进程持久化 path cache。
- 完整 frontier 复用；当前仍复用 seeds 和 Top-K ids。

## 验证方式

构建与运行命令：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_v7_signature_compare.ps1
```

数据集 / workload：

- synthetic agent workload。
- `base_count=2000`，`query_count=300`，`dim=64`，`clusters=32`。
- `session_length=10`。
- `layout=packed`，`packing=coaccess`。
- `cache_policy=agent`，`cache_pages=24`。
- `run_type=warm`，`warmup_runs=1`。
- Ground truth：exact brute force。
- 随机种子：42。

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 成功生成 `build/agentmem_flow.exe` |
| 指标输出完整 | 是 | 五个变体均输出 Recall、latency、cache、path cache 和 SSD read 指标 |
| Recall@10 不明显下降 | 是 | 五个变体均为 0.9987 |
| Path cache 命中率可观测 | 是 | `routed` 为 0.5533，SimHash/PQ 系列为 1.0000 |
| SSD reads/query 下降 | 是 | 无 path cache 为 19.7000，最佳约 16.6300 |
| 结果、配置、日志、环境信息已归档 | 是 | 见归档结果 |

## 指标结果

| 变体 | Recall@10 | P99 ms | SSD Reads / Query | Path Hit Rate | Expanded / Query |
|---|---:|---:|---:|---:|---:|
| no_path_cache | 0.9987 | 0.2870 | 19.7000 | 0.0000 | 128.0000 |
| path_cache_routed | 0.9987 | 0.2852 | 18.2800 | 0.5533 | 110.2933 |
| path_cache_simhash | 0.9987 | 0.2009 | 16.6333 | 1.0000 | 96.0000 |
| path_cache_pq-prefix | 0.9987 | 0.1797 | 16.6300 | 1.0000 | 96.0000 |
| path_cache_simhash-pq | 0.9987 | 0.2202 | 16.6333 | 1.0000 | 96.0000 |

## 结果分析

V7 证明，在 synthetic agent 连续查询 workload 下，query signature 对路径复用有直接影响。`routed` signature 能命中一部分相似路径，但 SimHash 和 PQ prefix 在这个 workload 中更稳定，path cache hit rate 达到 1.0000，同时保持 Recall@10 不变。

`pq-prefix` 在本次本地 warm/smoke 中 P99 最低，为 0.1797 ms；但这个结论只适用于当前 synthetic workload。正式论文式结论仍需要在 SIFT100K/SIFT1M 和 strict cold/warm 条件下复测。

## 风险与局限

- 本次是 Windows 本地 warm/smoke，不是 strict cold I/O 结论。
- 数据集为 synthetic agent workload，不能外推到所有真实 embedding 分布。
- 当前 path cache 复用的是 seeds 和 Top-K ids，不是完整候选 frontier。
- 当前 index 使用小规模 exact build，V8 才引入面向 SIFT100K/SIFT1M 的近似建图默认路径。

## 归档结果

- `archive/results/v7-signature-local-2026-05-29.txt`
- `archive/configs/v7-signature-local-2026-05-29.json`
- `archive/logs/v7-signature-local-2026-05-29.log`
- `archive/build_info/v7-signature-local-2026-05-29.txt`

## 下一步

进入 V8：为 SIFT100K/SIFT1M 替换掉 O(N^2) exact graph builder，并补齐支持删除的 FreshVamana + LSM-style StreamMerge。
