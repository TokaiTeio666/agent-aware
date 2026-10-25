# 内存预算：模型估算 vs 实测 RSS

> 数据：[`results/2026-10-25-windows-msvc-v2/`](../../results/2026-10-25-windows-msvc-v2/) 搜索矩阵 JSON
> （以下数值以 `search-graph-aware-2q/sw156/round1.json` 为例）；口径见 [`reproducibility.md`](reproducibility.md)

## 1. 结论

在 100k×128（51.2 MB）数据集、20% 预算（10,240,000 B ≈ 9.77 MiB）下：

| 量 | 值 | 是否在预算内 |
| --- | ---: | --- |
| 预算（数据字节 × 0.20） | 10,240,000 B | — |
| 模型估算常驻（PQ codes 3.2 MB + codebook 4.2 MB + 目录 0.4 MB + 入度表 0.4 MB + 380 页 × 5,376 B） | 10,237,184 B（`0.199945`） | 是（估算） |
| **实测基线 RSS**（加载索引与 query 后、搜索前） | 13,717,504 B | **否** |
| **实测峰值 RSS** | 18,452,480 B（`graph-aware-2q`，其余策略 16.0–17.7 MB） | **否** |
| 实测 / 预算 | 约 1.34–1.80 倍 | — |

JSON 中因此同时出现：

```text
"estimated_resident_ratio": 0.199945,
"memory_budget_enforce": false,
"cache_pages_truncated": false,
"memory_budget_pass": true,
"rss_within_budget": false,
"warnings": ["measured baseline RSS (13717504 B) plus modelled search structures exceeds the dataset memory budget; ..."]
```

注意 `memory_budget_pass: true` 只说明"按模型估算，该缓存配置没有超出预算"，它与真实 RSS
不满足预算**并不矛盾**——模型漏掉的进程固定开销正是差距所在。

两个字段回答的是不同问题：`memory_budget_pass` 是"按模型配置，缓存页数是否被预算约束"，
`rss_within_budget` 是"进程实际是否在预算内"。**只有后者能对外宣称满足预算。**

## 2. 为什么差这么多

| 组成 | 大小 | 是否计入模型 |
| --- | ---: | --- |
| PQ codes（100k × 32 B） | 3.2 MB | 是 |
| PQ codebook（`PqAdcModel` 内存中按满维存：32 × 256 × 128 × 4 B = 4,194,304 B） | 4.0 MB | 是 |
| 节点 → 页目录（100k × 4 B） | 0.4 MB | 是 |
| 节点入度表（100k × 4 B，仅 graph-aware-2q / hot page 保护时） | 0.4 MB | 是 |
| 页缓存（380 页 × 5,376 B = 4 KiB 原始字节 + 5 节点 × 64 邻居 × 4 B） | 2.0 MB | 是 |
| 进程自身（运行时、查询集 2,000×128×4 B ≈ 1 MB、数据文件映射、堆碎片） | 约 3.5 MB | **否** |
| 解码/会话临时对象（每次查询把访问到的节点全精度向量复制进会话） | 2–3 MB | **否** |

- 预算分母是"原始向量字节"，分子却必须包含与数据规模无关的进程固定开销。数据集越小，
  这部分越难摊薄；100k×128 只有 51.2 MB，5.5 MB 的基线就是 10.7% 的预算。
- PQ codebook 占 4 MB 是硬成本：`PqAdcModel` 对每个 sub-centroid 都按**整行 128 维**存储
  （训练与 ADC 查表直接按 `(m × centroids + c) × dim + offset` 寻址），并且**这份满维布局被
  原样写进索引文件**（加载时会校验 `codebooks_.size() == subspaces × centroids × dim`），
  所以是 32 × 256 × 128 × 4 B，而不是通常假设的 32 × 256 × 4 × 4 B（后者只有 128 KB）。
  要压低它，只能减少 subspaces / centroids，或改成按 subspace 只存对应维度的块——后者是索引
  格式变更，需要同时改读写与 ADC 查表。
- Windows 的 `PeakWorkingSetSize` 还包含映射文件的共享页；换到 Linux `VmHWM` 会略低，
  但不会低到 9.77 MB。

## 3. 本轮代码改动（让报告如实）

1. `sift_search_benchmark` 在预热前读取 `process_current_resident_bytes()` 作为基线，
   写进 `measured_baseline_rss_bytes`，与模型估算相加后与预算比较，写 `rss_within_budget`
   并生成警告（原实现只比较模型值）。
2. `--cache-pages` 显式超预算时的截断改为真实生效，并在 `estimated_resident_ratio` 中按
   **实际页数**计算（原实现按请求页数计算，会报出一个不会发生的配置）。
3. 本轮矩阵以 `--memory-budget-enforce 0` 运行：在本数据集上按预算分配只剩 380 页，
   再强制执行没有意义；报告如实标注"预算不满足"，而不是把页数截到 0 后宣称达标。

## 4. 要真正满足 10%–20% 预算，需要

| 方向 | 代价 | 预估效果 |
| --- | --- | --- |
| 按预算硬截断缓存并在报告中标注"预算内" | 无缓存或极少缓存，延迟上升 | 仍不满足，因为基线本身超预算 |
| 减少 PQ codebook（16 subspaces × 256） | Recall 下降 | codebook 4 MB → 2 MB（需重建索引） |
| PQ codes 存磁盘、ADC 时按页解码 | 每次扩展多一次读 | codes 3.2 MB → 0，但 I/O 增加 |
| 更大数据集（1M×128 = 512 MB） | 构索引时间（约 1 小时+）、内存需求 | 固定开销占比降到约 1% |
| 进程基线瘦身（避免整文件映射、按需加载 query） | 工程改动 | 5.5 MB → 2–3 MB |

下一轮验收应先选"更大数据集"，因为它同时解决预算占比与缓存策略/预取可测性两个问题。
