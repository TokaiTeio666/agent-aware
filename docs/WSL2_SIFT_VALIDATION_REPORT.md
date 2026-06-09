# WSL2 SIFT10K / SIFT100K 验证报告

日期：2026-06-05  
运行环境：WSL2 Ubuntu，项目路径 `/mnt/d/agent-aware`  
数据目录：`/mnt/d/agent-aware/data/sift`  
验证目标：在 WSL2 上使用真实 SIFT 文件验证 SIFT10K 与 SIFT100K 的图索引搜索、20% resident memory 约束和 Recall@10。

## 1. 环境与约束

| 项目 | 值 |
|---|---|
| WSL 发行版 | Ubuntu, WSL2 |
| 项目路径 | `/mnt/d/agent-aware` |
| 数据文件 | `sift_base.fvecs`, `sift_query.fvecs`, `sift_groundtruth.ivecs` |
| 编译命令 | `bash scripts/linux/build.sh` |
| 二进制 | `build/agentmem_flow` |
| I/O 模式 | `pread` |
| 查询数 | 100 |
| K | 10 |
| 内存约束 | `--memory-budget-ratio 0.20 --enforce-memory-budget` |

注意：

- 本次运行在 WSL2 的 `/mnt/d` Windows 挂载路径上，延迟明显受到跨文件系统访问影响；结果适合验证功能、召回和内存合规，不作为最终 NVMe/ext4 性能数字。
- SIFT10K/SIFT100K 是 SIFT1M base 的子集，官方 `sift_groundtruth.ivecs` 针对完整 SIFT1M。程序检测到 truth id 超过当前 `base_limit` 后，自动回退为当前子集 exact brute force truth。
- `io_mode_effective=pread`，本次没有验证原生 `O_DIRECT` 或 `io_uring`。

## 2. 结果总览

| 数据集 | 配置 | Recall@10 | QPS | P99 延迟 | SSD reads/query | Resident/Raw | 内存合规 | 结论 |
|---|---|---:|---:|---:|---:|---:|---|---|
| SIFT10K | `lsh-rp`, BFS, early-stop min 192 | 0.9990 | 3.8299 | 731.7332 ms | 561.7500 | 11.42% | 是 | 召回与内存均达标 |
| SIFT100K | `lsh-rp`, BFS, early-stop min 192 | 0.9640 | 2.4614 | 1678.1738 ms | 1934.1100 | 9.06% | 是 | 召回与内存均达标 |
| SIFT10K | `lsh-rp` + agent cache, resident 接近 20% | 0.9990 | 8.6361 | 278.0768 ms | 550.7600 | 19.10% | 是 | 召回不变，延迟改善 |
| SIFT100K | `lsh-rp` + agent cache, resident 接近 20% | 0.9640 | 1.9070 | 2667.5456 ms | 1855.4200 | 19.06% | 是 | 读次数略降，但 P99 变差 |
| SIFT10K 反例 | `lsh-rp` + PQ(M=4,K=16) + ADC + rerank64 | 0.4460 | 4.6187 | 648.3881 ms | 594.5700 | 12.86% | 是 | 内存达标但召回严重不足，不推荐 |

核心结论：

- SIFT10K 与 SIFT100K 的稳健图搜索配置均通过 20% resident memory 约束。
- SIFT100K 在 100-query smoke 下达到 `Recall@10=0.9640`，超过 `>=0.95` 推荐线，也超过 `>=0.85` 最低线。
- “20% 常驻内存占比”应理解为上限而不是必须用满。SIFT10K 接近 20% 后延迟改善；SIFT100K 接近 20% 后 page cache 命中率提升到 4.07%，但 P99 变差，说明预算分配需要按数据规模调优。
- 从 synthetic 迁移来的小码本 PQ/ADC 配置在 SIFT10K 上召回降到 `0.4460`，说明 PQ/ADC 不能直接作为 SIFT 主线，需要单独调参或限制为 rerank/压缩辅助路径。
- 当前 P99 延迟偏高主要与 WSL2 `/mnt/d` 挂载路径和 `pread` 后端有关，不能等同于 Linux ext4/NVMe 的最终性能。

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
  --enforce-memory-budget \
  --run-type smoke
```

关键指标：

| 指标 | 值 |
|---|---:|
| graph_build_seconds | 0.863294 |
| recall_at_10 | 0.9990 |
| qps | 3.8299 |
| avg_latency_ms | 261.1027 |
| p95_latency_ms | 645.9141 |
| p99_latency_ms | 731.7332 |
| memory_budget_bytes | 1024000 |
| memory_resident_bytes | 584584 |
| memory_resident_ratio | 0.1142 |
| memory_budget_pass | 1 |
| ssd_reads_per_query_avg | 561.7500 |
| graph_expanded_per_query_avg | 192.0000 |

结论：SIFT10K 在 20% 内存约束下通过召回验证。由于官方 SIFT1M truth 对子集不适用，本次 truth 为 `file_out_of_range_exact_bruteforce`。

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
  --enforce-memory-budget \
  --run-type smoke
```

关键指标：

| 指标 | 值 |
|---|---:|
| graph_build_seconds | 8.13294 |
| recall_at_10 | 0.9640 |
| qps | 2.4614 |
| avg_latency_ms | 406.2738 |
| p95_latency_ms | 1359.1952 |
| p99_latency_ms | 1678.1738 |
| memory_budget_bytes | 10240000 |
| memory_resident_bytes | 4638664 |
| memory_resident_ratio | 0.0906 |
| memory_budget_pass | 1 |
| ssd_reads_per_query_avg | 1934.1100 |
| graph_expanded_per_query_avg | 192.0000 |

结论：SIFT100K 在 20% 内存约束下达到 `Recall@10=0.9640`，满足当前推荐达标线。构图时间为 8.13s，说明 `lsh-rp` 路径可支撑 SIFT100K 现场构建。

## 5. 接近 20% 常驻内存占比对照

为回答“如果常驻内存占比接近 20% 会怎样”，在原达标配置基础上开启 agent page cache，并将 cache pages 控制在 20% 预算内。

| 数据集 | cache pages | memory_bytes_cache | Resident/Raw | Recall@10 | QPS | P99 延迟 | SSD reads/query | page cache hit rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| SIFT10K | 96 | 393216 | 19.10% | 0.9990 | 8.6361 | 278.0768 ms | 550.7600 | 1.96% |
| SIFT100K | 1250 | 5120000 | 19.06% | 0.9640 | 1.9070 | 2667.5456 ms | 1855.4200 | 4.07% |

对比无 cache 的达标配置：

| 数据集 | 无 cache Resident/Raw | 近 20% Resident/Raw | QPS 变化 | P99 变化 | reads/query 变化 |
|---|---:|---:|---:|---:|---:|
| SIFT10K | 11.42% | 19.10% | 3.8299 -> 8.6361 | 731.7332 -> 278.0768 ms | 561.7500 -> 550.7600 |
| SIFT100K | 9.06% | 19.06% | 2.4614 -> 1.9070 | 1678.1738 -> 2667.5456 ms | 1934.1100 -> 1855.4200 |

结论：20% 是硬上限，不是越接近越好。SIFT10K 中额外 page cache 有收益；SIFT100K 中 cache 确实减少了约 4.1% 的物理读，但 cache 管理成本和 WSL2 `/mnt/d` I/O 波动使尾延迟变差。因此当前 SIFT100K 推荐配置仍保留 9.06% resident 的 no-cache 档，后续应在 ext4/NVMe 上重新评估 cache 容量。

## 6. PQ/ADC 迁移反例

合成数据中表现良好的 `PQ(M=4,K=16) + ADC + rerank64` 直接迁移到 SIFT10K 后，结果如下：

| 指标 | 值 |
|---|---:|
| recall_at_10 | 0.4460 |
| qps | 4.6187 |
| p99_latency_ms | 648.3881 |
| memory_resident_ratio | 0.1286 |
| memory_budget_pass | 1 |
| memory_bytes_pq_codes | 40000 |
| memory_bytes_pq_codebooks | 32808 |

结论：PQ/ADC 路径内存合规，但当前 SIFT 召回不合格。后续如果要使用 PQ，需要重新设计 SIFT 参数，例如更高 `rerank_topk`、更大的候选集、更合适的 `M/K`，或只将 PQ 用于路由/压缩辅助而不是主搜索距离。

## 7. 原始日志

| 文件 | 内容 |
|---|---|
| `archive/results/wsl-sift10k-lsh-budget-20260605-130016.txt` | SIFT10K 达标图搜索结果 |
| `archive/logs/wsl-sift10k-lsh-budget-20260605-130016.log` | SIFT10K 达标图搜索日志 |
| `archive/results/wsl-sift100k-lsh-budget-20260605-130120.txt` | SIFT100K 达标图搜索结果 |
| `archive/logs/wsl-sift100k-lsh-budget-20260605-130120.log` | SIFT100K 达标图搜索日志 |
| `archive/results/wsl-sift10k-lsh-cache20-20260605-131427.txt` | SIFT10K 接近 20% cache 对照结果 |
| `archive/logs/wsl-sift10k-lsh-cache20-20260605-131427.log` | SIFT10K 接近 20% cache 对照日志 |
| `archive/results/wsl-sift100k-lsh-cache20-20260605-131500.txt` | SIFT100K 接近 20% cache 对照结果 |
| `archive/logs/wsl-sift100k-lsh-cache20-20260605-131500.log` | SIFT100K 接近 20% cache 对照日志 |
| `archive/results/wsl-sift10k-m2-pq-budget-20260605-125914.txt` | SIFT10K PQ/ADC 反例结果 |
| `archive/logs/wsl-sift10k-m2-pq-budget-20260605-125914.log` | SIFT10K PQ/ADC 反例日志 |

## 8. 下一步

| 优先级 | 事项 | 目标 |
|---|---|---|
| P0 | 将 SIFT 受限内存配置加入 `scripts/linux/run_sift_local.sh` | 避免每次手写 `--memory-budget-*` 参数 |
| P0 | 在 WSL ext4 或原生 Linux SSD 上复跑 | 获得更可信的 P99 / QPS / I/O 结果 |
| P1 | 跑 SIFT100K 多档 sweep | 比较 `search_width=384/512/768/1024` 与 early-stop min |
| P1 | 单独调优 SIFT PQ/ADC | 找到既内存合规又 Recall>=0.95 的压缩配置 |
| P2 | 跑 SIFT1M full base | 使用官方 `.ivecs`，避免子集 exact truth 回退 |
