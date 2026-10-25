# 历史留档说明：并发调度器修复前的数据在哪里

本目录**只保存说明，不含任何 JSON**。修复前调度器的完整矩阵（搜索 + 混合读写 + 恢复检查）
实际存放在 [`../2026-10-25-windows-msvc/`](../2026-10-25-windows-msvc/)，此处保留一个指向它的
占位说明，避免旧文档中指向 `-v1-bench-bug` 的链接失效。

| 项 | 说明 |
| --- | --- |
| 数据位置 | [`../2026-10-25-windows-msvc/`](../2026-10-25-windows-msvc/)（`search-*`、`mixed/`、`failure/`、`manifest.json`）与其中的 `stability-v1-bench-bug/` |
| 修复内容 | `src/mixed_rw_benchmark.cpp`：CAS + `yield()` 自旋领取槽位 → 上一个槽位完成后才领取下一个 |
| 现行口径 | [`../2026-10-25-windows-msvc-v2/`](../2026-10-25-windows-msvc-v2/) |
| 分析 | [`docs/experiments/mixed-read-write.md`](../../docs/experiments/mixed-read-write.md) §3.3 |

**本目录下的混合读写数字不可引用**：调度器缺陷会让 read QPS 随运行时长持续下降、写吞吐被压到
设计值的一半。搜索矩阵不受该缺陷影响，可作为搜索侧的参考。

这些 JSON 由 `b1e0dc3f7f62c7ff01488b084177f592f498eb2a85afb0e75e318bb3baa5e413`
版本的 `bench_mixed_rw` 产出；该二进制未保留，如需复跑请按 §3.3 记录的修复前实现重建。
