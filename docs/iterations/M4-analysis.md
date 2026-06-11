# AgentMem-Flow M4 分析报告

## 版本信息

- 大版本：M4 - 大规模构图与最终评测层
- 合并旧版本：V8 的构图部分、V9、最终评测归档
- 对应计划：SIFT100K/SIFT1M 构图、FreshLSH-Vamana、early-stop、冷/热启动评测
- 状态：已完成 SIFT100K/SIFT1M 路径验证，仍需最终参数归档

## 合并范围

M4 合并了原始报告中的大规模构图和最终搜索评测：

- V8：`exact|approx-rp` 构图、robust prune、SIFT 辅助脚本。
- V9：FreshLSH-Vamana，`lsh-rp` 构图、bulk reverse edge、batched delete patch、early-stop。
- Final：面向 SIFT1M 的冷启动、warm run、direct I/O、Recall/latency/QPS 汇总。

这一层用于回答项目最终问题：在大规模向量图外存化之后，系统能否在受限内存和真实 I/O 下达到可接受 Recall 与延迟。

## 核心能力

### 大规模构图

V8 将构图从 exact 扩展到 approximate random projection 策略，并加入 robust prune。该阶段解决了小规模 smoke 可以运行但大规模构图成本过高的问题。

V9 进一步引入 FreshLSH-Vamana 路径，用 LSH random projection 找候选，再通过剪枝控制出边规模。bulk reverse edge 和 batch patch 降低了构图与删除修复的额外成本。

### Early-Stop 搜索

V9 增加 early-stop，用 expanded node 数或候选稳定性限制搜索尾部开销。该策略可以显著降低 P99 和磁盘读取，但会带来 Recall 损失，需要通过参数扫描找到可接受折中点。

### SIFT 评测链路

M4 汇总 SIFT10K、SIFT100K、SIFT1M 的实验路径，保留从小规模 smoke 到大规模运行的递进证据。最终评测应同时报告：

- Recall@10
- QPS
- Avg/P50/P95/P99 latency
- Reads/Query
- expanded nodes
- build time
- memory resident ratio
- I/O mode effective

## 指标摘要

| 场景 | 关键结果 | 结论 |
| --- | --- | --- |
| SIFT10K lsh-rp | Recall 约 1.0000，P99 约 7.6510 ms，Reads/Query 约 769.47 | 小规模 LSH 构图质量充足 |
| SIFT10K early128 | Recall 约 0.9990，P99 约 6.0019 ms，Reads/Query 约 467.16 | early-stop 能降低尾延迟且几乎不损失 recall |
| SIFT100K lsh-rp w1536 | Recall 约 0.9990，P99 约 60.6911 ms，Reads/Query 约 4436.47 | 高 recall 参数可用但 I/O 开销偏高 |
| SIFT100K w384 | Recall 约 0.9940，P99 约 34.3073 ms，Reads/Query 约 2613.41 | 降低候选宽度可改善延迟 |
| SIFT100K w384 early128 | Recall 约 0.9530，P99 约 21.9926 ms，Reads/Query 约 1625.72 | aggressive early-stop 延迟更好但 recall 下降明显 |
| SIFT100K w384 early192 | Recall 约 0.9760，P99 约 30.5251 ms，Reads/Query 约 1941.55 | early-stop 折中点更稳 |
| FreshLSH StreamMerge smoke | Recall 约 0.9848，P99 约 0.3479 ms，Reads/Query 约 18.5952 | 动态 merge 路径具备基本可行性 |

## 与 M2 的衔接

M4 的最终评测必须复用 M2 的真实 I/O 约束。只在 buffered I/O 或 warm OS Page Cache 下得到的结果，不能作为最终系统指标。

最终归档建议拆成三组：

- buffered warm：用于和历史报告可比。
- O_DIRECT cold：用于证明绕过 Page Cache 后的真实性能。
- io_uring + prefetch：用于展示异步 I/O 的收益。

## 计划问题映射

- P15：大规模构图成本，由 approx-rp 和 lsh-rp 构图缓解。
- P16：高 Recall 与低尾延迟折中，由 search width 和 early-stop 参数扫描处理。
- P17：最终评测归档，由 SIFT100K/SIFT1M 的统一指标表完成。

## 阶段结论

M4 是最终展示层。它需要把前面所有模块串起来：M0 的真值，M1 的缓存和路径复用，M2 的真实 direct/asynchronous I/O，M3 的动态写入和合并。

当前证据已经证明项目可以完成从构图、外存搜索、动态更新到 SIFT 路径运行的端到端链路。最终版本还应补齐 SIFT1M 的 cold/warm/direct/io_uring 参数矩阵，并把 Recall@10 大于等于 85% 与 20% 内存限制作为硬性验收线。

