# AgentMem-Flow 本地合成数据验证报告

日期：2026-06-05  
Commit：`a554e29`  
验证目标：基于本地合成数据验证升级后的受限内存机制、读优化路径、PQ/ADC 压缩路径和混合读写路径。

## 1. 实验环境

| 参数 | 值 |
|---|---|
| CPU | Intel(R) Core(TM) i7-14700HX |
| RAM | 31.71 GB |
| OS | Microsoft Windows 11 专业版 10.0.26200 |
| 可执行文件 | `D:\agent-aware\build\agentmem_flow.exe` |
| 构建时间 | 2026-06-05 00:41:39 |
| 数据来源 | 本地 synthetic 随机/聚类合成数据 |
| 约束口径 | `memory_budget_ratio=0.20`，以原始向量字节数的 20% 作为 resident engine memory 上限 |

说明：本报告用于验证项目机制和本地 smoke 趋势，不替代 Linux NVMe / `io_uring` / SIFT10K-1M 的最终参赛成绩。Windows 本地路径使用 `pread` 等价路径，`io_uring` 相关能力需要在 Linux 环境继续验证。

## 2. 验证结论

| 大版本/场景 | 配置摘要 | Recall@10 | QPS | P99 延迟 | Resident/Raw | 20% 内存合规 | 结论 |
|---|---:|---:|---:|---:|---:|---:|---|
| M0 Exact 正确性基线 | exact, 2000x64 | 1.0000 | 12851.82 | 0.1003 ms | 100.60% | 否 | 仅用于 ground truth / correctness，对受限内存不达标 |
| M1 SSD 图索引基线 | graph, packed BFS, width=768 | 1.0000 | 370.10 | 3.8750 ms | 13.68% | 是 | 已证明图索引主路径可在 20% 内存约束下达标 |
| M2 PQ/ADC 压缩读优化 | graph + PQ(M=4,K=16) + ADC + rerank64 | 1.0000 | 654.68 | 2.5749 ms | 12.35% | 是 | 在合规内存下比 M1 更快，读放大更低 |
| M2 反例：默认大码本 | graph + PQ(M=8,K=256) + ADC | 1.0000 | 556.08 | 2.8439 ms | 114.67% | 否 | 默认 K=256 对小数据集码本过大，证明内存守卫能识别超限 |
| M3 混合读写路径 | graph, 1000x32, 20% write, 25% delete | 0.6319 | 1625.93 | 1.0933 ms | 15.68% | 是 | 内存合规，但动态主图召回不足，需要下一阶段调优 |

核心判断：

- 20% resident memory 机制已生效：合规配置在 `--enforce-memory-budget` 下正常运行，超限配置会被判定为 `memory_budget_pass=0`，可用 `--allow-over-budget-for-debug` 显式标记为调试反例。
- M1/M2 读路径已经具备受限内存证明：原始向量不常驻内存，图元数据、router、PQ codes/codebooks 等组件进入统一内存统计。
- PQ/ADC 小码本配置是当前本地 synthetic 的推荐读优化路径：相比 M1，QPS 从 370.10 提升到 654.68，P99 从 3.8750 ms 降到 2.5749 ms，SSD reads/query 从 166.9450 降到 99.6050。
- M3 动态读写路径的内存控制已经过关，但 Recall@10=0.6319 低于计划中的 `>=0.85` 最低线，后续应优先处理动态主图召回、删除修补和 delta-main 合并策略。

## 3. 受限内存机制验证

新增运行参数：

| 参数 | 作用 |
|---|---|
| `--memory-budget-ratio 0.20` | 按原始向量大小的 20% 计算预算 |
| `--memory-budget-bytes N` | 显式指定预算上限 |
| `--enforce-memory-budget` | resident memory 超预算时 fail fast |
| `--allow-over-budget-for-debug` | 允许超预算实验继续运行，但结果标记为不合规 |

新增输出字段：

| 字段 | 含义 |
|---|---|
| `memory_budget_bytes` | 当前实验内存预算 |
| `memory_resident_bytes` | 搜索期常驻引擎内存估算 |
| `memory_resident_ratio` | 常驻内存 / 原始向量大小 |
| `memory_budget_pass` | 是否满足预算 |
| `memory_accounting_scope` | 统计口径，本次为 `engine_resident` |
| `memory_bytes_*` | raw vectors、PQ、graph metadata、cache、router、delta、tombstone 等组件拆分 |

关键反例：

- Exact baseline 的 `memory_resident_ratio=1.0060`，因为原始向量全量常驻内存，不能作为受限内存达标证据。
- 默认 `PQ M=8,K=256` 在 2000x64 小数据集下 `memory_bytes_pq_codebooks=524360`，单独码本已经超过 20% 预算，最终 `memory_resident_ratio=1.1467`。
- 将 PQ 调整为 `M=4,K=16` 后，`memory_bytes_pq_codes=8000`，`memory_bytes_pq_codebooks=16424`，总 resident ratio 降到 12.35%，达成预算。

## 4. 分场景结果

### 4.1 M0 Exact 正确性基线

命令摘要：

```powershell
.\build\agentmem_flow.exe --engine exact --synthetic --base-count 2000 --query-count 200 --dim 64 --clusters 32 --k 10
```

结果：

| 指标 | 值 |
|---|---:|
| Recall@10 | 1.0000 |
| QPS | 12851.8185 |
| Avg 延迟 | 0.0777 ms |
| P99 延迟 | 0.1003 ms |
| memory_budget_bytes | 102400 |
| memory_resident_bytes | 515064 |
| memory_resident_ratio | 1.0060 |
| memory_budget_pass | 0 |

结论：Exact 路径用于 correctness upper bound。由于 raw vectors 全量常驻，违反 20% 内存约束，不能作为达标实现。

### 4.2 M1 SSD 图索引受限内存基线

命令摘要：

```powershell
.\build\agentmem_flow.exe --engine graph --synthetic --base-count 2000 --query-count 200 --dim 64 --clusters 32 --k 10 --index build\m1_synth_graph.idx --build-index --layout packed --packing bfs --graph-degree 32 --search-width 768 --entry-count 192 --routing-sample-count 192 --cache-policy none --path-cache-policy none --memory-budget-ratio 0.20 --enforce-memory-budget
```

结果：

| 指标 | 值 |
|---|---:|
| Recall@10 | 1.0000 |
| QPS | 370.1005 |
| Avg 延迟 | 2.7013 ms |
| P99 延迟 | 3.8750 ms |
| memory_budget_bytes | 102400 |
| memory_resident_bytes | 70024 |
| memory_resident_ratio | 0.1368 |
| memory_budget_pass | 1 |
| SSD reads/query | 166.9450 |
| graph_expanded/query | 768.0000 |

结论：图索引基线已满足受限内存约束。该配置更偏向召回验证，`search_width=768` 带来较高随机读，需要 M2 继续降低读放大。

### 4.3 M2 PQ/ADC 压缩读优化

命令摘要：

```powershell
.\build\agentmem_flow.exe --engine graph --synthetic --base-count 2000 --query-count 200 --dim 64 --clusters 32 --k 10 --index build\m1_synth_pq_budget.idx --build-index --layout packed --packing bfs --graph-degree 32 --search-width 256 --search-early-stop --search-early-stop-min 128 --entry-count 96 --routing-sample-count 96 --pq-enable --pq-m 4 --pq-ks 16 --adc-enable --rerank-topk 64 --cache-policy none --path-cache-policy none --memory-budget-ratio 0.20 --enforce-memory-budget
```

结果：

| 指标 | 值 |
|---|---:|
| Recall@10 | 1.0000 |
| QPS | 654.6827 |
| Avg 延迟 | 1.5270 ms |
| P99 延迟 | 2.5749 ms |
| memory_budget_bytes | 102400 |
| memory_resident_bytes | 63216 |
| memory_resident_ratio | 0.1235 |
| memory_budget_pass | 1 |
| memory_bytes_pq_codes | 8000 |
| memory_bytes_pq_codebooks | 16424 |
| SSD reads/query | 99.6050 |
| graph_expanded/query | 128.0000 |
| ADC table build | 1.9545 us/query |

结论：这是当前本地 synthetic 下的推荐读优化配置。它同时满足 Recall、20% 内存和较低 I/O 放大的要求，适合作为后续 SIFT10K/SIFT100K 的主线参数起点。

### 4.4 M2 反例：默认 PQ 大码本超预算

命令摘要：默认 `PQ M=8,K=256`，并使用 `--allow-over-budget-for-debug` 保留调试输出。

结果：

| 指标 | 值 |
|---|---:|
| Recall@10 | 1.0000 |
| QPS | 556.0795 |
| P99 延迟 | 2.8439 ms |
| memory_budget_bytes | 102400 |
| memory_resident_bytes | 587088 |
| memory_resident_ratio | 1.1467 |
| memory_budget_pass | 0 |
| memory_over_budget_allowed | 1 |
| memory_bytes_pq_codes | 16000 |
| memory_bytes_pq_codebooks | 524360 |

结论：默认大码本在小规模数据上明显不合规。该结果验证了内存统计的必要性，也说明后续参数选择必须随数据规模和预算自适应。

### 4.5 M3 混合读写路径

命令摘要：

```powershell
.\build\agentmem_flow.exe --engine graph --synthetic --base-count 1000 --query-count 100 --dim 32 --clusters 16 --k 10 --index build\m1_synth_mixed.idx --build-index --layout packed --packing bfs --graph-degree 16 --search-width 128 --entry-count 64 --routing-sample-count 64 --workload-mode mixed --operation-count 200 --write-ratio 20 --delete-ratio 25 --wal build\m1_synth_mixed.wal --compaction-policy sla --delta-compaction-threshold 32 --compaction-batch-size 16 --sla-p99-ms 5 --memory-budget-ratio 0.20 --enforce-memory-budget
```

结果：

| 指标 | 值 |
|---|---:|
| Recall@10 | 0.6319 |
| QPS | 1625.9336 |
| Ops/s | 2032.4171 |
| Avg 延迟 | 0.5985 ms |
| P99 延迟 | 1.0933 ms |
| memory_budget_bytes | 25600 |
| memory_resident_bytes | 20071 |
| memory_resident_ratio | 0.1568 |
| memory_budget_pass | 1 |
| insert_count | 30 |
| delete_count | 10 |
| tombstone_count | 10 |
| delta_recall_at_10 | 1.0000 |
| WAL records | 40 |
| WAL bytes | 4496 |

结论：M3 已经能在写入、删除、WAL、delta、tombstone 都计入预算的情况下保持 20% 内存合规。但主图搜索召回不足，说明动态场景不能只看延迟和内存，需要继续提升主图候选质量、删除修补和合并后连通性。

## 5. 原始日志

| 文件 | 内容 |
|---|---|
| `archive/results/m1-synthetic-exact-20260605.txt` | Exact correctness baseline |
| `archive/results/m1-synthetic-graph-budget-20260605.txt` | 20% 内存合规图索引基线 |
| `archive/results/m1-synthetic-pq-adc-rerank-budget-20260605.txt` | 20% 内存合规 PQ/ADC rerank |
| `archive/results/m1-synthetic-pq-adc-rerank-20260605.txt` | 默认 PQ 参数超预算的 enforced 运行输出 |
| `archive/results/m1-synthetic-pq-overbudget-debug-20260605.txt` | 默认 PQ 参数超预算但允许 debug 的完整输出 |
| `archive/results/m1-synthetic-mixed-budget-20260605.txt` | 20% 内存合规混合读写输出 |

## 6. 下一步建议

| 优先级 | 事项 | 目标 |
|---|---|---|
| P0 | 将 M2 小码本 PQ/ADC 参数迁移到 SIFT10K/SIFT100K 验证脚本 | 形成标准数据集上的 20% 内存合规证据 |
| P0 | 修复/调优 M3 动态召回 | 将 mixed Recall@10 从 0.6319 提升到 `>=0.85` |
| P1 | 引入预算自适应参数选择 | 根据 `raw_vector_bytes` 自动调整 PQ `M/K`、router sample、cache/path cache 容量 |
| P1 | Linux NVMe 复测 | 验证 `O_DIRECT` / `io_uring` 下的真实 I/O 延迟和吞吐 |
| P2 | 将 `memory_budget_pass=0` 从汇总推荐中排除 | 避免超预算配置被误判为最佳版本 |

## 7. 总体结论

本次本地合成数据验证表明，项目已经从“口头受限内存目标”升级为“可执行、可统计、可 fail fast 的受限内存机制”。M1/M2 读路径可以在 20% resident memory 约束内达到 Recall@10=1.0，其中 M2 PQ/ADC 小码本配置是当前最佳合规读优化版本。M3 混合读写路径已经验证了内存预算、WAL、delta 与 tombstone 统计链路，但召回率尚未达标，应作为下一轮升级重点。
