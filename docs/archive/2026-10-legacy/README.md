# 2026-10 旧实验记录（废弃）

本目录保存 2026-10-25 之前 `docs/experiments/` 下的两份实验报告，以及生成这些数字时的
源码快照（`legacy-source/`）。它们**不代表当前代码的可复现结果**，新的实验记录见
[`docs/experiments/`](../../experiments/)。

## 为什么废弃

| 问题 | 证据 |
| --- | --- |
| 产出数字的代码已不在 `main` 上 | 报告记录 binary commit `91406a8`、dirty worktree；`8670a61` 之后 `src/`、`include/` 共 51 个文件、约 2.1 万行增删，两个 benchmark 程序几乎整体重写，`DynamicReadView` 被删除 |
| 原始 JSON 没有入库 | README 引用的 `build/search_width_*_baseline.json` 与混合读写 JSON 只在作者本地 `build/`，`build/` 被 `.gitignore` 忽略 |
| 单轮样本支撑"结论" | `128` 首跑 Avg 79.72 ms、复跑 49.86 ms；`156`/`192` 各只有一轮，README 却据此得出"`156` 平均延迟低 3.5%" |
| 预算数字是模型值 | `estimated_resident_ratio=0.198505` 是公式估算，不是进程 RSS；全仓库当时没有读取 `/proc` 或 `GetProcessMemoryInfo` |
| 缓存策略结论未验证 | `docs/design/02-cache-zero-copy.md` 的 LRU / 2Q / Graph-Aware 2Q 对照表为空；历史上唯一一组对照（`archive/results/p3-cache-*`，synthetic 3000 点）显示 graph-aware 命中率与 QPS 都低于 LRU |
| 环境不可复现 | WSL2 开发环境、`dirty` 工作树，且查询窗口/缓存页数与新口径不同 |

## 旧数字（仅留档，勿引用）

| 实验 | 旧口径 | 旧结论 |
| --- | --- | --- |
| Search width | SIFT1M、查询 5000-5999、cache 15000 页、io_uring | `156` Recall@10 0.9748、Avg 48.10 ms；`192` Avg 99.12 ms |
| 混合读写 | SIFT1M、4 reader + 1 writer、30 s × 3 轮 | 纯读 703 read QPS；95/5 647；80/20 578 read / 144.6 write QPS |

## 新口径的位置

| 旧报告 | 新报告 | 数据来源 |
| --- | --- | --- |
| `experiments-search-width-tuning.md` | [`docs/experiments/search-width-tuning.md`](../../experiments/search-width-tuning.md) | `results/2026-10-25-windows-msvc-v2/` |
| `experiments-mixed-read-write.md` | [`docs/experiments/mixed-read-write.md`](../../experiments/mixed-read-write.md) | `results/2026-10-25-windows-msvc-v2/`（并发调度器修复后） |
| —（旧报告没有缓存策略对照） | [`docs/experiments/cache-policy.md`](../../experiments/cache-policy.md) | `results/2026-10-25-windows-msvc-v2/` |

旧报告中的数据与结论在评审材料中不应再出现数值，只能作为"历史待验证点"引用。

## legacy-source

`legacy-source/` 是 `8670a61`（本轮修复前的最后一个提交）中 `src/`、`include/`、`tests/`
与根目录 `CMakeLists.txt` 的逐字节快照，用于对照本轮修复（`docs/problem/current-issues.md`
的逐条问题与修复位置）。同样的内容也保留在 `backup/pre-fix-2026-10-25` 分支上，此处留档是
为了不依赖分支存在。它不是独立工程（只含源码与构建脚本，不含文档），也不随主版本维护。
