# AgentMem-Flow 项目实验报告

生成日期：2026-06-09  
当前口径：正式验证仅采用 `SIFT10K` 与 `SIFT100K`，不再将本地 synthetic / local smoke 结果作为验证依据。

## 1. 实验环境

| 项目 | 值 |
|---|---|
| 主机系统 | Windows 11 |
| Linux 环境 | Ubuntu on WSL2 |
| 仓库路径 | `D:\agent-aware` / `/mnt/d/agent-aware` |
| 数据集 | `data/sift/sift_base.fvecs`, `sift_query.fvecs`, `sift_groundtruth.ivecs` |
| Linux 构建命令 | `bash scripts/linux/build.sh` |
| 二进制 | `build/agentmem_flow` |
| 当前 I/O 后端 | `pread` |
| 内存预算口径 | `--memory-budget-ratio 0.20 --enforce-memory-budget` |

说明：
- 当前归档中的正式验证结果来自 WSL2 对 `SIFT10K` / `SIFT100K` 的真实运行。
- 这些结果适合验证功能正确性、召回率和 resident memory 合规性。
- 由于仓库与数据位于 `/mnt/d` 挂载路径，P99 / QPS 不能视为最终 Linux ext4 / NVMe 性能结论。

## 2. 项目目标

项目目标是在受限内存条件下实现面向 SSD 的向量检索引擎，并验证：

- 在 `<=20%` resident memory 预算内完成 Top-K ANN 检索。
- 以 `Recall@10`、QPS、P99 latency、`ssd_reads_per_query` 和 `memory_budget_pass` 作为核心指标。
- 在真实 SIFT 数据集上验证图索引主路径，而不是依赖本地 synthetic smoke。

## 3. 核心接口

当前主程序为实验型 CLI，关键受限内存参数如下：

```bash
--memory-budget-ratio 0.20
--memory-budget-bytes N
--enforce-memory-budget
--allow-over-budget-for-debug
```

关键输出字段如下：

```text
memory_budget_ratio
memory_budget_bytes
memory_resident_bytes
memory_resident_ratio
memory_budget_pass
memory_bytes_cache
memory_bytes_router
memory_accounting_scope
```

Linux 侧运行脚本 `scripts/linux/run_sift_local.sh` 已支持通过环境变量透传预算：

```bash
MEMORY_BUDGET_RATIO=0.20
MEMORY_BUDGET_BYTES=...
ENFORCE_MEMORY_BUDGET=1
ALLOW_OVER_BUDGET_FOR_DEBUG=1
```

## 4. 正式验证范围

| 数据集 | Base Count | Query Count | Ground Truth | 状态 |
|---|---:|---:|---|---|
| SIFT10K | 10,000 | 100 | subset exact brute force fallback | 已验证 |
| SIFT100K | 100,000 | 100 | subset exact brute force fallback | 已验证 |
| SIFT1M full | 1,000,000 | 待运行 | 官方 `.ivecs` | 暂未运行 |

说明：
- 当前只保留 `SIFT10K` / `SIFT100K` 作为正式验证方法。
- 当官方 `sift_groundtruth.ivecs` 中的 id 超出当前 `base_limit` 时，程序会回退到当前子集上的 exact brute force truth，并输出 `truth=file_out_of_range_exact_bruteforce`。

## 5. 当前结果

### 5.1 达标配置

| 数据集 | 配置 | Recall@10 | QPS | P99 latency | SSD reads/query | Resident/Raw | 20% 合规 |
|---|---|---:|---:|---:|---:|---:|---|
| SIFT10K | `lsh-rp`, BFS, early-stop min 192, no cache | 0.9990 | 3.8299 | 731.7332 ms | 561.7500 | 11.42% | 是 |
| SIFT100K | `lsh-rp`, BFS, early-stop min 192, no cache | 0.9640 | 2.4614 | 1678.1738 ms | 1934.1100 | 9.06% | 是 |

结论：
- 两组结果均在 `--enforce-memory-budget` 下运行并通过 `memory_budget_pass=1`。
- `SIFT100K Recall@10=0.9640` 已达到当前主线验证目标。

### 5.2 接近 20% 预算的对照

| 数据集 | cache pages | Resident/Raw | Recall@10 | QPS | P99 latency | SSD reads/query | cache hit rate |
|---|---:|---:|---:|---:|---:|---:|---:|
| SIFT10K | 96 | 19.10% | 0.9990 | 8.6361 | 278.0768 ms | 550.7600 | 1.96% |
| SIFT100K | 1250 | 19.06% | 0.9640 | 1.9070 | 2667.5456 ms | 1855.4200 | 4.07% |

结论：
- `20%` 应理解为预算上限，而不是必须尽量占满。
- SIFT10K 接近 20% 时有收益；SIFT100K 则出现 reads/query 略降但尾延迟变差的现象。

### 5.3 PQ/ADC 反例

| 数据集 | 配置 | Recall@10 | QPS | P99 latency | Resident/Raw | 合规 |
|---|---|---:|---:|---:|---:|---|
| SIFT10K | `PQ(M=4,K=16) + ADC + rerank64` | 0.4460 | 4.6187 | 648.3881 ms | 12.86% | 是 |

结论：
- PQ/ADC 这组参数虽然内存合规，但召回不足，不能作为当前 SIFT 主线配置。

## 6. 当前结论

1. 项目已经具备可执行、可统计、可 fail-fast 的 `20%` resident memory 约束机制。
2. 当前正式验证结论只基于 `SIFT10K` / `SIFT100K`，不再引用本地 synthetic / local smoke 结果。
3. `SIFT100K` 在当前主线配置下达到 `Recall@10=0.9640`，同时 resident ratio 为 `9.06%`。
4. 当前结果仍基于 WSL2 `/mnt/d` + `pread`，因此只能证明功能、召回和内存合规，不代表最终原生 Linux I/O 性能。
5. `O_DIRECT` 与 `io_uring` 仍未进入原生后端验证阶段，当前不应宣称 `io_uring_enabled=1`。

## 7. 下一步

| 优先级 | 事项 | 目标 |
|---|---|---|
| P0 | 在 WSL ext4 或原生 Linux NVMe 上复跑 SIFT10K / SIFT100K | 获得可信的 P99 / QPS / I/O 数据 |
| P0 | 跑 SIFT1M full-base | 使用官方 `.ivecs` 形成最终展示结果 |
| P1 | 实现原生 `O_DIRECT` | 摆脱当前 `pread` fallback |
| P1 | 实现真正的 `io_uring` 批量异步预取 | 使 `io_uring_enabled=1` 成为真实后端能力 |
| P1 | 按 SIFT 数据重新调 PQ/ADC | 找到兼顾 Recall 与内存预算的压缩配置 |

## 8. 证据文件

| 文件 | 内容 |
|---|---|
| `docs/VALIDATION_METHOD.md` | 当前正式验证方法，只保留 SIFT10K / SIFT100K |
| `docs/WSL2_SIFT_VALIDATION_REPORT.md` | WSL2 SIFT10K / SIFT100K 验证与 near-20% 对照 |
| `docs/sift_experiment_report.md` | 当前保留的 SIFT 实验摘要 |
| `archive/results/wsl-sift10k-lsh-budget-20260605-130016.txt` | SIFT10K 达标结果 |
| `archive/results/wsl-sift100k-lsh-budget-20260605-130120.txt` | SIFT100K 达标结果 |
| `archive/results/wsl-sift10k-lsh-cache20-20260605-131427.txt` | SIFT10K near-20% 对照 |
| `archive/results/wsl-sift100k-lsh-cache20-20260605-131500.txt` | SIFT100K near-20% 对照 |
| `archive/results/wsl-sift10k-m2-pq-budget-20260605-125914.txt` | SIFT10K PQ/ADC 反例 |
