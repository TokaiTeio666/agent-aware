# AgentMem-Flow V6 分析报告

## 版本信息

- 版本：V6 - AutoDL 部署版与真实文件 I/O Compaction
- 日期：2026-05-28
- 状态：已完成本地 smoke 验证与 AutoDL 部署脚本；服务器正式实验待上传到 AutoDL 后执行

## 版本目标

V6 的目标是把 V5 中的 compaction 时间片模拟升级为真实文件 I/O 干扰，并把最终实验环境收敛到 AutoDL Linux 服务器。这样答辩时可以明确说明：后台合并确实产生文件写入和 fsync，而不是只用 CPU busy wait 模拟延迟。

## 对应计划书问题

计划书位置：`docs/PROJECT_PLAN.md`。

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P7：后台 compaction 与查询抢 I/O，放大 P99 | 部分解决 | V6 新增 `--compaction-io-mode file`，在 Linux 下使用真实文件写入和 `fsync` 产生 I/O 干扰 |
| P6：动态写入频繁修改主图成本高 | 继承 V5 | 继续使用 WAL + Delta Index，查询时 Main + Delta Top-K merge |
| P0：验证方法不完整 | 部分解决 | V6 明确 AutoDL cold/warm run、环境采集、归档和 pass criteria |
| P8：标准数据集不能依赖暴力搜索 | 部分解决 | V6 增加 SIFT 脚本模板，要求传入官方 `.ivecs`；但大规模建图仍需预构建索引或近似建图器 |
| P9：归档不完整 | 部分解决 | V6 新增 AutoDL 环境采集、结果归档和证据打包脚本 |

## 实现范围

新增内容：

- `--compaction-io-mode time|file`。
- `--compaction-io-path`。
- `--compaction-io-bytes-per-vector`。
- `compaction_io_bytes` 输出指标。
- Linux 下 file compaction 使用 `open/write/fsync/close`。
- Windows 本地 smoke 使用 `ofstream + flush`。
- `scripts/autodl/collect_env.sh`。
- `scripts/autodl/drop_caches.sh`。
- `scripts/autodl/run_v6_synthetic.sh`。
- `scripts/autodl/run_v6_sift_template.sh`。
- `scripts/autodl/package_archive.sh`。
- `docs/AUTODL_DEPLOYMENT.md`。

未包含内容：

- 完整 io_uring/O_DIRECT 后台合并。
- 真正将 sealed delta 合并回 Main Graph SSD 索引。
- Delta HNSW。
- WAL replay。
- SIFT100K/SIFT1M 现场完整运行结果。

## 验证方式

本地 smoke：

`powershell -ExecutionPolicy Bypass -File .\scripts\run_v6_fileio_compare.ps1`

AutoDL synthetic warm run：

`DATE_TAG=v6-warm bash scripts/autodl/run_v6_synthetic.sh`

AutoDL strict cold run：

```bash
bash scripts/autodl/drop_caches.sh
DATE_TAG=v6-cold RUN_TYPE=cold WARMUP_RUNS=0 bash scripts/autodl/run_v6_synthetic.sh
```

AutoDL SIFT 模板：

```bash
export SIFT_BASE=/root/autodl-tmp/sift/sift_base.fvecs
export SIFT_QUERY=/root/autodl-tmp/sift/sift_query.fvecs
export SIFT_TRUTH=/root/autodl-tmp/sift/sift_groundtruth.ivecs
BASE_LIMIT=100000 QUERY_LIMIT=1000 BUILD_INDEX=0 INDEX=/root/autodl-tmp/index/sift100k.idx \
DATE_TAG=sift100k-v6 bash scripts/autodl/run_v6_sift_template.sh
```

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | 本地 `scripts/build.ps1` 可生成可执行文件 |
| file compaction 指标可观测 | 是 | aggressive `compaction_io_bytes = 196608`，SLA `compaction_io_bytes = 229376` |
| Recall@10 >= 0.95 | 是 | 三组本地 smoke 均为 0.9986 |
| WAL records 等于 insert count | 是 | 三组均为 72 / 72 |
| SLA P99 低于 aggressive | 是 | 本地 smoke 中 aggressive P99 = 0.4977 ms，SLA P99 = 0.4148 ms |
| SLA query-path interference 低于 aggressive | 是 | aggressive = 0.0065 ms/query，SLA = 0 |
| AutoDL 部署材料齐全 | 是 | 已新增部署文档、环境采集、cold cache、synthetic、SIFT 模板和证据打包脚本 |

## 本地 Smoke 指标

| 变体 | Recall@10 | P99 ms | WAL Records | Delta Active | Delta Sealed | Compaction Ops | Compaction I/O Bytes | Interference ms/query |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| no compaction | 0.9986 | 0.3001 | 72 | 72 | 0 | 0 | 0 | 0.0000 |
| aggressive file I/O | 0.9986 | 0.4977 | 72 | 24 | 48 | 6 | 196608 | 0.0065 |
| SLA file I/O | 0.9986 | 0.4148 | 72 | 16 | 56 | 7 | 229376 | 0.0000 |

## 结果分析

V6 本地 smoke 证明真实文件 I/O compaction 路径已经生效：aggressive 和 SLA 两组都产生了非零 `compaction_io_bytes`，并且 query-path interference 能被区分。aggressive 策略会在查询路径上执行 compaction，因此 P99 高于 no compaction；SLA 策略将 compaction 放到写入侧，查询路径 interference 为 0。

本地 Windows smoke 只能证明功能正确，不能作为最终 I/O 结论。最终性能结论必须在 AutoDL Linux 服务器上执行 `scripts/autodl/run_v6_synthetic.sh`，并至少保留 warm 与 cold 两组归档。

## 风险与降级方案

- 如果 AutoDL 上 `drop_caches` 权限不足，则 cold run 标注为 cold-like，只作为辅助结果。
- 如果通过 zip 上传导致 `.sh` 可执行位丢失，则执行 `find scripts -name "*.sh" -exec chmod +x {} \;`；V6 脚本内部已改为 `bash script.sh` 调用子脚本，避免再次触发 `Permission denied`。
- 如果 SIFT100K/SIFT1M 建图过慢，不使用当前 exact builder 现场建图，改为预构建索引或先跑 SIFT10K/SIFT100K 子集。
- 如果真实文件 I/O 干扰不明显，提高 `COMPACTION_IO_BYTES_PER_VECTOR`，例如从 64 KB 调到 256 KB。
- 如果 SLA 策略牺牲写入延迟过多，调高 `SLA_P99_MS` 或增大 `DELTA_COMPACTION_THRESHOLD`。

## 归档结果

- `archive/results/v6-fileio-local-2026-05-28.txt`
- `archive/configs/v6-fileio-local-2026-05-28.json`
- `archive/logs/v6-fileio-local-2026-05-28.log`
- `archive/build_info/v6-fileio-local-2026-05-28.txt`
- `docs/AUTODL_DEPLOYMENT.md`

## 下一步

将仓库上传到 AutoDL，在数据盘上执行 V6 synthetic warm/cold 实验并归档。若要做最终答辩数据，再准备 SIFT100K/SIFT1M 官方 `.ivecs`，并为大规模图索引准备预构建索引或近似建图器。
