# Search width 与缓存策略调优（可复现结果）

> 数据：[`results/2026-10-25-windows-msvc-v2/`](../../results/2026-10-25-windows-msvc-v2/)（48 个搜索 JSON，3 轮）
> 汇总：`scripts/summarize_repro_results.cpp`（3 轮中位数，括号内为最小-最大）
> 环境与口径：[`reproducibility.md`](reproducibility.md)

## 1. 矩阵

| 项 | 取值 |
| --- | --- |
| 数据集 | 合成 `gmm-v1`，base 100,000 × 128，query 2,000，精确 L2 Top-10 |
| 索引 | V3 packed graph，degree 64，page 4096（5 节点/页），PQ 32×256，single-medoid 入口 |
| 搜索 | 单线程，`top_k=10`，`beam_width=16`，`io-mode pread`，`memory-budget-ratio 0.20`（缓存自动 380 页，`none` 为 0），`memory-budget-enforce 0` |
| 变量 | cache policy `none / lru / 2q / graph-aware-2q` × search width `96 / 128 / 156 / 192` |
| 轮数 | 每格 3 轮，同一二进制、同一索引，每轮启动新进程 |

原始 JSON 里每个文件都带完整命令行（`run_id`、`cache_policy`、`effective_search_width` 等），
可逐字段核对。索引与二进制 SHA-256 见 `results/2026-10-25-windows-msvc-v2/manifest.json`。

## 2. 结果

### 2.1 按 search width（graph-aware-2q，其余策略见 §2.2）

| search width | Recall@10 | avg ms | P95 ms | P99 ms | P999 ms | QPS | reads/query | cache hit | peak RSS MB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `96` | 0.6082 | 0.9408 (0.9027-0.9711) | 1.4327 | 1.6902 | 2.2220 | 1060.9 | 50.05 | 0.2469 | 17.55 |
| `128` | 0.7968 | 1.0351 (1.0192-1.0735) | 1.5526 | 1.8374 | 2.2168 | 964.5 | 56.52 | 0.2047 | 17.57 |
| **`156`** | **0.8767** | **1.1364 (1.1334-1.1421)** | **1.7151** | **2.0016** | **2.3734** | **878.5** | **61.62** | 0.1767 | 17.61 |
| `192` | 0.9160 | 1.3795 (1.2795-1.4388) | 1.9623 | 2.4359 | 3.1080 | 723.9 | 72.78 | 0.1497 | 17.68 |

Recall 只取决于宽度（与缓存策略无关）。逐段折算：

| 区间 | Recall 增益 | avg 延迟变化 | reads/query 变化 |
| --- | ---: | ---: | ---: |
| `96 → 128` | +18.9 pt | +10.0% | +12.9% |
| `128 → 156` | +8.0 pt | +9.8% | +9.0% |
| `156 → 192` | +3.9 pt | +21.4% | +18.1% |

宽度越大，每多 1 个百分点 Recall 付出的 I/O 越多：`96→128` 每点 Recall 多读约 0.38 页，
`128→156` 约 0.64 页，`156→192` 约 2.9 页。

**推荐配置**：`search_width=156`（Recall ≥ 0.87、P999 < 2.4 ms、reads/query 约 62）。
若应用对 Recall 更敏感，`192`；若对尾延迟更敏感，`128`（Recall 0.80）。`96` 不推荐（Recall 0.61）。

### 2.2 按缓存策略（同宽度、同 Recall）

| width | none | lru | 2q | graph-aware-2q |
| ---: | ---: | ---: | ---: | ---: |
| 96 | 0.9252 / 84.80 | 0.8950 / 59.05 | 0.8082 / 50.16 | 0.9408 / 50.05 |
| 128 | 0.9986 / 90.86 | 0.9988 / 65.68 | 0.8703 / 56.61 | 1.0351 / 56.52 |
| 156 | 1.1139 / 95.77 | 1.0870 / 70.97 | 0.9844 / 61.68 | 1.1364 / 61.62 |
| 192 | 1.2705 / 106.94 | 1.2802 / 82.65 | 1.2076 / 72.82 | 1.3795 / 72.78 |

（单元格：avg ms / reads per query。）进程内缓存把 reads/query 相对 `none` 降低 32–41%
（`96` 41.0%，`128` 37.8%，`156` 35.7%，`192` 31.9%）；四个宽度上 graph-aware-2q 都是最慢的，
`2q` 最快或接近最快。详细分析与限制见 [`cache-policy.md`](cache-policy.md)。

## 3. 与旧 SIFT1M 结果的关系

旧记录（SIFT1M，`156` → Recall@10 0.9748、avg 48.10 ms、io_uring）已移入
[`docs/legacy/archive/2026-10-legacy/`](../legacy/archive/2026-10-legacy/README.md)，原因见该目录 README。
这里的两组数字**不可横向比较**：

| 差异 | 旧 | 新 |
| --- | --- | --- |
| 数据 | SIFT1M（真实分布） | 合成高斯簇（query 更贴近 cluster 中心） |
| I/O | io_uring + O_DIRECT | pread（Windows 构建） |
| 页布局 | 未记录每页节点数 | 5 节点/页，62 页/query（旧记录为 109.8 reads/query） |
| 延迟 | 48 ms/query | 1.14 ms/query |

Recall 的绝对值与宽度梯度由数据集决定，旧记录在 SIFT1M 上"`128` 与 `156` 延迟持平"的拐点在新数据上
没有复现；新数据是单调的"宽度↑ → Recall↑、延迟↑、I/O↑"。结论层面一致的是：`156` 附近是 Recall 与
I/O 的合理折中，`192` 的边际收益下降。

## 4. 复现

```bat
run_repro_experiments.exe --bin-dir build\bin --index build\i100.idx ^
  --dataset build\repro_gmm --out results\<run> --scratch build\repro_scratch ^
  --query-limit 2000 --mixed-duration 30
```

只跑搜索矩阵时 `--mixed-duration 0`；完整步骤见 [`scripts/README.md`](../../scripts/README.md)。

## 5. 结论边界

- 单线程、`pread`、数据集可完全驻留系统文件缓存；并发与真实 NVMe 直接 I/O 下的宽度-延迟关系会变化。
- 合成数据的绝对 Recall 不可与 SIFT1M 对比；宽度之间的**相对**梯度（越大越贵）可迁移。
- 每个格 3 轮，P999 只有约 2 条样本的语义，仅作趋势参考。
