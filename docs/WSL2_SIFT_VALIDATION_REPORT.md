# WSL2 SIFT10K / SIFT100K 验证报告

日期：2026-06-05  
运行环境：WSL2 Ubuntu，项目路径 `/mnt/d/agent-aware`  
数据目录：`/mnt/d/agent-aware/data/sift`

本报告是当前正式验证的主体文档之一。它只保留 `SIFT10K` 与 `SIFT100K` 的验证结果，不再引用本地 synthetic / local smoke 结果。

## 1. 环境与约束

| 项目 | 值 |
|---|---|
| WSL 发行版 | Ubuntu, WSL2 |
| 项目路径 | `/mnt/d/agent-aware` |
| 数据文件 | `sift_base.fvecs`, `sift_query.fvecs`, `sift_groundtruth.ivecs` |
| 构建命令 | `bash scripts/linux/build.sh` |
| 二进制 | `build/agentmem_flow` |
| I/O 模式 | `io_mode_effective=pread` |
| 查询数 | 100 |
| K | 10 |
| 内存预算 | `--memory-budget-ratio 0.20 --enforce-memory-budget` |

注意：

- 仓库和数据位于 `/mnt/d`，因此结果适合验证功能、召回和 resident memory 合规性。
- 这些结果不能直接作为最终 Linux ext4 / NVMe 尾延迟与吞吐结论。
- 当前没有原生 `O_DIRECT` 或原生 `io_uring` 后端验证结果。

## 2. 结果总览

| 数据集 | 配置 | Recall@10 | QPS | P99 latency | SSD reads/query | Resident/Raw | 内存合规 | 结论 |
|---|---|---:|---:|---:|---:|---:|---|---|
| SIFT10K | `lsh-rp`, BFS, early-stop min 192 | 0.9990 | 3.8299 | 731.7332 ms | 561.7500 | 11.42% | 是 | 达标 |
| SIFT100K | `lsh-rp`, BFS, early-stop min 192 | 0.9640 | 2.4614 | 1678.1738 ms | 1934.1100 | 9.06% | 是 | 达标 |
| SIFT10K | `lsh-rp` + agent cache，resident 接近 20% | 0.9990 | 8.6361 | 278.0768 ms | 550.7600 | 19.10% | 是 | 预算对照 |
| SIFT100K | `lsh-rp` + agent cache，resident 接近 20% | 0.9640 | 1.9070 | 2667.5456 ms | 1855.4200 | 19.06% | 是 | 预算对照 |
| SIFT10K 反例 | `PQ(M=4,K=16) + ADC + rerank64` | 0.4460 | 4.6187 | 648.3881 ms | 594.5700 | 12.86% | 是 | 不达标 |

核心结论：

- `SIFT10K` 与 `SIFT100K` 均通过 `20%` resident memory 预算约束。
- `SIFT100K Recall@10=0.9640` 满足当前主线验证要求。
- 预算接近 `20%` 并不一定带来更优性能；SIFT100K 上 cache 反而使 P99 变差。
- `PQ/ADC` 当前配置不能作为 SIFT 主线。

## 3. SIFT10K 达标配置

命令摘要：

```bash
./build/agentmem_flow \
  --engine graph \
  --base data/sift/sift_base.fvecs \
  --query data/sift/sift_query.fvecs \
  --truth data/sift/sift_groundtruth.ivecs \
  --base-limit 10000 \
  --query-limit 100 \
  --k 10 \
  --layout packed \
  --packing bfs \
  --build-index \
  --graph-build-policy lsh-rp \
  --graph-degree 32 \
  --lsh-tables 8 \
  --lsh-bits 10 \
  --lsh-bucket-limit 64 \
  --approx-window 32 \
  --approx-random-samples 32 \
  --approx-candidate-limit 192 \
  --search-width 1024 \
  --search-early-stop \
  --search-early-stop-min 192 \
  --entry-count 256 \
  --routing-sample-count 1024 \
  --cache-policy none \
  --path-cache-policy none \
  --memory-budget-ratio 0.20 \
  --enforce-memory-budget
```

关键指标：

| 指标 | 值 |
|---|---:|
| graph_build_seconds | 0.863294 |
| recall_at_10 | 0.9990 |
| qps | 3.8299 |
| p99_latency_ms | 731.7332 |
| memory_resident_ratio | 0.1142 |
| memory_budget_pass | 1 |
| ssd_reads_per_query_avg | 561.7500 |

说明：官方 SIFT1M truth 对子集越界，因此该组结果使用 `file_out_of_range_exact_bruteforce` 口径。

## 4. SIFT100K 达标配置

命令摘要：

```bash
./build/agentmem_flow \
  --engine graph \
  --base data/sift/sift_base.fvecs \
  --query data/sift/sift_query.fvecs \
  --truth data/sift/sift_groundtruth.ivecs \
  --base-limit 100000 \
  --query-limit 100 \
  --k 10 \
  --layout packed \
  --packing bfs \
  --build-index \
  --graph-build-policy lsh-rp \
  --graph-degree 32 \
  --lsh-tables 8 \
  --lsh-bits 14 \
  --lsh-bucket-limit 64 \
  --approx-window 32 \
  --approx-random-samples 32 \
  --approx-candidate-limit 192 \
  --search-width 384 \
  --search-early-stop \
  --search-early-stop-min 192 \
  --entry-count 512 \
  --routing-sample-count 8192 \
  --cache-policy none \
  --path-cache-policy none \
  --memory-budget-ratio 0.20 \
  --enforce-memory-budget
```

关键指标：

| 指标 | 值 |
|---|---:|
| graph_build_seconds | 8.13294 |
| recall_at_10 | 0.9640 |
| qps | 2.4614 |
| p99_latency_ms | 1678.1738 |
| memory_resident_ratio | 0.0906 |
| memory_budget_pass | 1 |
| ssd_reads_per_query_avg | 1934.1100 |

说明：该结果是当前正式验证中的主线达标配置。

## 5. 接近 20% 预算的对照

| 数据集 | cache pages | memory_bytes_cache | Resident/Raw | Recall@10 | QPS | P99 latency | SSD reads/query | page cache hit rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| SIFT10K | 96 | 393216 | 19.10% | 0.9990 | 8.6361 | 278.0768 ms | 550.7600 | 1.96% |
| SIFT100K | 1250 | 5120000 | 19.06% | 0.9640 | 1.9070 | 2667.5456 ms | 1855.4200 | 4.07% |

结论：

- `20%` 是预算上限，不是目标占满值。
- SIFT10K 中适度 page cache 有收益。
- SIFT100K 中当前 cache 配置虽然略降 reads/query，但没有改善尾延迟。

## 6. PQ/ADC 反例

| 指标 | 值 |
|---|---:|
| recall_at_10 | 0.4460 |
| qps | 4.6187 |
| p99_latency_ms | 648.3881 |
| memory_resident_ratio | 0.1286 |
| memory_budget_pass | 1 |

结论：PQ/ADC 当前仅能作为调参对象，不能作为正式主线配置。

## 7. 原始日志

| 文件 | 内容 |
|---|---|
| `archive/results/wsl-sift10k-lsh-budget-20260605-130016.txt` | SIFT10K 达标结果 |
| `archive/logs/wsl-sift10k-lsh-budget-20260605-130016.log` | SIFT10K 达标日志 |
| `archive/results/wsl-sift100k-lsh-budget-20260605-130120.txt` | SIFT100K 达标结果 |
| `archive/logs/wsl-sift100k-lsh-budget-20260605-130120.log` | SIFT100K 达标日志 |
| `archive/results/wsl-sift10k-lsh-cache20-20260605-131427.txt` | SIFT10K near-20% 对照 |
| `archive/logs/wsl-sift10k-lsh-cache20-20260605-131427.log` | SIFT10K near-20% 日志 |
| `archive/results/wsl-sift100k-lsh-cache20-20260605-131500.txt` | SIFT100K near-20% 对照 |
| `archive/logs/wsl-sift100k-lsh-cache20-20260605-131500.log` | SIFT100K near-20% 日志 |
| `archive/results/wsl-sift10k-m2-pq-budget-20260605-125914.txt` | SIFT10K PQ/ADC 反例 |
| `archive/logs/wsl-sift10k-m2-pq-budget-20260605-125914.log` | SIFT10K PQ/ADC 日志 |
