# 赛事评分映射与答辩清单

本文档面向 2026 年全国大学生计算机系统能力大赛操作系统设计赛 OS 功能挑战赛道，用于把 `agent-aware` 的实现、实验和文档组织成评审可快速理解的材料。目标不是承诺固定得分，而是按评分项准备一套有机会冲击 90 分的证据闭环。

## 1. 90 分目标拆解

赛题评审要点为四项，各占 25%。建议答辩材料按下面目标组织：

| 评分项 | 权重 | 目标得分 | 当前支撑 |
| --- | ---: | ---: | --- |
| 性能指标 | 25 | 22 | SIFT1M Recall@10 `0.9940`、resident ratio `0.199992`、纯读与混合读写 benchmark、读路径分段指标 |
| 创新性 | 25 | 23 | Graph-Aware 2Q、PQ ADC 与 SSD 读取协同、io_uring 拓扑预取、LSM delta + immutable read view |
| 代码质量 | 25 | 22 | C++17/CMake 分层、可选 liburing fallback、单元测试、JSON benchmark、动态层恢复与一致性测试 |
| 文档完整性 | 25 | 23 | README、项目计划、赛题说明、阶段计划、版本说明、SIFT1M 实验报告、评分矩阵 |
| 合计 | 100 | 90 | 已具备可展示闭环，后续通过参数矩阵和真实 NVMe 复测继续补强 |

## 2. 评分证据矩阵

| 评分项 | 评委关注点 | 现有证据 | 答辩时先展示 |
| --- | --- | --- | --- |
| 性能指标 | 内存 10%-20%、Recall@10 >= 85%、读写混合 QPS、尾延迟 | `docs/experiments/sift1m-mixed-rw-immutable-view.md`、`docs/changelog.md`、`build/*.json` | SIFT1M 主路径 Recall `0.9940`，resident ratio `0.199992`，mixed P95 `167-178 ms` |
| 创新性 | 是否针对图索引随机 I/O 和动态写入做了系统级优化 | `PROJECT_PLAN.md` 第 6、12 节，`docs/design/*`、`docs/roadmap/*` | “用户态图感知缓存 + 拓扑预取 + LSM delta + immutable read view”组合方案 |
| 代码质量 | 模块边界、可运行性、可测试性、并发安全、异常回退 | `CMakeLists.txt`、`include/agent_aware/*`、`src/agent_aware/*`、`tests/unit/*` | core/graph/storage/dynamic/engine 分层，liburing 缺失自动 fallback，动态层单测 |
| 文档完整性 | 是否能复现、是否说明限制、是否有结果和后续计划 | `README.md`、`docs/README.md`、`docs/changelog.md`、本文件 | 按 README -> 评分矩阵 -> 实验报告的顺序演示 |

## 3. 答辩讲述顺序

建议 8-10 分钟技术陈述按以下顺序展开：

1. 赛题约束：Agent memory 高维向量远超内存，图遍历随机读，实时写入造成随机写和尾延迟；评分看性能、创新、代码、文档。
2. 系统总架构：内存保存 PQ codes、图导航数据和受控 BufferPool，全精度向量放 SSD 4KB record。
3. 读优化：Graph-Aware 2Q 保护 hub 节点，PQ ADC 减少无效 SSD 读取，beam/io_uring/next-hop prefetch 隐藏随机 I/O。
4. 写优化：WAL + MemTable + SSTable + manifest + compaction 将随机写转为顺序追加，base/delta merge 保证新版本优先。
5. 关键优化：immutable read view 让 reader 不再抢 `DynamicWriteManager` 全局锁，`latest_record_lookup_p95_ms` 从秒级降到约 `0.07 ms`。
6. 实验结果：先展示 SIFT1M Recall 和内存约束，再展示 pure read t8 和 mixed compaction/no-compaction 结果。
7. 工程质量：CMake 一键构建、单元测试、io_uring fallback、JSON 输出、读路径 breakdown、recovery 测试。
8. 已知限制与路线：delta linear scan、异步 flush queue、Delta memory graph、周期性 rebuild packed graph。

## 4. 提交前验收清单

| 类别 | 必做项 | 推荐命令或材料 |
| --- | --- | --- |
| 构建 | Release 构建成功 | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)` |
| 单元测试 | 动态写入、磁盘布局、入口选择测试通过 | `./build/agent_aware_tests`、`./build/test_dynamic_insert`、`./build/test_compaction` |
| SIFT1M 主路径 | 输出 Recall、QPS、P95、resident ratio | `scripts/linux/run_sift1m_once.sh` |
| 混合读写 | 输出 read/write QPS、P95/P99、读路径 breakdown | `./build/bench_mixed_rw --duration_sec 30 --read_threads 4 --write_threads 1 ...` |
| 文档一致性 | README、索引、版本说明、实验报告中的文件名和指标一致 | 搜索旧编号文件名、旧实验目录和旧 mixed benchmark 脚本引用 |
| 答辩材料 | 按评分项准备 4 张核心证据页 | 性能表、创新图、代码结构图、复现命令页 |

## 5. 当前短板与补分策略

| 短板 | 影响 | 补分动作 |
| --- | --- | --- |
| SIFT1M 参数矩阵还不够完整 | 性能指标可能被认为只有单点结果 | 按 `docs/experiments/param-tuning-and-sift-scale-test.md` 补 search_width、beam_width、cache ratio 曲线 |
| delta 搜索仍为 linear scan | 大 delta 下写入后查询性能可能下降 | 实现 Delta memory graph 或分段 immutable delta table |
| flush queue 仍可进一步异步化 | 写入突发时尾延迟仍有风险 | 将 flush 从 manager 热路径拆为后台队列，报告前后 P99 |
| 未在真实 NVMe 上复测 | WSL2 或虚拟化结果可能偏保守 | 在比赛环境或裸机 Linux 上复跑主路径和 mixed RW |
| 图表化材料不足 | 答辩展示冲击力不够 | 将 JSON 结果整理为 Recall/QPS/P95/lookup P95 对比图 |

## 6. 最终材料推荐目录

```text
README.md
PROJECT_PLAN.md
docs/
  README.md
  changelog.md
  competition/
    problem.md
    scoring-and-defense.md
  design/
    ssd-storage-path.md
    cache-zero-copy.md
    async-prefetch.md
    beam-width-io-uring.md
    dynamic-write.md
    fresh-streaming-ann.md
  experiments/
    sift1m-mixed-rw-immutable-view.md
    high-concurrency-mixed-rw.md
    param-tuning-and-sift-scale-test.md
  roadmap/
    dynamic-write-task-breakdown.md
    next-sift1m-mixed-rw-optimization.md
logs/sift_bench/*/result.json
build/sift1m_*.json
```

提交说明中应明确：大文件数据集和索引不进入 Git，结果 JSON 作为实验证据保存；评审复现时按 README 的数据准备与运行命令重新生成。
