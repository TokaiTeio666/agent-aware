# AgentMem-Flow V5 分析报告

## 版本信息

- 版本：V5 - Delta Index + WAL + SLA-aware Compaction
- 日期：2026-05-28
- 状态：已完成最小可运行 baseline

## 版本目标

V5 的目标是把系统从只读向量检索推进到动态读写场景：新写入的 Agent memory 不直接修改 Main Index，而是先写入 WAL，再进入内存 Delta Index；查询时同时搜索 Main Index 和 Delta Index，并合并 Top-K。后台合并不再无条件抢占查询路径，而是通过 SLA-aware 调度把合并工作尽量移到写入路径或低压时段，降低混合读写场景下的 P99 抖动。

## 对应计划书问题

计划书位置：`docs/PROJECT_PLAN.md`。

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P6：动态写入频繁修改主图成本高 | 部分解决 | V5 新增 WAL + Delta flat index，新 memory 可实时写入且不重建 Main Index |
| P7：后台 compaction 与查询抢 I/O，放大 P99 | 部分解决 | V5 新增 aggressive 与 SLA-aware compaction 对比，SLA 策略避免在 query critical path 上执行 compaction |
| P0：验证方法不完整 | 部分解决 | V5 增加混合读写 workload、动态 ground truth、insert latency、compaction interference 指标和 pass criteria |
| P9：归档不完整 | 部分解决 | V5 已归档配置、日志、结果和 build info |
| P5：路径复用 | 保持有效 | V5 混合读写实验继续启用 V4 Query Path Cache，path cache hit rate 为 0.5556 |

## 验证定义

- Recall@10：对每个 query，系统 Top-10 返回结果与 ground truth Top-10 的交集比例，再对所有 query 取平均。
- 1-Recall@10：`1 - Recall@10`。
- Ground Truth 来源：V5 使用动态 exact brute force。每个查询时刻的 ground truth 基于 `Main base + 当前 Delta active + 当前 Delta sealed` 计算。
- Run 类型：warm run，执行 1 轮 warmup 后记录正式指标。
- Workload：Agent-style synthetic mixed workload，20% insert，80% query。
- 全局验证规范：`docs/VALIDATION_METHOD.md`。

## 实现范围

新增内容：

- `WalWriter`：追加写入 insert record，记录 WAL records 和 WAL bytes。
- `DeltaFlatIndex`：内存 active delta + sealed delta，支持插入、精确搜索和 active-to-sealed compaction。
- Top-K merge：Main Index ANN 结果与 Delta exact 结果合并。
- mixed workload：`--workload-mode mixed`、`--operation-count`、`--write-ratio`。
- compaction policy：`none`、`aggressive`、`sla`。
- SLA-aware compaction：根据近期查询 P99 判断是否在写入路径执行合并；查询路径上只记录跳过次数。
- 新指标：insert latency、WAL records、delta active/sealed size、compaction ops、query compaction interference。
- `scripts/run_v5_mixed_compare.ps1`。

未包含内容：

- Delta HNSW。
- WAL recovery replay。
- 真正把 delta 合并回 SSD Main Graph。
- 真正的后台线程和 io_uring I/O 限速。
- 真实 Agent trace。

## 验证方式

构建命令：

`powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

对比脚本：

`powershell -ExecutionPolicy Bypass -File .\scripts\run_v5_mixed_compare.ps1`

主实验 workload：

- synthetic dataset
- synthetic_workload = agent
- session_length = 10
- base_count = 2000
- query_count = 300
- operation_count = 360
- write_ratio = 20%
- measured_queries = 288
- insert_count = 72
- dim = 64
- clusters = 32
- seed = 42
- layout = packed
- packing = coaccess
- cache_policy = agent
- path_cache_policy = reuse
- delta_compaction_threshold = 24
- compaction_batch_size = 8
- compaction_work_us = 700

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 可生成 `build/agentmem_flow.exe` |
| Recall@10 >= 0.95 | 是 | 三组实验均为 0.9986 |
| WAL records 等于 insert_count | 是 | WAL records = 72，insert_count = 72 |
| Delta 与 Main Top-K 可合并 | 是 | Ground truth 为动态 exact，Recall@10 保持 0.9986 |
| insert latency 可观测 | 是 | 输出 `insert_latency_ms_*` |
| aggressive compaction 会产生查询路径干扰 | 是 | `query_compaction_ms_p99 = 0.7009 ms` |
| SLA compaction 查询路径干扰低于 aggressive | 是 | SLA 的 `compaction_interference_ms_per_query = 0.0000` |
| SLA compaction P99 低于 aggressive | 是 | P99 从 1.0024 ms 降到 0.4430 ms |
| 结果、配置、日志、环境信息已归档 | 是 | 已补充 `archive/configs`、`archive/logs`、`archive/results`、`archive/build_info` |

## 指标结果

Warm mixed run 对比：

| 变体 | Recall@10 | Query P99 ms | Avg ms | Insert Avg ms | WAL Records | Delta Active | Delta Sealed | Compaction Ops | Query Compaction P99 ms | Interference ms/query |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| no compaction | 0.9986 | 0.4139 | 0.3005 | 0.0106 | 72 | 72 | 0 | 0 | 0.0000 | 0.0000 |
| aggressive | 0.9986 | 1.0024 | 0.3078 | 0.0074 | 72 | 24 | 48 | 6 | 0.7009 | 0.0148 |
| SLA-aware | 0.9986 | 0.4430 | 0.2984 | 0.0776 | 72 | 16 | 56 | 7 | 0.0000 | 0.0000 |

相对 aggressive compaction：

```text
SLA P99 latency reduction = 55.81%
SLA query-path compaction interference reduction = 100.00%
Recall@10 change = 0.0000
WAL records / insert_count = 72 / 72
```

## 结果分析

V5 达到了最小动态写入闭环目标。查询结果不再只来自 Main Index，而是由 Main Graph ANN 结果和 Delta exact 结果合并得到。由于 ground truth 在每次查询时动态计算，插入向量会真实进入 Recall@10 评价，而不是只统计写入吞吐。

aggressive compaction 会在查询路径上执行 active-to-sealed 合并，虽然可以减少 active delta backlog，但会把 query P99 从 no-compaction 的 0.4139 ms 放大到 1.0024 ms。SLA-aware compaction 将合并工作放在写入侧，并在近期查询 P99 超过预算时跳过调度，因此 query-path compaction interference 为 0，P99 降到 0.4430 ms。

SLA 版本的 insert latency 上升是符合预期的：它把原本可能干扰查询的合并工作转移到了写入路径。对 Agent memory 场景而言，查询通常是在线交互关键路径，写入可以接受更高但可控的延迟，因此这是合理的调度取舍。

## 风险与局限

- 当前 Delta Index 是 flat exact index，适合最小 baseline 和中小规模 delta；大规模 delta 需要 Delta HNSW 或分层 delta segment。
- WAL 目前只写入，不做 recovery replay；后续可以增加启动恢复验证。
- compaction 目前是 active-to-sealed 内存合并，没有真正重写 Main Graph SSD 文件。
- compaction I/O 干扰使用可控时间片模拟，正式 Linux/WSL 实验需要替换为真实后台读写或 O_DIRECT I/O。
- 当前调度是单线程串行模拟，还不是完整后台线程调度器。

## 归档结果

- `archive/results/v5-mixed-2026-05-28.txt`
- `archive/configs/v5-mixed-2026-05-28.json`
- `archive/logs/v5-mixed-2026-05-28.log`
- `archive/build_info/v5-mixed-2026-05-28.txt`
- `archive/validation/validation-method-2026-05-28.md`

## 下一步

V5.1 可以补 WAL replay，使程序重启后能从 WAL 恢复 Delta active segment。V5.2 可以把 Delta flat index 升级为小型 HNSW 或 IVF-flat。最终版可以在 Linux/WSL 下用真实后台文件写入替代 compaction 时间片模拟，并补 SIFT100K/SIFT1M 官方 ground truth 实验。
