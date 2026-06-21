# agent-aware 文档中心

本目录用于汇总 `agent-aware` 的赛题要求、系统设计、阶段计划、实验报告和后续路线。文档按评审阅读路径和工程主题分层组织，目标是让评委在短时间内看到完整闭环：赛题约束 -> 技术路线 -> 代码实现 -> 实验结果 -> 评分证据。

## 目录结构

| 目录 | 用途 |
| --- | --- |
| `competition/` | 赛题要求、评分映射、答辩清单 |
| `design/` | SSD 存储、缓存、异步预取、动态写入等系统设计 |
| `experiments/` | SIFT1M、混合读写、参数调优等实验报告 |
| `roadmap/` | 后续优化计划、任务拆分和 90 分冲刺路线 |
| `changelog.md` | 当前版本能力边界、关键改动、验证命令和已知限制 |

## 评审入口

| 阅读顺序 | 文档 | 服务评分项 | 用途 |
| ---: | --- | --- | --- |
| 1 | `docs/competition/problem.md` | 性能、创新、文档 | 赛题原文、内存 10%-20%、Recall@10 >= 85%、四项 25% 权重 |
| 2 | `README.md` | 文档完整性 | 项目第一入口、当前版本结果、构建运行命令 |
| 3 | `PROJECT_PLAN.md` | 创新、代码质量、文档 | 项目目标、架构分层、核心模块、测试验收 |
| 4 | `docs/competition/scoring-and-defense.md` | 四项评分 | 90 分目标拆解、证据矩阵、答辩顺序和材料清单 |
| 5 | `docs/experiments/sift1m-mixed-rw-immutable-view.md` | 性能指标 | SIFT1M 主结果、混合读写、immutable read view 优化前后对比 |
| 6 | `docs/changelog.md` | 代码质量、文档 | 当前版本能力边界、关键改动、验证命令、已知限制 |

## 设计与实现文档

| 编号 | 文档 | 当前状态 | 说明 |
| --- | --- | --- | --- |
| 10 | `docs/design/ssd-storage-path.md` | 已落地 | 4KB record、DiskIndexReader/Writer、packed graph 文件和基础测试 |
| 11 | `docs/design/cache-zero-copy.md` | 已接入主路径 | Graph-Aware 2Q、同页复用、direct distance、缓存统计 |
| 12 | `docs/design/async-prefetch.md` | 已接入主路径 | `AsyncPageReader`、`QueryPageSession`、io_uring fallback、frontier/next-hop prefetch |
| 13 | `docs/design/beam-width-io-uring.md` | 已进入 benchmark | beam batch、批量读取、I/O 统计字段和 fallback 语义 |
| 20 | `docs/design/dynamic-write.md` | 已落地最小闭环 | WAL/MemTable/SSTable/manifest/compaction/recovery 与 mixed RW benchmark |
| 21 | `docs/roadmap/dynamic-write-task-breakdown.md` | T1-T5 已有实现 | 动态写入拆分任务和验收清单，异步 flush queue 仍可推进 |
| 22 | `docs/experiments/high-concurrency-mixed-rw.md` | 已完成可展示版 | 高并发 mixed RW、动态 Recall、base/delta merge、一致性策略 |
| 23 | `docs/roadmap/next-sift1m-mixed-rw-optimization.md` | 下一阶段计划 | SIFT1M mixed RW 后续优化项、对照实验和验收标准 |
| 30 | `docs/experiments/param-tuning-and-sift-scale-test.md` | 已完成关键 evidence | 参数调优、SIFT 规模化测试矩阵和可复现实验计划 |
| 31 | `docs/design/fresh-streaming-ann.md` | 创新路线归档 | LSM delta + immutable read view 已完成，Delta memory graph 和周期性 rebuild 为后续路线 |

## 当前可展示结果

| 场景 | 结果摘要 | 结果文件 |
| --- | --- | --- |
| SIFT1M SSD 主路径 | Recall@10 `0.9940`，resident ratio `0.199992`，P95 `501 ms` | `logs/sift_bench/codex_sift1m_once_20260620-022610/result.json` |
| 纯读并发 baseline | 8 reader 下 `56.55 read_qps`，P95 `178 ms` | `build/sift1m_readonly_t8.json` |
| mixed no compaction | `23.50 read_qps`，`217.05 write_qps`，P95 `167 ms` | `build/sift1m_mixed_rw_no_recall_no_compaction_immutable_view.json` |
| mixed background compaction | `28.86 read_qps`，`221.33 write_qps`，P95 `178 ms` | `build/sift1m_mixed_rw_no_recall_compaction_immutable_view.json` |
| dynamic Recall evidence | Recall@10 `1.0`，`27` samples，`250.36 write_qps` | `build/sift1m_dynamic_recall_immutable_view.json` |

## 90 分冲刺优先级

| 优先级 | 目标 | 对应文档 |
| --- | --- | --- |
| P0 | 保持当前 immutable read view mixed RW baseline 可复现，答辩时先展示 SIFT1M 证据 | `docs/experiments/sift1m-mixed-rw-immutable-view.md` |
| P0 | 用评分矩阵组织答辩，避免只讲实现不讲得分点 | `docs/competition/scoring-and-defense.md` |
| P1 | 扩展参数矩阵，输出 Recall/QPS/延迟/I/O 的稳定曲线 | `docs/experiments/param-tuning-and-sift-scale-test.md` |
| P1 | 将 flush 拆成真正后台队列，降低写路径同步停顿 | `docs/experiments/high-concurrency-mixed-rw.md` |
| P2 | 实现 Delta memory graph，提高新写入向量检索质量 | `docs/design/fresh-streaming-ann.md` |
| P2 | 周期性 rebuild packed graph，将 delta 批量吸收到 base | `docs/roadmap/next-sift1m-mixed-rw-optimization.md` |
