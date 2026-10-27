# 实验记录

所有数字均来自 [`results/2026-10-25-windows-msvc-v2/`](../../results/2026-10-25-windows-msvc-v2/)
中的原始 JSON，用 `scripts/summarize_repro_results.cpp` 汇总（每个单元格为 3 轮中位数，
括号内为最小-最大）。旧实验记录已移入 [`docs/legacy/archive/2026-10-legacy/`](../legacy/archive/2026-10-legacy/)。

| 文档 | 内容 | 一句话结论 |
| --- | --- | --- |
| [`reproducibility.md`](reproducibility.md) | 环境、数据集配方、矩阵、字段口径、已知限制 | 合成数据集 100k×128、3 轮、原始 JSON 入库 |
| [`search-width-tuning.md`](search-width-tuning.md) | search width × cache policy 的 Recall/延迟/I/O 对照 | `128` 是 Recall-I/O 平衡点；`156/192` 只买 Recall |
| [`cache-policy.md`](cache-policy.md) | `none / lru / 2q / graph-aware-2q` 对照 | 数据集体积小于系统文件缓存，缓存策略无可见收益 |
| [`mixed-read-write.md`](mixed-read-write.md) | 纯读 / 95-5 / 80-20 × compaction 开关、恢复检查 | 写吞吐受单线程 commit 限制；读侧对 delta 增长敏感 |
| [`memory-budget.md`](memory-budget.md) | 模型估算 vs 真实 RSS、预算是否满足 | 20% 预算在当前数据规模下**不满足**，如实报告 |

## 阅读顺序建议

1. 先看 `reproducibility.md` 的"已知限制"，了解哪些结论不能用、为什么；
2. 答辩/评审需要的单点配置看 `search-width-tuning.md` 的"推荐配置"；
3. 创新点相关的对照（缓存策略、delta 增长、恢复）分别看后三篇。

## 与旧记录的差异（摘要）

| 维度 | 旧记录 | 新记录 |
| --- | --- | --- |
| 数据集 | SIFT1M，下载站已不可达 | 仓库内脚本生成的 `gmm-v1` 合成集，字节级可复现 |
| 代码 | dirty worktree，commit `91406a8`，结果不再可产出 | 基线 `b9f3e46` + 本轮修复（记录在 `docs/problem/`） |
| 轮数 | 多为一轮 | 每个配置 3 轮，给范围 |
| 内存 | 公式估算 `0.1985` | 估算 + 实测 RSS，并明确报告超出预算 |
| 混合读写 | 无调度器问题记录 | 发现并修复并发调度器缺陷后重跑；修复前后的差异单独记录 |
