# AgentMem-Flow V9 分析报告

## 版本信息

- 版本：V9 - FreshLSH-Vamana SIFT1M 构图与低延迟搜索
- 日期：2026-05-29
- 状态：已完成本地构建、SIFT10K/SIFT100K smoke 回归和 FreshVamana + StreamMerge 动态删除 smoke；SIFT1M full run 待 WSL2/AutoDL

## 版本目标

V9 的目标不是照搬 FreshDiskANN，而是在保留 FreshVamana 删除修补语义的前提下，加入更适合 SIFT1M 现场构图的候选生成和低延迟搜索策略。

核心创新点是 FreshLSH-Vamana：用多表 SimHash LSH 生成小而稳定的候选集，构图阶段使用快速 top-k prune，反向边和删除修补都改为批处理。查询阶段新增 early-stop，将固定 `search_width` 改为“预算上限 + 最小探索 + frontier 自适应停止”。

## 对应计划书问题

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P10：exact kNN 图构建无法支撑 SIFT100K/SIFT1M | 部分解决 | `lsh-rp` 在本地 SIFT100K 上 5.77s 完成建图，避免 O(N^2) |
| P11：删除会破坏图索引连通性和召回 | 部分解决 | 保留 FreshVamana 删除 patch，并将 incoming edge 收集批量化 |
| P12：低延迟不能只靠扩大 search width | 部分解决 | early-stop 档在 Recall@10 >= 0.95 时降低 P99 和 reads/query |
| P9：归档不完整 | 部分解决 | 补齐 V9 result/config/log/build_info/report |

## 实现范围

新增内容：

- `--graph-build-policy lsh-rp`。
- `--lsh-tables`、`--lsh-bits`、`--lsh-probe-radius`、`--lsh-bucket-limit`。
- FreshLSH-Vamana 构图：multi-table SimHash LSH candidate generation + fast top-k prune。
- 批量反向边收集和一次性裁剪。
- 批量 FreshVamana delete patch，避免每个 delete 全图扫描。
- `--search-early-stop`。
- `--search-early-stop-min`。
- WSL/SIFT 和 AutoDL SIFT helper 默认使用 `lsh-rp`。

未包含内容：

- PQ 压缩和 ADC 距离表。
- O_DIRECT / io_uring。
- SIFT1M full base 正式 cold/warm 结果。
- 与外部 DiskANN/FreshDiskANN 二进制的同机对比。

## 验证方式

构建命令：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

SIFT10K 构建回归：

```powershell
.\build\agentmem_flow.exe --engine graph --base data\sift\sift_base.fvecs --query data\sift\sift_query.fvecs --truth data\sift\sift_groundtruth.ivecs --base-limit 10000 --query-limit 100 --k 10 --layout packed --packing bfs --index build\v9_lsh_sift10k.idx --build-index --graph-build-policy lsh-rp --graph-degree 32 --lsh-tables 8 --lsh-bits 10 --lsh-bucket-limit 64 --approx-window 32 --approx-random-samples 32 --approx-candidate-limit 192 --search-width 1024 --entry-count 256 --routing-sample-count 4096 --cache-policy none --path-cache-policy none --run-type smoke
```

SIFT100K 构建回归：

```powershell
.\build\agentmem_flow.exe --engine graph --base data\sift\sift_base.fvecs --query data\sift\sift_query.fvecs --truth data\sift\sift_groundtruth.ivecs --base-limit 100000 --query-limit 100 --k 10 --layout packed --packing bfs --index build\v9_lsh_sift100k.idx --build-index --graph-build-policy lsh-rp --graph-degree 32 --lsh-tables 8 --lsh-bits 14 --lsh-bucket-limit 64 --approx-window 32 --approx-random-samples 32 --approx-candidate-limit 192 --search-width 1536 --entry-count 512 --routing-sample-count 8192 --cache-policy none --path-cache-policy none --run-type smoke
```

SIFT100K low-latency early-stop 档：

```powershell
.\build\agentmem_flow.exe --engine graph --base data\sift\sift_base.fvecs --query data\sift\sift_query.fvecs --truth data\sift\sift_groundtruth.ivecs --base-limit 100000 --query-limit 100 --k 10 --layout packed --packing bfs --index build\v9_lsh_sift100k.idx --graph-build-policy lsh-rp --graph-degree 32 --lsh-tables 8 --lsh-bits 14 --lsh-bucket-limit 64 --approx-window 32 --approx-random-samples 32 --approx-candidate-limit 192 --search-width 384 --search-early-stop --search-early-stop-min 192 --entry-count 512 --routing-sample-count 8192 --cache-policy none --path-cache-policy none --run-type smoke
```

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 成功 |
| SIFT10K 构建显著快于 V8 | 是 | V8 SIFT10K `approx-rp` 为 8.9465s，V9 `lsh-rp` 为 0.3640s |
| SIFT10K Recall@10 >= 0.95 | 是 | low-latency 档 Recall@10 = 0.9990 |
| SIFT100K 可现场建图 | 是 | `graph_build_seconds=5.7700` |
| SIFT100K Recall@10 >= 0.95 | 是 | low-latency `min=192` 档 Recall@10 = 0.9760 |
| early-stop 降低 P99 和 reads/query | 是 | SIFT100K `w384` P99 34.31ms 降到 30.53ms，reads/query 2613 降到 1942 |
| FreshVamana + StreamMerge smoke 仍通过 | 是 | dynamic Recall@10 = 0.9848，StreamMerge ops = 1 |

## 指标结果

| 实验 | Build s | Recall@10 | P99 ms | SSD Reads / Query | Expanded / Query |
|---|---:|---:|---:|---:|---:|
| SIFT10K lsh-rp | 0.3640 | 1.0000 | 7.6510 | 769.4700 | 1024.0000 |
| SIFT10K lsh-rp early128 | N/A | 0.9990 | 6.0019 | 467.1600 | 128.0000 |
| SIFT100K lsh-rp w1536 | 5.7700 | 0.9990 | 60.6911 | 4436.4700 | 1536.0000 |
| SIFT100K lsh-rp w384 | N/A | 0.9940 | 34.3073 | 2613.4100 | 384.0000 |
| SIFT100K lsh-rp w384 early128 | N/A | 0.9530 | 21.9926 | 1625.7200 | 128.0000 |
| SIFT100K lsh-rp w384 early192 | N/A | 0.9760 | 30.5251 | 1941.5500 | 192.0000 |
| FreshLSH StreamMerge smoke | 0.0208 | 0.9848 | 0.3479 | 18.5952 | 96.0000 |

## 结果分析

V9 解决了 V8 的主要构图瓶颈。V8 的 SIFT10K `approx-rp + RobustPrune` 建图为 8.9465s；V9 的 `lsh-rp` 为 0.3640s，约 24.6x 加速。SIFT100K 在本地 Windows smoke 中可以 5.77s 建图，说明该策略已经具备向 SIFT1M 扩展的工程基础。

early-stop 不能无下限使用。`min=0` 会把 SIFT100K P99 降到 7.95ms，但 Recall@10 只有 0.5790，因此不能作为默认。`min=192` 是当前稳健默认，Recall@10 = 0.9760，P99 相比无 early-stop 的 `w384` 从 34.31ms 降到 30.53ms；如果比赛指标只要求 Recall@10 >= 0.85，则 `min=128` 可以作为低延迟档，Recall@10 = 0.9530，P99 = 21.99ms。

## 风险与局限

- 本地 SIFT100K 使用的是官方 SIFT1M truth 文件加子集 fallback，因此报告中标注为 exact brute force fallback，不是正式 SIFT1M `.ivecs` 结论。
- SIFT1M full run 尚未执行，需要在 WSL2 ext4 或 AutoDL SSD 上跑完整 base。
- 当前尚未实现 PQ/ADC、O_DIRECT、io_uring，因此和图中参考实现的系统层 I/O 优化不是同一维度。
- `lsh_bits` 与数据规模有关：SIFT10K 用 10-12，SIFT100K/SIFT1M 默认 14 起步。

## 归档结果

- `archive/results/v9-freshlsh-summary-2026-05-29.txt`
- `archive/results/v9-lsh-sift10k-2026-05-29.txt`
- `archive/results/v9-lsh-sift10k-early128-2026-05-29.txt`
- `archive/results/v9-lsh-sift100k-2026-05-29.txt`
- `archive/results/v9-lsh-sift100k-w384-early192-2026-05-29.txt`
- `archive/results/v9-freshlsh-lsm-smoke-2026-05-29.txt`
- `archive/configs/v9-freshlsh-sift-local-2026-05-29.json`
- `archive/logs/v9-lsh-sift10k-2026-05-29.log`
- `archive/logs/v9-lsh-sift100k-2026-05-29.log`
- `archive/logs/v9-freshlsh-lsm-smoke-2026-05-29.log`
- `archive/build_info/v9-freshlsh-local-2026-05-29.txt`

## 下一步

在 WSL2 Ubuntu ext4 或 AutoDL SSD 上执行 SIFT1M full run：先 `BUILD_INDEX=1 GRAPH_BUILD_POLICY=lsh-rp BASE_LIMIT=0 QUERY_LIMIT=1000` 构建 LTI，再使用 `BUILD_INDEX=0` 分别跑 high-recall 档和 low-latency 档。正式对外结论必须使用 full base + 官方 `.ivecs`，并保留 cold/warm 两组。
