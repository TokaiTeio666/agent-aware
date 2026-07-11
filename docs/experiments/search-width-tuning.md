# SIFT1M Search Width 调优记录

> 更新日期：2026-07-11  
> 当前结论：在已测 `128/156/192` 三个点中，推荐 `search_width=156` 作为后续实验统一口径。

## 实验目标


## 固定条件

| 条件 | 取值 |
| --- | --- |
| dataset | SIFT1M，base 1,000,000，dim 128 |
| query window | `5000-5999`，共 1,000 条 |
| top-k | `10` |
| beam width | `16` |
| entry | `single-medoid`，seed id `123742` |
| PQ | 32 subspaces，256 centroids，train limit 100,000，8 iterations |
| exact rerank top-k | `100` |
| cache | `graph-aware-2q`，15,000 pages |
| I/O | io_uring，batch 16，depth 32，O_DIRECT effective |
| index | `indexes/sift.idx`，不重建 |
| binary commit | `91406a839e42500f11e8c13b9073aa46a6b5775a`，dirty worktree |
| environment | WSL2 开发环境 |

三个 JSON 的 `binary_mtime` 均为 `1783690363`，用于确认它们来自同一二进制。由于运行时工作区包含未提交修改，复现时还应记录当前 diff 或固定一个新 commit。

## 运行命令

```bash
SW=156

./build/agent-aware \
  --sift-dir data/sift \
  --base-limit 1000000 \
  --query-offset 5000 \
  --query-limit 1000 \
  --top-k 10 \
  --search-width "$SW" \
  --beam-width 16 \
  --entry-count 64 \
  --entry-strategy single-medoid \
  --enable-pq 1 \
  --pq-subspaces 32 \
  --pq-centroids 256 \
  --pq-train-limit 100000 \
  --pq-iterations 8 \
  --rerank-topk 100 \
  --cache-policy graph-aware-2q \
  --cache-pages 15000 \
  --memory-budget-ratio 0.20 \
  --io-mode io_uring \
  --io-batch 16 \
  --io-depth 32 \
  --index-path indexes/sift.idx \
  --rebuild-index 0 \
  --run-id "search-width-${SW}-baseline" \
  --output-json "build/search_width_${SW}_baseline.json"
```

## 结果

| search width | Recall@10 | Avg ms | P95 ms | P99 ms | QPS | Reads/query | Rerank reads/query | Submit calls/query |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `128` | `0.9610` | `49.8550` | `66.2511` | `70.2307` | `20.0570` | `106.872` | `28.679` | `23.997` |
| **`156`** | **`0.9748`** | **`48.1043`** | **`62.3861`** | **`66.2874`** | **`20.7870`** | `109.815` | **`6.364`** | **`12.505`** |
| `192` | `0.9815` | `99.1154` | `124.6940` | `129.5860` | `10.0888` | `136.624` | `0.930` | `11.079` |

`128` 的结果采用复跑值；首次运行曾出现 Avg `79.72 ms`、P99 `113.43 ms`，说明 WSL2 环境存在明显运行间波动。`156` 与 `192` 当前各只有一轮，因此上表适合确定候选点，不足以单独支持统计显著性结论。

## 结论

`156` 相对 `128` 增加 `1.38` 个百分点 Recall，同时 Avg 降低约 `3.5%`、P99 降低约 `5.6%`。虽然总 reads/query 增加约 `2.8%`，但 rerank reads/query 从 `28.679` 降至 `6.364`，I/O submit calls/query 从 `23.997` 降至 `12.505`。这说明端到端性能不能只看搜索扩展数，还要同时观察候选物化、exact rerank 和批量提交行为。

`192` 相对 `156` 只增加 `0.67` 个百分点 Recall，但 reads/query 增加约 `24.4%`，Avg 和 P99 均接近翻倍。当前没有理由把 `192` 作为主展示参数。

因此后续默认实验固定：

```text
search_width=156
beam_width=16
query_offset=5000
query_limit=1000
cache_pages=15000
io_mode=io_uring
```

## 待完成验证

1. 对 `156` 再运行两轮，报告 Avg/P95/P99/QPS 的中位数与波动范围。
2. 补测 `144` 和 `168`，确认局部拐点不是离散采样造成。
4. 在真实 NVMe 或比赛环境复跑，WSL2 数据只作为开发验证。
5. 记录进程真实 RSS；当前 `estimated_resident_ratio=0.198505` 只统计模型、目录和 cache 的预算估算，不等同于进程总常驻内存。
