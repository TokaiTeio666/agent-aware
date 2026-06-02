# AgentMem-Flow V8 分析报告

## 版本信息

- 版本：V8 - FreshVamana + LSM-style StreamMerge
- 日期：2026-05-29
- 状态：已完成本地 synthetic 删除 smoke、WAL replay 删除验证和 SIFT10K FreshVamana 回归；SIFT100K/SIFT1M 正式实验待 WSL2/AutoDL 继续运行

## 版本目标

V8 的目标是把项目从“只适合插入的 Delta + Compaction baseline”推进到可支持删除的图索引更新流程，并为 SIFT100K/SIFT1M 去掉 O(N^2) exact kNN 建图瓶颈。

实现上引入 FreshVamana 风格的 RobustPrune、删除 patch、近似随机投影建图，以及 LSM-style StreamMerge。新增写入仍进入 RW TempIndex / WAL 路径，删除先进入 tombstone/delete-list，查询时过滤 tombstone，最终由 StreamMerge 产出新的 LTI index。

## 对应计划书问题

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P10：exact kNN 建图无法支撑 SIFT100K/SIFT1M | 部分解决 | 新增 `--graph-build-policy approx-rp`，Linux/SIFT helper 默认使用近似随机投影候选构图 |
| P11：删除会破坏图连通性和召回 | 部分解决 | 删除时将入点/出点 patch 后通过 RobustPrune 控制度数，并在 StreamMerge 中批处理 |
| P6：动态写入频繁修改主图成本高 | 部分解决 | 插入保留 WAL + Delta/TempIndex 路径，最终由 StreamMerge 合并 |
| P8：标准数据集不能依赖暴力搜索 | 部分解决 | SIFT helper 支持官方目录；当 SIFT1M 官方 truth 与 base subset 不匹配时会自动检测并重算当前子集 exact truth |
| P9：归档不完整 | 部分解决 | 补齐 V8 result/config/log/build_info/report，并归档验证方法快照 |

## 实现范围

新增内容：

- Graph build policy：`--graph-build-policy exact|approx-rp`。
- 近似随机投影候选建图参数：`--approx-projections`、`--approx-window`、`--approx-random-samples`、`--approx-candidate-limit`。
- FreshVamana RobustPrune 参数：`--robust-prune-alpha`。
- 删除 patch：删除节点后连接其入点和出点，再对超度数节点运行 RobustPrune。
- WAL insert/delete record replay。
- `--delete-ratio` mixed workload。
- tombstone 查询过滤。
- `--compaction-policy stream-merge`。
- `--stream-merge-index` 输出新 LTI index。
- WSL/SIFT 本地脚本：`scripts/linux/run_sift_local.sh`。
- AutoDL SIFT 模板加入 approx-rp 和 Fresh LSM 参数。

未包含内容：

- 完整 FreshDiskANN 论文级磁盘 block-by-block lazy merge 实现。
- 多个持久化 RO TempIndex segment 的长期生命周期管理。
- StreamMerge 完成后在同一进程热切换到新 LTI；当前新 index 供下一次运行使用。
- SIFT100K/SIFT1M strict cold/warm 正式结论。

## 验证方式

构建命令：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

Synthetic FreshVamana + LSM smoke：

```powershell
.\build\agentmem_flow.exe --engine graph --synthetic --base-count 1000 --query-count 100 --dim 32 --clusters 16 --k 10 --layout packed --packing bfs --index build\fresh_lti_archive.idx --build-index --graph-build-policy approx-rp --graph-degree 16 --approx-projections 6 --approx-window 16 --approx-random-samples 16 --approx-candidate-limit 256 --search-width 128 --entry-count 48 --routing-sample-count 256 --cache-policy none --path-cache-policy none --workload-mode mixed --operation-count 300 --write-ratio 30 --delete-ratio 50 --wal build\fresh_lsm_archive.wal --compaction-policy stream-merge --stream-merge-index build\fresh_lti_archive_merged.idx --run-type smoke
```

SIFT10K FreshVamana 回归：

```powershell
.\build\agentmem_flow.exe --engine graph --base data\sift\sift_base.fvecs --query data\sift\sift_query.fvecs --truth data\sift\sift_groundtruth.ivecs --base-limit 10000 --query-limit 100 --k 10 --layout packed --packing bfs --index build\sift10k_freshvamana_20260529-155420.idx --build-index --graph-build-policy approx-rp --graph-degree 32 --approx-projections 10 --approx-window 32 --approx-random-samples 32 --approx-candidate-limit 768 --search-width 1024 --entry-count 256 --routing-sample-count 4096 --cache-policy none --path-cache-policy none --run-type smoke
```

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 成功生成可执行文件 |
| 近似建图可运行 | 是 | SIFT10K approx-rp + RobustPrune 建图完成，`graph_build_seconds=8.9465` |
| 动态删除可观测 | 是 | synthetic smoke 中 `delete_count=45`，`tombstone_count=45` |
| WAL insert/delete 可观测 | 是 | `wal_records=90`，WAL replay 删除验证得到 `wal_replay_inserts=45`、`wal_replay_deletes=45` |
| StreamMerge 可产出新 LTI | 是 | `stream_merge_ops=1`，`stream_merge_vectors=1045`，`stream_merge_seconds=0.0489` |
| Dynamic Recall@10 >= 0.95 | 是 | synthetic mixed delete smoke 为 0.9862 |
| SIFT10K 回归 Recall@10 >= 0.95 | 是 | 当前 base subset exact truth 下为 1.0000 |
| 结果、配置、日志、环境信息已归档 | 是 | 见归档结果 |

## 指标结果

Synthetic FreshVamana + LSM smoke：

| 指标 | 数值 |
|---|---:|
| Recall@10 | 0.9862 |
| 1-Recall@10 | 0.0138 |
| QPS | 2953.8150 |
| P99 Latency ms | 0.5381 |
| Insert Count | 45 |
| Delete Count | 45 |
| Tombstone Count | 45 |
| WAL Records | 90 |
| StreamMerge Ops | 1 |
| StreamMerge Vectors | 1045 |
| StreamMerge Inserted | 45 |
| StreamMerge Deleted | 45 |
| StreamMerge Seconds | 0.0489 |

SIFT10K FreshVamana 回归：

| 指标 | 数值 |
|---|---:|
| Graph Build Seconds | 8.9465 |
| Graph Build Policy | approx-rp |
| RobustPrune Alpha | 1.2 |
| Recall@10 | 1.0000 |
| P99 Latency ms | 8.3964 |
| SSD Reads / Query | 855.2200 |
| Search Width | 1024 |

## 结果分析

V8 已经把“删除只记 tombstone、最终批量修图”的闭环跑通。Synthetic mixed workload 中，45 个 insert 和 45 个 delete 都进入 WAL，查询阶段能过滤 tombstone，StreamMerge 最后将 1000 个原始 LTI 向量加 45 个插入并扣除 45 个删除，写出新的 LTI index。

SIFT10K 回归说明 approx-rp + RobustPrune 路径可以在官方 SIFT 文件上工作，并且当官方 SIFT1M ground truth 与 `base_limit=10000` 子集不匹配时，程序能检测 out-of-range id 并切换到当前子集 exact truth。这是开发期必要保护，但正式 SIFT1M 结果仍应使用完整 base 和官方 `.ivecs`。

## 风险与局限

- 当前实现是 FreshVamana + LSM-style StreamMerge 原型，不是完整 FreshDiskANN 生产系统。
- StreamMerge 写出新 LTI 后没有在同一进程热切换；下一次运行需要用 `INDEX=<merged.idx> BUILD_INDEX=0` 读取新索引。
- RobustPrune 当前提高了 SIFT10K 建图成本，后续需要调 `approx_*` 参数、degree 和 prune alpha。
- SIFT100K/SIFT1M 还需要在 WSL2 ext4 或 AutoDL SSD 上正式运行 cold/warm；Windows 本地结果只能作为功能验证。
- SIFT 子集使用官方 SIFT1M truth 会出现 out-of-range id；正式子集实验应准备子集 truth，或明确标注 exact brute force fallback。

## 归档结果

- `archive/results/v8-freshvamana-lsm-local-2026-05-29.txt`
- `archive/results/v8-freshvamana-lsm-smoke-2026-05-29.txt`
- `archive/results/v8-freshvamana-sift10k-2026-05-29.txt`
- `archive/configs/v8-freshvamana-lsm-local-2026-05-29.json`
- `archive/logs/v8-freshvamana-lsm-smoke-2026-05-29.log`
- `archive/logs/v8-freshvamana-sift10k-2026-05-29.log`
- `archive/build_info/v8-freshvamana-lsm-local-2026-05-29.txt`
- `archive/validation/validation-method-2026-05-29.md`

## 下一步

在 WSL2 Ubuntu ext4 或 AutoDL SSD 上运行 SIFT100K，再扩大到 SIFT1M。SIFT1M 正式结果应使用官方 `.ivecs`、warm/cold 分组、保留建图耗时、查询指标和 StreamMerge 后新 LTI 的二次读取结果。
