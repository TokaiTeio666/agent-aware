# 修复状态：`current-issues.md` 逐条对照（2026-10-25）

> 对照对象：[`current-issues.md`](current-issues.md)（审计时 HEAD `6d7b69d`，当前基线 `b9f3e46`）
> 修复代码：当前工作树（未提交）；旧文档与旧结果：[`docs/legacy/archive/2026-10-legacy/`](../legacy/archive/2026-10-legacy/)
> 新实验：[`docs/experiments/`](../experiments/)、[`results/`](../../results/)
> 同类问题归类与教训：[`retrospective-2026-10-25.md`](retrospective-2026-10-25.md)

状态定义：**已修复**（有代码 + 验证）、**部分修复**（核心问题仍在或只修了一半）、**未修复**（本轮未动）、
**已改文档**（实现不变，文档不再夸大）。

## 1. P0

| 编号 | 问题 | 状态 | 处理 |
| --- | --- | --- | --- |
| 1.1 | 结果不可复现、产物不是当前工具链生成 | **已修复** | 数据集生成/矩阵/汇总脚本入库；原始 JSON + manifest（二进制与索引 SHA-256）入库；`results/README.md` 说明每次运行；旧记录移入 `docs/archive` |
| 1.2 | 内存预算未强制、报告值不是 RSS | **部分修复** | 新增 `process_current_resident_bytes()`（Linux `VmHWM` / Windows PeakWorkingSet）与 `process_peak_resident_bytes()`；JSON 新增 `measured_baseline_rss_bytes`、`measured_peak_rss_bytes`、`rss_within_budget` 与警告；显式 `--cache-pages` 超预算按实际生效页数截断并计入估算；`estimated_resident_ratio` 改为按生效配置计算。**未解决**：20% 预算在本数据集上仍不满足（基线 13.7 MB > 9.77 MB），见 [`memory-budget.md`](../experiments/memory-budget.md) |
| 1.3 | read view 不是快照、版本历史无界增长 | **部分修复** | 恢复时按 node 去重、只保留最新可见版本；运行期版本按 `version_history_limit`（默认 4）裁剪；`snapshot()` 返回按序列过滤后的视图。**未解决**：仍不是发布式 immutable snapshot，每次查询仍逐 shard 过滤 |
| 1.4 | `update` 无更新语义 | **未修复（已改文档）** | README 不再把 update 描述为独立语义；接口合并留待下一轮 |
| 1.5 | io_uring 预取可致缓冲池耗尽崩溃 | **已修复** | 预取容量上限扣除 demand 批量；缓冲不足时丢弃本次预取并计数，不再抛异常；提交包 try/catch |
| 1.6 | 写侧单线程、每次写 O(delta) | **部分修复** | 实测写吞吐约 63 条/秒，确认瓶颈为单线程 commit + 同步 `fdatasync`；混合读写调度器缺陷已修复。**未解决**：写吞吐上限、写入时的全量 delta 扫描 |
| 1.7 | 混合读写默认把 O(N) 回算算进读延迟 | **已修复** | `--recall_sample_rate` 默认改为 0；未采样时 `recall_at_10` 输出 `null` 而不是伪造 1.0 |
| 1.8 | 锁等待指标失真 | **已改文档** | README 标注 `search_mutex_wait_*` 当前不可用、`page_read_wait_*` 为整批 submit→drain 时间；指标本身未重做 |

## 2. P1

| 编号 | 问题 | 状态 | 处理 |
| --- | --- | --- | --- |
| 2.1 | PQ 过滤统计与实现不符 | **已改文档** | README 不再把 PQ 描述为过滤器 |
| 2.2 | 没有提前终止 | **未修复** | `search_width` 仍是唯一预算旋钮 |
| 2.3 | 多线程路径每线程独立 index | **未修复（已改文档）** | README 与 PROJECT_PLAN 明确该前提 |
| 2.4 | Graph-Aware 2Q 无实验支撑 | **已测，结论为无收益** | 4 策略 × 4 宽度 3 轮；graph-aware 在四个宽度上都最慢，见 [`cache-policy.md`](../experiments/cache-policy.md) |
| 2.5a/b/c | SSTable/manifest 只 flush 不 fsync、rename 窗口 | **已修复** | SSTable data/index/meta 关闭后重开并 `fsync_file`；manifest 先 fsync tmp 再 rename，Windows 上删除重试并 fsync 目录（Linux） |
| 2.5d/e | WAL 坏记录静默丢弃、校验不全 | **已修复** | `WalReader::replay_with_report()` 上报截断位置/字节/原因，恢复 JSON 写 `wal_truncated_segments`；校验覆盖 node_id/sequence/dim/deleted |
| 2.5f | compaction 的 `wal_checkpoint` 继承旧值 | **部分修复** | flush 提交时 checkpoint 取 `max(旧值, 批次最大序列)`，已 flush 的 WAL 不会在重开时重复进入 MemTable；compaction 本身仍不推进 checkpoint，tombstone/旧版本继续留在 SSTable |
| 2.5g/h | flush 失败不重试、WAL 关闭模式无说明 | **部分修复** | flush 失败计数并保留 WAL；未做自动重试 |
| 2.5i | WAL 记录校验算法变更但记录版本号仍为 1 | **未修复（已知不兼容）** | 旧算法写入的 WAL 会被新校验判为 `payload_checksum_mismatch` 并计入截断段。仓库内无持久化兼容需求，未做双算法回退；如需兼容旧目录应先升版本号再分支校验 |
| 2.5j | `version_history_limit=4` 使长搜索可能漏读中间版本 | **未修复（已改文档）** | reader 在搜索开始时取 `read_sequence`，但搜索期间同一 id 被写 4 次以上会裁掉该序列，读到的版本可能比快照更新。当前只影响单次搜索耗时超过 4 次同 id 更新的场景；修复方向是保留"低于最旧活跃读序列"的版本 |
| 2.6 | delta 只有线性扫描 | **未修复** | 已确认读侧成本随 delta 增长，但受调度器限制本轮无法画曲线（见 mixed 文档 §3.2） |
| 2.7 | 写入的邻居字段无人读取 | **未修复（已改文档）** | README 标注该字段当前不生效 |
| 2.8 | bench 参数与文档不一致 | **已改文档** | README 更正 `pq_train_limit`/`pq_iterations` 不生效、`--dynamic_graph_degree` 语义、`sstable_count` 来源 |
| 2.9 | 索引格式校验偏弱 | **未修复** | — |
| 2.10 | 预取实现与设计文档不一致 | **部分修复** | 缓冲池与统计口径修了一部分；预测/自适应仍与文档不符，README 维持"默认关闭、未通过验收" |
| 2.11 | 测试与可观测性不足 | **部分修复** | JSON 增加真实 RSS、恢复报告、警告列表；崩溃注入/ASan/CI 仍未做 |
| 2.12 | `agent_aware.py ab` 口径不一致 | **未修复** | — |
| 2.13 | 无 Python/C API 绑定 | **未修复** | — |
| 2.14 | 多数据集与规模测试缺失 | **部分修复** | 新数据集为合成 100k×128（非 SIFT1M）；更大规模未测 |

## 3. P2

| 编号 | 问题 | 状态 | 处理 |
| --- | --- | --- | --- |
| 3.1/3.2/3.3 | 文档漂移、路线图堆积、口径矛盾 | **部分修复** | README / PROJECT_PLAN / design/04 与本轮结果对齐；路线图未清理 |
| 3.4 | 构建无警告/CI | **未修复** | Windows 无 cmake；编译命令与开关记录在 `scripts/README.md` |
| 3.5 | 仓库卫生（pptx、`.claude/settings.local.json`、大文件提交） | **未修复** | 需要用户决定是否移除跟踪 |
| 3.6 | 依赖与外部资源 | **部分修复** | 数据集改为仓库内生成，不再依赖外部下载站 |
| 3.7 | 性能数据呈现 | **部分修复** | 新增 P999、`bytes_read_per_query`；混合读写仍无真实 NVMe 数据 |

## 4. 仍然最重要的 5 件事（按评分影响）

1. **更大数据集 + 真实 Linux/io_uring 复测**：同时解决预算占比（§1.2）、缓存策略可测性（§2.4）、
   预取验收（§2.10）。100k×128 上这三点都无法得到有意义的结论。
2. **写路径吞吐**：组提交 / 异步刷盘，把 63 条/秒提升后再测 delta 增长曲线（§1.6、§2.6）。
3. **读写非阻塞调度**：当前严格按序的调度让读吞吐 = 写吞吐 × 比例（mixed 文档 §3.1）。
4. **崩溃一致性**：flush/WAL/manifest 中途 kill 的注入测试（§2.5）。
5. **透明集成**：C API 或 Python 绑定 + 最小示例（§2.13）。
