# AgentMem-Flow 验证方法

本文档定义 AgentMem-Flow 各版本必须遵守的验证方法。后续每个版本的分析报告、结果归档和答辩材料都应以本文档为准。

## 1. Recall@10 与 1-Recall@10 定义

对查询集合 `Q`，每个 query `q` 有：

- `GT_10(q)`：ground truth Top-10 向量 id 集合。
- `R_10(q)`：系统返回的 Top-10 向量 id 集合。

单个 query 的召回率：

```text
recall@10(q) = |R_10(q) ∩ GT_10(q)| / 10
```

整个查询集的召回率：

```text
Recall@10 = average(recall@10(q) for q in Q)
```

召回缺失率：

```text
1-Recall@10 = 1 - Recall@10
```

例如 `Recall@10 = 0.95` 表示平均每个 query 的 Top-10 ground truth 中有 9.5 个被召回；`1-Recall@10 = 0.05` 表示平均缺失比例为 5%。

## 2. Ground Truth 选择原则

标准数据集优先使用官方 ground truth：

- SIFT1M 必须优先使用官方 `.ivecs`，不依赖 V0 暴力搜索。
- SIFT100K / SIFT10K 如果由 SIFT1M 子集构造，优先使用对应子集的官方或预处理 ground truth。
- 如果直接用官方 SIFT1M `.ivecs` 验证 base subset，程序必须检查 truth id 是否越过当前 `base_count`；一旦越界，报告必须标注并切换为当前子集的 exact brute force truth，或使用预先生成的子集 truth。
- 只有 synthetic workload 或没有官方 ground truth 的小规模实验，才允许使用 V0 exact brute force 生成 ground truth。
- 动态读写实验必须使用“按查询时刻”的 dynamic ground truth：对每次 query，ground truth 应基于当时已经可见的 `Main Index + Delta Index - delete-list/tombstones` 逻辑全集计算，不能用运行结束后的全集倒推，不能忽略新插入向量，也不能把已删除向量算入 ground truth。

报告中必须明确记录：

- ground truth 文件路径或生成方式；
- 是否使用官方 `.ivecs`；
- 若使用 V0 暴力搜索，必须说明数据规模和原因。

## 3. Cold Run 与 Warm Run

AgentMem-Flow 是 I/O 优化项目，因此正式性能实验必须区分 cold run 和 warm run。

Cold run：

- 目标：体现磁盘 I/O 压力和冷启动行为。
- 要求：清空系统 Page Cache 或尽可能降低系统缓存影响；同时清空应用内缓存。
- Linux 推荐方式：在有权限时使用 `sync` 后写入 `/proc/sys/vm/drop_caches`。
- WSL/Linux 严格 cold run 推荐使用 `scripts/linux/run_v2_strict_cold_warm.sh`，并尽量将仓库和 index 文件放在 WSL ext4 文件系统中，而不是 `/mnt/c`。
- Windows 开发环境：如果不能可靠清空系统缓存，必须在报告中标注“未能严格清 OS cache”，并将结果视为 smoke/warm-like，不作为最终 cold 结论。

Warm run：

- 目标：体现缓存预热后的稳态性能。
- 要求：先执行至少一轮预热查询，再记录正式指标。
- V2/V3 之后必须报告 warm run，因为 packed layout 和 cache 优化都可能显著影响稳态性能。

每个正式实验至少记录：

```text
cold_qps
cold_p99_latency
warm_qps
warm_p99_latency
cache_hit_rate
ssd_reads_per_query
```

对于 V0/V1 的 smoke test，可以暂时标记为 `smoke run`；从 V2 开始，涉及 I/O 布局和缓存的版本必须区分 cold/warm。

## 4. 版本通过标准

每个版本必须设置 pass criteria，不能只描述“测了什么”。

建议标准如下。

| 版本 | 通过标准 |
|---|---|
| V0 | synthetic Recall@10 = 1.0；1-Recall@10 = 0；能输出完整指标；build/run/archive 流程正常 |
| V1 | graph Recall@10 达到设定阈值；visited/query 和 expanded/query 能正常统计；resident router 相比关闭 router 有明显 Recall 提升；SSD reads/query 可观测 |
| V2 | packed page layout 的 SSD reads/query 低于 one-node-per-page；co-access packing 优于 random packing；Recall@10 不明显下降 |
| V3 | cache hit rate 可观测；warm run P95/P99 latency 下降；SSD reads/query 下降 |
| V4 | path cache hit rate 可观测；相似连续查询的 latency 和 SSD reads/query 下降；Recall@10 不明显下降 |
| V5 | WAL records 等于成功 insert 数；Delta 与 Main Top-K 合并后 Recall@10 不明显下降；insert latency 可观测；读写混合下 SLA compaction 的 query P99 低于 aggressive compaction；compaction interference 可量化 |
| V5.1 | `--wal-replay` 后 `wal_replay_records == wal_replay_delta_size`；append 写入不覆盖已有 WAL；replay 后动态查询 Recall@10 不明显下降 |
| V5.2 | 保留 flat delta truth；`ivf-flat` 输出 delta search latency 和 delta recall；Delta ANN Recall@10 不明显低于 flat；delta search latency 相比 flat 可观测 |
| V6 | 真实 file compaction 的 `compaction_io_bytes` 可观测；AutoDL warm/cold 归档完整；SLA compaction 在真实文件 I/O 干扰下 P99 低于 aggressive compaction；SIFT 实验使用官方 `.ivecs` |
| V7 | Query signature policy 对比归档完整；Recall@10 不明显下降；path cache hit rate 可观测；SSD reads/query 和 expanded/query 相比 no path cache 下降 |
| V8 | `approx-rp` 建图可完成 SIFT10K 回归；mixed delete workload 中 `delete_count == tombstone_count`；`wal_records == insert_count + delete_count`；StreamMerge 指标可观测并写出新 LTI；dynamic Recall@10 >= 0.95 |
| V9 | `lsh-rp` 建图可完成 SIFT100K 现场构建；SIFT10K 构建时间显著低于 V8；early-stop 档在 Recall@10 >= 0.95 时降低 P99 和 SSD reads/query；FreshVamana + StreamMerge smoke 仍通过 |

如果某个标准未通过，报告必须明确写出原因和降级计划。

## 5. 归档要求

每个版本至少归档以下内容：

```text
docs/iterations/vX-analysis.md
archive/results/vX-*.txt
archive/configs/vX-*.json
archive/logs/vX-*.log
archive/build_info/vX-*.txt
```

归档内容必须包含：

- git commit hash；如果尚未产生 commit，记录为 `no commit yet` 并保存 `git status --short`。
- 编译时间。
- 编译器版本。
- CPU 信息。
- 内存信息。
- 磁盘 / 文件系统信息，如能采集则记录。
- 系统版本。
- 运行命令。
- 随机种子。
- 数据集路径。
- index 参数。
- run 类型：cold / warm / smoke。
- ground truth 来源。

V8 之后还必须记录：

- `graph_build_policy`、`approx_*` 参数和 `robust_prune_alpha`。
- `delete_count`、`tombstone_count`。
- `wal_replay_inserts`、`wal_replay_deletes`。
- `stream_merge_ops`、`stream_merge_vectors`、`stream_merge_inserted`、`stream_merge_deleted`、`stream_merge_seconds`。
- StreamMerge 输出 index 路径，以及该 index 是否已在后续进程中重新加载验证。

V9 之后还必须记录：

- `lsh_tables`、`lsh_bits`、`lsh_probe_radius`、`lsh_bucket_limit`。
- `search_early_stop`、`search_early_stop_min_expansions`。
- early-stop 前后的 Recall@10、P99、SSD reads/query 和 graph expanded/query 对照。

验证方法本身也要归档快照，避免后续规范变化导致旧版本不可解释：

```text
archive/validation/validation-method-YYYY-MM-DD.md
```
