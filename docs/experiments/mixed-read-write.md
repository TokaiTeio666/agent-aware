# SIFT1M 动态混合读写对照实验

## 实验结论

2026-07-11 在同一台 WSL2 主机上完成四组 SIFT1M 配对实验。每组先预热一次，再正式运行三次；下表报告三次中位数，括号内为最小值到最大值。

| 配置 | 实际读写比 | Read QPS | Write QPS | Read P95 (ms) | Read P99 (ms) | Delta search (ms/read) | Compaction 次数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 纯读，compaction 关 | 100/0 | 703.18 (695.36-703.68) | 0 | 6.70 (6.67-6.71) | 8.36 (8.23-8.84) | 0.002 | 0 |
| 95/5，compaction 关 | 95/5 | 647.05 (645.43-649.09) | 34.06 (34.00-34.19) | 7.23 (7.12-7.37) | 8.76 (8.63-8.95) | 0.205 | 0 |
| 95/5，compaction 开 | 95/5 | 653.09 (621.66-686.04) | 34.39 (32.75-36.12) | 7.16 (6.77-8.14) | 8.68 (8.21-9.57) | 0.206 | 1 (每轮均为 1) |
| 80/20，compaction 开 | 80/20 | 578.23 (566.33-580.71) | 144.58 (141.62-145.19) | 8.31 (8.22-8.65) | 9.84 (9.66-10.27) | 0.912 | 9 (8-9) |

严格配对结果表明：

- 95/5、关闭 compaction 相对纯读的 read QPS 下降 8.0%，P95 增加 7.9%；主要新增开销来自 Delta search。
- 95/5 下开启后台 compaction 的中位 read QPS 与 P95 没有退化，但三轮范围明显扩大，因此不能据此宣称 compaction 提升性能。
- 从 95/5 增加到 80/20 后，write QPS 增加 320.4%，read QPS 下降 11.5%，P95 增加 16.0%；Delta search 从 0.206 ms/read 增至 0.912 ms/read。
- 四组的 graph reads/read 中位数均约为 153.95，说明上述差异不是主图搜索宽度变化造成的。

## 实验环境与口径

| 项目 | 设置 |
| --- | --- |
| 日期 | 2026-07-11，Asia/Shanghai |
| 系统 | WSL2，Linux 6.6.87.2-microsoft-standard-WSL2 |
| CPU | Intel Core i7-14700HX，14 cores / 28 logical CPUs |
| 数据与索引 | `data/sift/sift_base.fvecs`，1,000,000 vectors；`indexes/sift.idx` |
| 构建 | Release；CMake 检测到 liburing |
| 查询 | 4 reader threads，Top-K 10，search width 156，entry count 64，beam width 16 |
| I/O 与缓存 | 请求 `io_uring`，Graph-Aware 2Q；当前 mixed JSON 不记录 effective I/O mode |
| 写入 | 1 writer thread；insert/update/delete = 70%/25%/5% |
| 时长 | 每配置预热 1 次，正式运行 3 次，每次 30 秒 |
| 正确性与吞吐 | 吞吐轮次关闭 exact Recall；另跑限速 Recall evidence |
| 代码状态 | commit `b691e77`，工作树 dirty；本轮同时修复了 timed benchmark 比例调度 |

正式轮次采用轮换顺序：第一轮为纯读、95/5 关闭 compaction、95/5 开启 compaction、80/20 开启 compaction；第二轮从 95/5 开启 compaction 开始；第三轮从 80/20 开启 compaction 开始。每次使用独立且启动前清空的 dynamic 目录。

## 三轮明细

| 配置 | 轮次 | Read QPS | Write QPS | P95 (ms) | P99 (ms) | Delta (ms/read) | Flush (ms) | SSTables | Compaction 次数 / 总耗时 (ms) | Recovery (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 纯读 | 1 | 703.18 | 0 | 6.67 | 8.84 | 0.002 | 8.30 | 0 | 0 / 0 | 4.83 |
| 纯读 | 2 | 703.68 | 0 | 6.70 | 8.23 | 0.002 | 7.09 | 0 | 0 / 0 | 5.06 |
| 纯读 | 3 | 695.36 | 0 | 6.71 | 8.36 | 0.002 | 8.97 | 0 | 0 / 0 | 5.22 |
| 95/5，compaction 关 | 1 | 645.43 | 34.00 | 7.12 | 8.63 | 0.200 | 6.56 | 3 | 0 / 0 | 10.51 |
| 95/5，compaction 关 | 2 | 647.05 | 34.06 | 7.37 | 8.95 | 0.209 | 6.82 | 3 | 0 / 0 | 11.79 |
| 95/5，compaction 关 | 3 | 649.09 | 34.19 | 7.23 | 8.76 | 0.205 | 6.81 | 3 | 0 / 0 | 9.95 |
| 95/5，compaction 开 | 1 | 621.66 | 32.75 | 8.14 | 9.57 | 0.201 | 7.64 | 2 | 1 / 9.14 | 9.42 |
| 95/5，compaction 开 | 2 | 686.04 | 36.12 | 6.77 | 8.21 | 0.206 | 9.05 | 2 | 1 / 7.55 | 10.78 |
| 95/5，compaction 开 | 3 | 653.09 | 34.39 | 7.16 | 8.68 | 0.208 | 6.34 | 2 | 1 / 7.78 | 8.82 |
| 80/20，compaction 开 | 1 | 566.33 | 141.62 | 8.65 | 10.27 | 0.917 | 6.29 | 3 | 8 / 155.39 | 29.18 |
| 80/20，compaction 开 | 2 | 578.23 | 144.58 | 8.22 | 9.66 | 0.889 | 9.34 | 2 | 9 / 172.89 | 28.74 |
| 80/20，compaction 开 | 3 | 580.71 | 145.19 | 8.31 | 9.84 | 0.912 | 11.80 | 2 | 9 / 177.07 | 30.91 |

三轮所有结果均为 `status=completed`，实际读比例与配置值的误差小于 0.01 个百分点，CRUD 比例接近 70/25/5，`write_pipeline.failed=0`。

## 复现命令

以下是 95/5、开启后台 compaction 的单轮命令。其他配置只改变 `read_ratio`、`write_ratio`、writer 数量和 compaction 开关；每次必须使用新的 `dynamic_dir`。

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift.idx \
  --dynamic_dir build/mixed95_compaction_dynamic \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 1 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --insert_ratio 0.70 \
  --update_ratio 0.25 \
  --delete_ratio 0.05 \
  --recall_sample_rate 0 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --compaction_interval_ms 1000 \
  --topk 10 \
  --search_width 156 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/mixed95_compaction.json
```

## Recall 正确性证据

独立场景使用 1 reader、1 writer、95/5 CRUD、每秒最多一次 exact Recall 回算。30 秒内取得 29 个有效样本：Recall@10 为 0.9966，read sequence 覆盖 1-233，exact visible Delta 平均 112.34 条，exact deleted 平均 4.59 条；169 次 insert、56 次 update、11 次 delete 均成功提交。

该场景仅证明 base/delta merge、update/delete 可见性和 read sequence 一致性，不用于吞吐解释。写入量未达到 compaction 合并阈值，因此 compaction 正常执行的证据来自上表的性能场景。

## 结论边界

- 结果来自 WSL2 和 dirty 工作树，不替代真实 NVMe、clean commit 的最终验收。
- 每个 benchmark 进程在 workload 计时前训练一次 PQ；训练时间约 28-30 秒，不计入 QPS。
- mixed JSON 记录请求参数但不记录 effective I/O mode，不能仅凭本报告证明 io_uring 没有 fallback。
- 结果只支持本文列出的三组严格配对关系，不应与 `build/` 中其他时长、缓存状态或旧实现产物横向比较。
