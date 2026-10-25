# 复现口径与实验环境

> 日期：2026-10-25
> 代码基线：`main` @ `b9f3e46` + 本轮未提交修复（见 `docs/problem/current-issues.md` 的处理状态）
> 原始结果：[`results/2026-10-25-windows-msvc-v2/`](../../results/2026-10-25-windows-msvc-v2/)

本轮的目标是把"文档里的数字"换成"任何人按仓库内脚本可以重跑、并且原始 JSON 入库的数字"。
做法：固定数据集配方和随机种子、固定矩阵与轮数、所有原始 JSON 与 manifest 入库、每张表给出
中位数与最小-最大范围。

## 1. 环境

| 项 | 取值 |
| --- | --- |
| 机器 | Windows 11 Pro（10.0.26300），Intel Core i7-14700HX（混合架构），32 GB 内存 |
| 数据盘 | D:，NTFS，YMTC YMSS2ED08D25MC（NVMe） |
| 编译器 | MSVC 14.42.34433，`/O2 /MD /std:c++17 /EHsc /utf-8 /W3` |
| 构建开关 | `AGENT_AWARE_HAS_LIBURING=0`、`AGENT_AWARE_ENABLE_DIRECT_IO=0` |
| I/O 实际路径 | `pread`（Windows 构建无 io_uring / O_DIRECT；所有 JSON 的 `io_effective_mode` 均为 `pread`） |
| 并发 | 搜索单线程；混合读写 4 reader + 1 writer |

本机没有 cmake，构建使用一次性命令行（`cl` + `link`），二进制以 SHA-256 记录在
`results/2026-10-25-windows-msvc-v2/manifest.json`，构建方法见 `scripts/README.md`。

> **该 MSVC 安装在本轮结束后已被移除**（2026-10-25 复查时 VS 2022 目录为空），本机剩余的唯一
> C++ 编译器是 MinGW g++ 8.1，其 `<filesystem>` 无法编译本工程。因此本目录中的数字**当前无法
> 在本机直接复跑**，需要重新安装 MSVC 或在 Linux（GCC ≥ 9 / Clang ≥ 10）上重跑。

> **Windows 构建不等于 Linux 构建。** 本轮修复中与持久化相关的部分
> （SSTable/manifest 的 `fsync`、目录同步、rename 覆盖语义）在 Linux 上用 `fsync`/`fsync(dir)`
> 实现，在 Windows 上用 `FlushFileBuffers`/`_commit` 实现，目录同步在 Windows 上为空操作。
> 断电容错的验证只在 Windows 语义下完成；Linux 侧的崩溃恢复需要单独验证。

## 2. 数据集

SIFT1M 的官方下载站不可达，本轮使用仓库内脚本生成的合成数据（`gmm-v1` 配方）：

| 项 | 取值 |
| --- | --- |
| base / query / dim | 100,000 / 2,000 / 128 |
| ground truth | 精确 L2 Top-10 |
| 生成 | 1,000 个簇中心 U(-1,1)^128；base = 中心 + N(0, 0.15²)，query = 中心 + N(0, 0.10²) |
| 种子 | `20261025`；splitmix64 派生 mt19937_64，base/query 两个独立流 |
| 索引 | graph degree 64，page 4096，PQ 32 subspaces × 256 centroids，single-medoid 入口 |

查询分布比 SIFT1M 更"簇化"（query 与 base 共享簇中心），所以**Recall 在不同宽度上的绝对值和
梯度与 SIFT1M 不同**，只能作为相对比较和工程验证使用，不能替代 SIFT1M 主表。

## 3. 矩阵

| 矩阵 | 配置 | 轮数 | 单轮时长 |
| --- | --- | ---: | --- |
| 搜索 | cache policy `none / lru / 2q / graph-aware-2q` × search width `96 / 128 / 156 / 192` | 3 | 2,000 条 query，单线程 |
| 混合读写 | `read-only`、`95/5`（compaction 关/开）、`80/20`（compaction 开） | 3 | 30 s，4 reader + 1 writer，CRUD 70/25/5 |
| 恢复检查 | 写 95/5 → 干净关闭 → 只读重开 | 1 | 30 s |

所有配置固定：`beam_width=16`、`top_k=10`、`memory_budget_ratio=0.20`、`recall_sample_rate=0`
（混合读写的 Recall 用独立场景验证，不混入吞吐轮次）、每轮使用全新的动态目录。
搜索矩阵额外固定 `memory-budget-enforce 0`：该数据集上实测基线 RSS 已超过 20% 预算，
强制执行只会把缓存页截到 0，预算是否满足按实测 RSS 单独报告（见 [`memory-budget.md`](memory-budget.md)）。

## 4. 字段口径

| 字段 | 定义 | 注意 |
| --- | --- | --- |
| `reads_per_query`（搜索） | `submitted_reads / query_count` | 提交的页读请求数，含被去重/命中的请求 |
| `graph_reads_per_read`（混合） | `submitted_reads / read_ops` | 同上，按读操作归一 |
| `cache_hit_rate` | `cache_hits / page_requests` | `none` 策略恒为 0 |
| `latency_p95/p99/p999` | 单次读/查询墙钟 | 单线程搜索下 P999 即最慢约 2 条查询 |
| `measured_peak_rss_bytes` | Linux `VmHWM`；Windows `PeakWorkingSetSize` | 物理内存驻留峰值；Windows 的工作集包含共享页，**不是**私有提交内存 |
| `measured_baseline_rss_bytes` | 预热后、正式计时前的 RSS | 用于说明预算是否可能满足 |
| `memory_budget_pass` / `rss_within_budget` | 估算值/实测基线是否在预算内 | 见 `memory-budget.md` |
| `recovery.*` | 重新打开同一动态目录时的 SSTable/WAL 重放统计 | `committed_write_records` 是运行期间已提交的写记录数，可与 `visible_delta_records` 对照 |
| `recall_at_10`（混合） | 抽样 exact 回算 | 未采样时为 `null`，不是 0，也不是 1 |

## 5. 已知限制

1. **真实 RSS 超出 20% 预算。** 100k × 128、20% 预算约 10 MB，而仅 PQ code（3.2 MB）+ codebook
   （4.2 MB）+ 进程基线（约 13 MB）就已超过；搜索结果中 `rss_within_budget=false`。见
   [`memory-budget.md`](memory-budget.md)。
2. **混合读写 write QPS 为 63/s**，低于设计序列的 128/s（80/20）：写路径单线程提交且每次 `fdatasync`。
   这是当前实现的写吞吐上限，不是调度问题。
3. **搜索结果对缓存策略不敏感**：数据集体积 51.2 MB，而 4 GB 文件缓存装得下整个索引，
   四种策略的命中率、延迟差异都在轮次噪声量级内；详见 [`cache-policy.md`](cache-policy.md)。
4. **Windows 峰值工作集**包含 mmap/共享页，绝对值偏大；跨平台只能比较同进程内的相对变化。
5. 结果为**未提交工作树**上的一次运行；`git_commit`/`dirty_status` 字段为 `unknown`。
   可追溯性以 `manifest.json` 中的二进制/索引 SHA-256 与数据配方为准。
