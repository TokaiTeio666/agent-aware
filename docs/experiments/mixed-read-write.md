# 动态混合读写对照（可复现结果）

> 数据：[`results/2026-10-25-windows-msvc-v2/mixed/`](../../results/2026-10-25-windows-msvc-v2/mixed/)（4 配置 × 3 轮）
> 表内数字为单位置中位数（`sorted[1]`，3 轮）；实验驱动的运行带 `--duration_sec`，指标被包在
> `scenarios[0]` 里，`summary.md` 的混合读写表需要修复后的汇总工具才能正确读出（见 §6）
> 与 `…/failure/`（干净关闭 / 重开）、[`results/2026-10-25-windows-msvc/stability/`](../../results/2026-10-25-windows-msvc/stability/)（调度器修复前后对照）
> 环境与口径：[`reproducibility.md`](reproducibility.md)

## 1. 矩阵

| 项 | 取值 |
| --- | --- |
| 数据集 / 索引 | 合成 `gmm-v1` 100k×128；V3 索引（degree 64、page 4096、PQ 32×256） |
| 并发 | 4 reader（各开一份 `PackedGraphEngine`）+ 1 writer |
| 业务比例 | 纯读 100/0；95/5；80/20 |
| 写事件 | insert 70% / update 25% / delete 5% |
| 时长 | 每轮 30 s，每配置 3 轮，每轮全新动态目录 |
| 搜索 | `search_width=156`、`beam_width=16`、`topk=10`、`pread`、`graph-aware-2q` |
| compaction | 关闭，或每 1,000 ms 后台执行一次 `compact_once` |
| Recall 采样 | 关闭（`recall_sample_rate=0`，JSON 中 `recall_at_10=null`） |

## 2. 结果（3 轮中位数，括号内为范围）

| 配置 | Read QPS | Write QPS | Read P95 (ms) | Read P99 (ms) | Delta search (ms/read) | Recovery (ms) | Compaction ok/attempt | SSTables | peak RSS MB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 纯读，compaction 关 | 2245.1 (2069.3-2275.3) | 0 | 2.845 | 3.390 | 0.001 | 4.02 | 0/0 | 0 | 107.98 |
| 95/5，compaction 关 | 1204.9 (1202.1-1209.3) | 63.42 | 3.789 | 4.537 | 0.460 | 88.63 | 0/0 | 5 | 116.70 |
| 95/5，compaction 开 | 1175.2 (1159.3-1178.7) | 61.85 | 4.016 | 4.715 | 0.506 | 142.38 | 3/29 | 2 | 118.21 |
| 80/20，compaction 开 | 248.2 (242.8-248.9) | 62.05 | 4.465 | 5.148 | 0.607 | 117.54 | 3/29 | 2 | 112.85 |

### 三轮明细（关键字段）

| 配置 | 轮次 | read ops | write ops | delta records | SSTables | compaction | recovery ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 纯读 | 1/2/3 | 62,109 / 67,383 / 68,299 | 0 | 0 | 0 | 0 | 4.02 / 4.05 / 3.74 |
| 95/5 关 compaction | 1/2/3 | 36,290 / 36,081 / 36,157 | 1,910 / 1,899 / 1,903 | 1,907 / 1,896 / 1,900 | 5 | 0 | 86.7 / 88.8 / 88.6 |
| 95/5 开 compaction | 1/2/3 | 36,155 / 35,663 / 36,153 | 1,903 / 1,877 / 1,903 | 1,900 / 1,874 / 1,900 | 2 | 3 | 110.9 / 145.5 / 142.4 |
| 80/20 开 compaction | 1/2/3 | 7,636 / 7,460 / 7,644 | 1,909 / 1,865 / 1,911 | 1,906 / 1,862 / 1,908 | 2 | 3 | 132.2 / 117.5 / 97.5 |

读/写操作比例精确符合配置（95/5 与 80/20 的 read_ops/write_ops 比值分别为 19.0 与 4.0）；
`write_pipeline.failed` 为 0；所有轮次 `status=completed`。

## 3. 解读

### 3.1 写吞吐约 63 条/秒，是写路径的上限，不是调度比例

80/20 下时间窗内应执行约 3,800 次写，实际约 1,900 次；两个配置的 write QPS 都落在 62–63。
原因在写路径本身：`execute_write` → `DynamicWriteManager::commit` 在 commit 线程上对每组 WAL 记录
同步 `fdatasync`，且 commit 只有单线程（`commit_loop`）。因此：

- **不能用 write QPS 比较 95/5 与 80/20**——两者都被同一上限夹住；
- 读 QPS 的差异不是 delta 规模造成的：95/5 与 80/20 在 30 秒内写入量相同（约 1,900 条），
  两者的 delta 记录数也几乎相同，但 read QPS 相差约 4.9 倍。

> **重要**：修复后的调度器是"严格按序"执行（上一个槽位完成后才领取下一个）。它让比例精确，
> 但代价是读、写不再真正并行——80/20 的每个读槽位之间要等 4 个写槽位完成，而每个写槽位都在
> 等 `fdatasync`（约 15 ms）。所以 80/20 的 read QPS 实际是"写吞吐 × 4"的结果，而不是读侧能力。
> 修复前的实现允许读写重叠，但比例漂移且读吞吐随运行时长持续下降（见 §3.3）。
> 两种实现都不能单独回答"读写并发时读侧能跑到多少"，下一轮需要**读写混合但不互相阻塞**的
> 调度（读写各自的队列 + 按比例配额的令牌桶），本轮未能实现。

### 3.2 delta 增长对读侧的影响

- 30 s 修复后矩阵里，95/5 与 80/20 的 delta 记录数都在 1,900 左右，delta search 为 0.46 vs 0.61 ms/read；
- 10 s 的稳定性对照里，纯读 delta search 为 0.001 ms/read，80/20 为 0.13 ms/read（delta 约 600 条）；
- 0.13 → 0.46 → 0.61 ms 对应 delta 约 600 → 1,900 → 1,900 条，说明 delta 扫描成本随记录数近似线性增长；
- 因此本组数据**不能**用来画"delta 规模 → 读延迟"曲线。要得到这条曲线，需要让写不受
  `fdatasync` 限制（例如关闭 WAL 同步）或在固定 delta 规模下跑纯读。

### 3.3 并发调度器缺陷（本轮修复）

第一版矩阵（`results/2026-10-25-windows-msvc/`，即 `-v1-bench-bug` 口径）使用的是
"CAS 领取槽位 + `yield()`"的调度：等待中的 reader 与上一个写操作并发、下一个写操作与上一个
读操作并发，14 代混合核上竞争线程长期占住 P 核。表现为：

| 检查 | 修复前 | 修复后 |
| --- | --- | --- |
| 纯读 4 reader + 1 空闲 writer，5 s vs 30 s read QPS | 2,470–2,500 → 2,070–2,280（不同策略均下滑） | 2,379–2,519（5 轮 10 s）vs 2,384（30 s 单轮），不随时长下滑 |
| 80/20 + compaction，单次 30 s 内 read QPS（分段时间窗） | 356 → 315 → 252（一轮之内持续下降） | 2,136–2,222（10 s 五轮，compaction 开），1,838–1,868（30 s 三轮，无 compaction 对照） |
| 80/20 write QPS（设计值 128/s） | 62.5–62.9 | 62.8–63.3（仍受 §3.1 限制） |

修复前的数据来自 `results/2026-10-25-windows-msvc/`（30 s 矩阵）与
`results/2026-10-25-windows-msvc/stability-v1-bench-bug/`（5 s / 30 s 补充测量）。
`results/2026-10-25-windows-msvc-v1-bench-bug/` 只有一份说明，不含 JSON。

修复后的 5 轮 10 s 重复（`results/2026-10-25-windows-msvc/stability/`）：

| 配置 | Read QPS（5 轮） | Write QPS | Read P95 ms |
| --- | ---: | ---: | ---: |
| 纯读（4 reader + 1 空闲 writer） | 2522 / 2462 / 2379 / 2382 / 2439 | 0 | 2.56–2.73 |
| 80/20 + compaction | 2190 / 2222 / 2136 / 2175 / 2202 | 62.8–63.3 | 2.70–2.92 |

注意 80/20 的 read QPS（约 2,200）**高于** 95/5 的 30 s 矩阵（约 1,200）：在严格按序的调度下，
读吞吐由"每个读槽位前面排了多少个写槽位"决定——95/5 每读前有 19 个写，80/20 只有 4 个。
这与 §3.1 的结论一致：read QPS 反映的是写路径速度与读写比例，而不是 delta 或读侧能力。
因此**不同读写比例、不同时长之间的 read QPS 不可横向比较**，只能同配置比较。

### 3.4 compaction 的影响

95/5 下每秒一次后台 compaction 把 SSTable 从 5 个压到 2 个，恢复时间从 88.6 ms 增加到 142.4 ms，
read QPS 从 1,204.9 降到 1,175.2（-2.5%），P95 从 3.79 升到 4.02 ms。compaction 本身不是读路径
瓶颈，但会与 flush / 恢复抢 I/O。80/20 下 compaction 同样每秒尝试一次、实际执行 3 次（29 次尝试中
其余因不足 2 个 SSTable 跳过）。

## 4. 恢复检查（干净关闭 → 重开）

同一动态目录先跑 95/5（30 s）并干净关闭，再只读重开：

| 文件 | 已提交写记录 | 重开后可见 delta 记录 | SSTables | WAL 段 | 重放 WAL 记录 | 截断 WAL 段 | 耗时 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `clean-close.json` | 1,862 | 1,859 | 5 | 2 | 0 | 0 | 139.16 |
| `clean-reopen.json` | 0 | 0 | 0 | 3 | 0 | 0 | 4.07 |

说明：

- 关闭前 flush 把 1,859 条可见记录写入 5 个 SSTable，`wal_checkpoint` 覆盖到它们，因此重开时
  WAL 记录为 0、直接从 SSTable 重建；
- `clean-reopen` 是**独立进程**，它的 "committed=0" 是本次运行的写入量，不是丢失——恢复结果
  在第一次打开时已计入（见 recovery 字段的 sstable_records）；
- **这不是崩溃一致性测试**：没有在 flush/WAL 写入中途杀进程。崩溃窗口（WAL 尾部半条记录、
  manifest rename 窗口）只由 `wal_truncated_segments` / `wal_truncation_reason` 字段暴露，
  本轮没有触发。

## 5. 结论

1. 动态写入路径功能正确：比例精确、无写失败、干净关闭后完整恢复。
2. 当前写的瓶颈是"每条记录同步 `fdatasync` + 单线程 commit"，约 63 条/秒；要提高写吞吐应做
   组提交（`wal_group_max_records`/`wal_group_max_delay_us` 已存在但未被利用到效益）或异步刷盘。
3. 本轮的并发调度让读写严格串行，read QPS 受写路径拖累，**不能**用来论证"高并发读"或
   "delta 增长曲线"。下一轮需要独立的读写调度与固定写吞吐的 delta 压测。
4. compaction 在 95/5 下代价为 -2.5% read QPS、+60% 恢复时间，属于可接受范围。

## 6. 复现

```bat
bench_mixed_rw.exe --data_path build\repro_gmm\sift_base.fvecs --base_count 100000 ^
  --query_count 2000 --index_path build\i100.idx --dynamic_dir build\scratch\mixed ^
  --output results\<run>.json --duration_sec 30 --read_threads 4 --write_threads 1 ^
  --read_ratio 0.8 --write_ratio 0.2 --insert_ratio 0.7 --update_ratio 0.25 ^
  --delete_ratio 0.05 --recall_sample_rate 0 --enable_compaction 1 ^
  --compaction_background 1 --compaction_interval_ms 1000 --topk 10 ^
  --search_width 156 --beam_width 16 --io_mode pread --cache_policy graph-aware-2q ^
  --compaction_interval_ms 1000
```

完整矩阵见 [`scripts/README.md`](../../scripts/README.md)。
