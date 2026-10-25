# 复现结果目录

本目录保存 `docs/experiments/` 中被引用的原始 JSON。每个子目录是一次完整矩阵运行，
`manifest.json` 记录矩阵参数、索引/Binary 的 SHA-256 与数据集配方。所有数据由
`scripts/run_repro_experiments.cpp`（无 Python 主机）或同口径的 `.py` 版本生成，
表格由 `scripts/summarize_repro_results.cpp` / `.py` 汇总（中位数，括号内为最小-最大）。

## 数据集：合成、带配方

SIFT1M 的官方下载站（`corpus-texmex.irisa.fr`）不可达，本机也没有 cmake，因此本轮所有结果
都使用可提交的合成数据集（`scripts/make_repro_dataset.cpp`，recipe `gmm-v1`）：

| 项 | 取值 |
| --- | --- |
| base | 100,000 × 128，随机种子 `20261025` |
| query / ground truth | 2,000 × 128，精确 L2 Top-10 |
| 生成方式 | 1,000 个高斯簇中心 U(-1,1)^128，base 噪声 σ=0.15，query 噪声 σ=0.10（splitmix64 + mt19937_64） |
| 确定性 | 同一配方在任意机器上生成字节级相同的 fvecs |

合成数据的结论**不能**与 `docs/archive/2026-10-legacy/` 中的 SIFT1M 数字横向比较。

## 目录

| 目录 | 内容 | 状态 |
| --- | --- | --- |
| `2026-10-25-windows-msvc-v2/` | 文档引用口径：搜索 4 policy × 4 width × 3 轮，混合读写 4 配置 × 3 轮，干净关闭/重开检查 | 现行 |
| `2026-10-25-windows-msvc-v1-bench-bug/` | 只有说明，指向 `2026-10-25-windows-msvc/`（修复前全矩阵的实际位置） | 占位，无 JSON |
| `2026-10-25-windows-msvc/` | 早期一次运行（调度器修复前）及其 `stability/` 修复后对照 | 历史留档 |

### 并发调度器缺陷（v1 → v2）

原调度器在 `std::atomic<size_t> claimed_ops` 上用 CAS + `yield()` 自旋领取"读/写槽位"，
并且在 duration 模式下，等待中的 reader 会与上一个写操作并发执行、下一个写操作又会与上一个
读操作并发执行。于是：

- 14 代混合核上，竞争线程长期占住 P 核，read QPS 随运行时长从 ~2500 掉到 ~1400；
- 写侧被压到约 63 次/秒（80/20 序列下设计值为 128 次/秒），delta 规模只有应达到的一半，
  "delta 越大越慢"的结论因此被严重低估。

修复后改为"上一个槽位完成后才领取下一个"的确认式调度（`confirmed_ops`）：时间窗内执行的
操作数严格满足配置比例，代价是读、写严格串行（read QPS = 写路径吞吐 × 读写比）。修复前后对照见 `2026-10-25-windows-msvc/stability/`（修复后）与
`2026-10-25-windows-msvc/stability-v1-bench-bug/`（修复前），结论写在
[`docs/experiments/mixed-read-write.md`](../docs/experiments/mixed-read-write.md)。

> 修复后 80/20 的 write QPS 仍约 63/s：`execute_write` 在 commit 线程上对每条提交同步
> `fdatasync`，且写侧只有单线程 `commit_loop`。这是当前写路径的吞吐上限，不是调度问题。

## 复现

完整步骤（生成数据集 → 建索引 → 跑矩阵 → 汇总）见 [`scripts/README.md`](../scripts/README.md)。
本轮运行使用未提交的工作树（基线 `b9f3e46` + 本轮修复），结果 JSON 中 `git_commit` /
`dirty_status` 为 `unknown`；可追溯性以各次 `manifest.json` 的二进制/索引 SHA-256 与数据配方为准。

### 产出 `2026-10-25-windows-msvc-v2` 的实际二进制

本轮换过几次二进制，为免混淆，产出上述 JSON 的四个可执行文件留在 `build/binaries/2026-10-25/`
（`build/` 不进版本库，如需长期保留请自行转存）。其中 `bench_mixed_rw` 是修复调度器后的版本，
其 SHA-256 不在 `v2/manifest.json` 里（那份 manifest 的 mixed 部分记录的是早期二进制），以本表为准：

| 文件 | SHA-256 | 用途 |
| --- | --- | --- |
| `agent-aware-search.exe` | `8a73cd16…722c97` | 搜索矩阵（= `v5`，含预算字段修正） |
| `bench_mixed_rw.exe` | `9fa98ee4…117721a0` | 混合读写 3 轮矩阵与恢复检查（= `v3`） |
| `bench_mixed_rw`（修复前） | `b1e0dc3f…a5e413` | 仅用于 `2026-10-25-windows-msvc/` 与 `stability-v1-bench-bug/` |
| `run_repro_experiments.exe` | `6c5b0a1e…b2ed41` | 实验驱动 |

搜索矩阵的索引 SHA-256 为 `6d0084b7…d57957`（100k×128 `gmm-v1` 索引，见 `v2/manifest.json`）。
