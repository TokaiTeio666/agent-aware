# AgentMem-Flow 验证方法

本文档定义当前正式验证口径。自 2026-06-09 起，正式验证只采用 `SIFT10K` 与 `SIFT100K`，不再使用本地 synthetic / local smoke 结果作为验证依据。

## 1. Recall@10 定义

对查询集合 `Q`，每个 query `q` 有：

- `GT_10(q)`：ground truth Top-10 id 集合
- `R_10(q)`：系统返回的 Top-10 id 集合

```text
recall@10(q) = |R_10(q) ∩ GT_10(q)| / 10
Recall@10 = average(recall@10(q) for q in Q)
1-Recall@10 = 1 - Recall@10
```

## 2. 数据集与 Ground Truth

当前正式验证范围：

| 数据集 | Base Count | Query Count | Ground Truth |
|---|---:|---:|---|
| SIFT10K | 10,000 | 100 | subset exact brute force fallback |
| SIFT100K | 100,000 | 100 | subset exact brute force fallback |

规则如下：

- 输入数据必须来自 `data/sift/` 下的真实 SIFT 文件。
- `SIFT1M` 官方 `.ivecs` 仅适用于 full-base；对子集运行时必须检查 truth id 是否越界。
- 一旦 `sift_groundtruth.ivecs` 中存在超出当前 `base_limit` 的 id，必须切换到当前子集的 exact brute force truth，并在结果中记录 `truth=file_out_of_range_exact_bruteforce`。
- 当前正式报告必须写明 truth 来源，不允许只写“用默认 truth”。

## 3. 受限内存验证

正式验证必须记录 resident engine memory，而不是机器总内存。默认预算口径为：

```text
memory_budget_ratio <= 0.20
memory_budget_bytes = raw_vector_bytes * memory_budget_ratio
```

必须归档以下字段：

- `memory_budget_ratio`
- `memory_budget_bytes`
- `memory_resident_bytes`
- `memory_resident_ratio`
- `memory_budget_pass`
- `memory_accounting_scope`
- `memory_bytes_*`

通过标准：

- `memory_budget_pass=1`
- `memory_resident_ratio <= 0.20`
- exact / 全量内存模式不能作为受限内存达标证据
- 如果启用 `--allow-over-budget-for-debug`，结果只能作为调试反例，不能算通过

## 4. 运行环境口径

当前正式验证允许使用 WSL2，但必须在报告中明确：

- 运行路径是否位于 `/mnt/d` 之类的 Windows 挂载路径
- 当前 `io_mode_requested` 与 `io_mode_effective`
- 是否仍然落回 `pread`

解释规则：

- WSL2 `/mnt/d` 结果可用于验证功能、召回和内存预算。
- WSL2 `/mnt/d` 结果不能作为最终 ext4 / NVMe 性能结论。
- 只有在原生 Linux 或 WSL ext4 上完成复跑后，P99 / QPS 才能作为更强的 I/O 结论。

## 5. 当前通过标准

当前主线只认以下通过条件：

| 项目 | 通过标准 |
|---|---|
| SIFT10K | `memory_budget_pass=1` 且 `Recall@10` 保持高召回，结果可复现 |
| SIFT100K | `memory_budget_pass=1` 且 `Recall@10 >= 0.95` |
| 近 20% 对照 | 只用于说明预算分配影响，不替代主线达标配置 |
| PQ/ADC 变体 | 只要 Recall 不达标，就归类为反例或调参对象 |

## 6. 归档要求

正式验证至少保留以下文件：

```text
docs/WSL2_SIFT_VALIDATION_REPORT.md
docs/sift_experiment_report.md
archive/results/wsl-sift10k-*.txt
archive/results/wsl-sift100k-*.txt
archive/logs/wsl-sift10k-*.log
archive/logs/wsl-sift100k-*.log
```

归档内容必须包含：

- git commit hash
- 构建时间与编译器版本
- 运行命令
- 数据集路径
- truth 来源
- `io_mode_requested`
- `io_mode_effective`
- memory budget / resident memory 字段
- `Recall@10`、QPS、P95/P99 latency、`ssd_reads_per_query`

## 7. 当前不再纳入正式验证的内容

以下内容不再作为正式验证证据：

- 本地 synthetic clustered workload
- Windows local smoke
- `run_type=smoke` 的本地功能回归结果
- 基于 synthetic 的 PQ/ADC、mixed workload 结论

这些内容如果保留，也只能作为开发期调试或历史材料，不能出现在当前正式验证结论中。
