# 历史留档：调度器修复前后的稳定性对照数据

本目录的现行口径是 `../2026-10-25-windows-msvc-v2/`；这里保留**并发调度器修复前**的一次完整
矩阵，以及修复后补充的稳定性对照，用于支撑
[`docs/experiments/mixed-read-write.md`](../../docs/experiments/mixed-read-write.md) §3.3。

| 子目录 / 文件 | 内容 |
| --- | --- |
| `stability-v1-bench-bug/` | 修复前补充测量：纯读（`none/lru/2q/graph-aware-2q`）5 s vs 30 s；80/20 + compaction 5 s vs 30 s。"修复前 read QPS 随时长下降、且在一轮之内从 356 掉到 252"的直接证据来自早期单轮日志，未作为 JSON 入库，此处只保留可核对的 5 s/30 s 对照 |
| `stability/` | 修复后：纯读 5 轮 10 s（2379–2519）与 30 s 单轮（2384）；80/20 + compaction 5 轮 10 s（2136–2222）；80/20 **无 compaction** 3 轮 30 s（1838–1868） |
| `search-*`、`mixed/`、`failure/`、`manifest.json` | 修复前调度器下的一次完整矩阵（仅留作存档）；`../2026-10-25-windows-msvc-v1-bench-bug/` 只是指向这里的占位说明 |

**文件名中的时长不完全可信，以 JSON 内的 `duration_sec` 为准**：`stability/` 里的
`readonly-ga-30-r*` 与 `mixed-80-20-30-r*` 实际都是 10 s（早期脚本参数残留）；真正的 30 s
数据是 `readonly-w0.json`（纯读）与 `mixed-80-20-noncomp-30-r*`（80/20、无 compaction）。
`stability/` 由修复后的二进制产出（含 `recovery` / `committed_write_records` 字段），
只用于"不随时长下滑"的稳定性论证，不作为本目录的口径数据。
现行数据见 [`../2026-10-25-windows-msvc-v2/`](../2026-10-25-windows-msvc-v2/)。
