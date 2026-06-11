# AgentMem-Flow M3 分析报告

## 版本信息

- 大版本：M3 - 动态写入与合并层
- 合并旧版本：V5、V5.1、V5.2、V8 的动态更新部分
- 对应计划：WAL、Delta Index、恢复、删除、FreshVamana patch、StreamMerge
- 状态：已完成基础动态更新链路

## 合并范围

M3 合并了原始报告中的写入、恢复和动态图维护能力：

- V5：Delta Index、WAL、SLA-aware compaction。
- V5.1：WAL replay 与恢复验证。
- V5.2：Delta IVF-Flat，降低增量区搜索成本。
- V8：FreshVamana 风格删除 patch、tombstone、LSM-style StreamMerge。

这一层面向 agent memory 的实时写入场景：新记忆不断插入，旧记忆可能删除或更新，查询不能因为后台整理而发生明显抖动。

## 核心能力

### WAL 与增量写入

V5 引入 write-ahead log，将实时插入先写入 WAL，再进入内存 delta index。查询时同时搜索主图和 delta，并在结果层合并候选。

这种结构将随机写转化为顺序追加写，避免每次插入都立即改写磁盘主图。

### WAL Replay

V5.1 增加 WAL parser/replay 能力，支持启动时从日志恢复 delta 状态。报告中 replay 记录数、replay 字节数和恢复后的 delta size 均可统计，证明写入链路具备崩溃恢复基础。

### Delta IVF-Flat

V5.2 将 delta 区从 flat search 扩展为 IVF-Flat。对于不断增长的增量区，IVF-Flat 可以降低每次查询扫描的 delta 向量数，在 recall 接近 flat delta 的情况下改善增量搜索延迟。

### 删除与 StreamMerge

V8 引入 tombstone、删除 patch 和 FreshVamana 风格图修复。被删除节点不再直接参与最终结果，必要时通过邻居重连维持局部连通性。

StreamMerge 使用 LSM 思想将实时写入逐步合并到更稳定的图结构中，降低高并发写入对查询延迟的影响。

## 指标摘要

| 阶段 | 关键实验 | 结果 | 结论 |
| --- | --- | --- | --- |
| V5 | Mixed read/write | 输出 insert latency、compaction metrics、动态 recall | 主图 + delta 的读写链路可运行 |
| V5.1 | WAL replay | replay 32 records，Recall 约 0.9975 | 日志恢复后查询正确性保持稳定 |
| V5.2 | Delta IVF-Flat | delta recall 约 0.9987，搜索延迟低于 flat delta | 增量区可继续索引化 |
| V8 | FreshVamana/StreamMerge smoke | 支持 tombstone、patch、merge | 动态图维护进入可验证阶段 |

## 计划问题映射

- P6：实时写入会干扰查询，已由 WAL + delta index 缓解。
- P7：后台 compaction/merge 缺失，已由 SLA-aware compaction 和 StreamMerge 补齐。
- P14：删除与图修复，由 tombstone 和 FreshVamana patch 支持。

## 阶段结论

M3 将项目从只读 ANN 引擎推进为动态 memory 存储引擎。WAL 负责可靠写入，delta index 负责新数据可见性，compaction/StreamMerge 负责把短期写入成本摊还到后台。

后续 M4 需要重点验证动态写入与大规模搜索混合时的尾延迟，尤其是后台 merge 是否会影响 direct I/O 读路径。

