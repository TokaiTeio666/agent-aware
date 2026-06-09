# AgentMem-Flow 项目实验报告

生成日期：2026-06-09  
当前 Commit：`2bfadbd`  
项目定位：面向 Agent 长期记忆场景的受限内存 SSD 向量检索 I/O 引擎原型。

## 1. 实验环境

| 参数 | 值 |
|---|---|
| 主机 CPU | Intel(R) Core(TM) i7-14700HX |
| 主机内存 | 31.71 GB |
| 主机系统 | Microsoft Windows 11 专业版 10.0.26200 |
| WSL 环境 | Ubuntu on WSL2 |
| Windows 构建 | `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1` |
| WSL 构建 | `bash scripts/linux/build.sh` |
| SIFT 数据路径 | `data/sift/sift_base.fvecs`, `sift_query.fvecs`, `sift_groundtruth.ivecs` |
| WSL 测试路径 | `/mnt/d/agent-aware` |
| 当前 I/O 后端 | `pread` |
| 受限内存口径 | `memory_budget_ratio=0.20`，即常驻引擎内存不超过原始向量字节数的 20% |

说明：本报告只汇总仓库中已经实际运行并归档的结果。SIFT10K/SIFT100K 的 WSL2 测试在 `/mnt/d` Windows 挂载路径运行，适合验证功能、召回和内存合规；最终性能仍建议在 WSL ext4 或原生 Linux NVMe 上复跑。

## 2. 赛题目标与项目约束

大模型 Agent 的长期记忆既规模庞大，又具有持续写入和频繁回忆的动态特征。若将高维向量索引存储在 SSD 上，图遍历式 ANN 检索会产生大量随机读，传统 OS Page Cache 和顺序预取难以稳定发挥作用；实时写入、删除和后台合并又会放大尾延迟。

本项目目标是在内存受限环境下实现一个底层 I/O 存储与向量检索引擎，重点验证：

- 在原始向量数据远大于常驻内存预算时，仍能提供可复现的 Top-K 检索。
- 常驻引擎内存满足 10%-20% 预算约束，默认达标口径为 `<=20%`。
- 以 `Recall@10`、QPS、Avg/P50/P95/P99 延迟、SSD reads/query、I/O amplification 和 `memory_budget_pass` 为核心指标。
- 支持 SIFT 标准数据文件、合成数据、读写混合 workload、WAL、Delta Index、tombstone 和 compaction 指标。

## 3. 核心接口实现

### 3.1 命令行接口

当前对外接口是实验型 CLI，而不是 HTTP 或 SDK。主程序位于 `src/main.cpp`，核心参数如下：

```bash
--memory-budget-ratio 0.20
--memory-budget-bytes N
--enforce-memory-budget
--allow-over-budget-for-debug
```

含义：

| 参数 | 作用 |
|---|---|
| `--memory-budget-ratio` | 按原始向量大小计算常驻内存预算，默认 `0.20` |
| `--memory-budget-bytes` | 指定绝对预算上限；若同时设置 ratio，则取二者较小值 |
| `--enforce-memory-budget` | 超预算时 fail fast，直接退出 |
| `--allow-over-budget-for-debug` | 允许超预算运行继续输出调试指标，但 `memory_budget_pass=0` |

预算计算公式：

```text
raw_vector_bytes = vector_count * dim * sizeof(float)
ratio_budget = raw_vector_bytes * memory_budget_ratio
memory_budget_bytes = min(user_budget_bytes, ratio_budget)
```

若用户未显式指定 `--memory-budget-bytes`，则使用 `ratio_budget`。

### 3.2 常驻内存统计接口

程序内部使用 `MemoryReport` 汇总常驻内存。图索引模式的统计口径为 `engine_resident`，不把全量 raw vectors 算作常驻内存，而是统计真正驻留在检索引擎中的结构：

| 组件 | 是否计入 |
|---|---|
| 全量 raw vectors | graph 模式不计入；exact 模式计入 |
| PQ codes | 计入 |
| PQ codebooks / offsets | 计入 |
| graph metadata | 计入 |
| page cache / BufferPool | 计入 |
| Query Path Cache | 计入 |
| Resident router sample | 计入 |
| Delta Index | 计入 |
| tombstone bitmap / sparse delete | 计入 |
| search frontier / visited / query 临时结构 | 按 temporary peak 估算计入 |

最终输出 key=value 字段，便于脚本归档和 CSV 汇总：

```text
memory_budget_ratio=0.2000
memory_budget_bytes=10240000
memory_resident_bytes=4638664
memory_resident_ratio=0.0906
memory_budget_pass=1
memory_bytes_cache=0
memory_bytes_router=4227224
memory_accounting_scope=engine_resident
```

### 3.3 Linux runner 接口

`scripts/linux/run_sift_local.sh` 已支持环境变量形式的内存预算透传：

```bash
MEMORY_BUDGET_RATIO=0.20
MEMORY_BUDGET_BYTES=...
ENFORCE_MEMORY_BUDGET=1
ALLOW_OVER_BUDGET_FOR_DEBUG=1
```

脚本会将这些变量转为主程序 CLI 参数，用于 SIFT10K/SIFT100K/SIFT1M 后续实验。

## 4. 验证范围

| 工作负载 | Base Count | Query Count | Ground Truth | 状态 |
|---|---:|---:|---|---|
| Synthetic exact | 2,000 | 200 | exact self | 已验证 |
| Synthetic graph | 2,000 | 200 | exact brute force | 已验证 |
| Synthetic mixed read/write | 1,000 | 100/200 ops | dynamic exact brute force | 已验证 |
| SIFT10K WSL2 | 10,000 | 100 | 子集 exact brute force fallback | 已验证 |
| SIFT100K WSL2 | 100,000 | 100 | 子集 exact brute force fallback | 已验证 |
| SIFT1M full | 1,000,000 | 待运行 | 官方 `.ivecs` | 暂未运行 |

Ground truth 说明：官方 `sift_groundtruth.ivecs` 面向完整 SIFT1M。对 SIFT10K/SIFT100K 子集，程序会检测 truth id 是否超过当前 `base_limit`；若越界，则自动切换为当前子集 exact brute force truth，并在日志中输出 `truth=file_out_of_range_exact_bruteforce`。

## 5. 版本合并

为避免报告呈现为 V0-V9 的流水账，当前项目按功能目标合并为五个大版本。后续汇报、答辩和实验归档建议统一使用 M0-M4 命名。

### 5.1 大版本定位

| 大版本 | 合并内容 | 核心目标 | 当前状态 |
|---|---|---|---|
| M0：正确性与验证基线 | V0 exact search、SIFT loader、synthetic generator、Recall/QPS/latency 指标 | 建立 ground truth、数据加载和基础指标链路 | 已完成 |
| M1：SSD 图索引与布局层 | V1 naive SSD graph、V2 packed page layout、BFS/coaccess packing、基础 router | 证明图索引可落盘，并降低随机 I/O 放大 | 已完成 |
| M2：受限内存读优化层 | V3 agent cache、V4 Query Path Cache、PQ/ADC、rerank、early-stop、20% memory budget | 在 `<=20%` 常驻内存下提升 Recall/QPS/P99/reads/query | 已完成核心机制，SIFT PQ 需继续调参 |
| M3：动态写入与合并层 | V5 WAL + Delta Index、V6 file-backed compaction、V8 FreshVamana delete、StreamMerge | 支持实时插入、删除、tombstone、WAL replay 和后台合并 | 功能可跑，mixed Recall 仍需提升 |
| M4：标准数据集与交付层 | V7 signature comparison、V8/V9 SIFT helper、V9 FreshLSH-Vamana、SIFT10K/SIFT100K/后续 SIFT1M | 面向 SIFT100K/SIFT1M 的可复现实验、报告和最终展示 | SIFT10K/SIFT100K 已验证，SIFT1M 待跑 |

### 5.2 原版本到大版本映射

| 原版本 | 原始功能 | 合并后归属 | 说明 |
|---|---|---|---|
| V0 | exact search、SIFT fvecs/ivecs、synthetic smoke | M0 | 作为 correctness upper bound 和指标链路 |
| V1 | one-node-per-page SSD graph baseline | M1 | 建立最朴素 SSD 图索引对照 |
| V2 | packed page layout、BFS/coaccess packing、cold/warm 标记 | M1 | 降低 page read 放大，是后续图索引默认布局 |
| V3 | LRU/agent page cache | M2 | 作为 20% resident memory 可调组件 |
| V4 | Query Path Cache、query signature、path reuse | M2 | 复用相似 query 的入口和路径 |
| V5 | WAL、Delta Index、mixed workload、SLA compaction | M3 | 动态写入主线 |
| V6 | file-backed compaction I/O、AutoDL/SIFT 模板 | M3/M4 | compaction 属于 M3，部署与标准实验模板属于 M4 |
| V7 | signature policy comparison | M4 | 用于实验归档和策略对照 |
| V8 | approx-rp、FreshVamana delete、StreamMerge | M3/M4 | delete/merge 属于 M3，SIFT 构图能力属于 M4 |
| V9 | FreshLSH-Vamana、lsh-rp、SIFT100K 现场构图、early-stop 档 | M4 | 当前 SIFT10K/SIFT100K 推荐主线 |

### 5.3 推荐汇报口径

| 汇报主题 | 使用版本名 | 推荐描述 |
|---|---|---|
| 项目基础能力 | M0 + M1 | 已具备 exact truth、SIFT loader、SSD 图索引和 packed layout |
| 赛题内存约束 | M2 | 已实现 `--memory-budget-ratio 0.20`、`memory_budget_pass` 和组件级统计 |
| 当前最好 SIFT 结果 | M4 | WSL2 SIFT10K/SIFT100K 在 20% 内存约束下通过 Recall 验证 |
| 动态能力 | M3 | WAL/Delta/tombstone/compaction 指标可观测，但 mixed Recall 仍需优化 |
| 未完成项 | M4 后续 | SIFT1M full、ext4/NVMe 复跑、原生 `O_DIRECT`/`io_uring` |

## 6. 性能与内存结果

### 6.1 本地合成数据验证

| 场景 | 配置摘要 | Recall@10 | QPS | P99 延迟 | Resident/Raw | 20% 合规 | 结论 |
|---|---|---:|---:|---:|---:|---|---|
| M0 Exact 正确性基线 | exact, 2000x64 | 1.0000 | 12851.82 | 0.1003 ms | 100.60% | 否 | 仅用于 correctness，不是受限内存实现 |
| M1 SSD 图索引基线 | graph, packed BFS, width=768 | 1.0000 | 370.10 | 3.8750 ms | 13.68% | 是 | 图索引主路径可在 20% 内存约束下达标 |
| M2 PQ/ADC 合成数据优化 | graph + PQ(M=4,K=16) + ADC + rerank64 | 1.0000 | 654.68 | 2.5749 ms | 12.35% | 是 | 合成数据下读放大更低、吞吐更高 |
| M2 默认大码本反例 | graph + PQ(M=8,K=256) + ADC | 1.0000 | 556.08 | 2.8439 ms | 114.67% | 否 | 默认 K=256 在小数据上码本超预算 |
| M3 混合读写 | graph, 20% write, 25% delete | 0.6319 | 1625.93 | 1.0933 ms | 15.68% | 是 | 内存合规，但动态召回未达标 |

结论：受限内存机制已经生效。合成数据上 M2 小码本 PQ/ADC 有收益，但该配置不能直接迁移到 SIFT 主线，需要单独调参。

### 6.2 WSL2 SIFT10K / SIFT100K 达标配置

| 数据集 | 配置 | Recall@10 | QPS | P99 延迟 | SSD reads/query | Resident/Raw | 20% 合规 |
|---|---|---:|---:|---:|---:|---:|---|
| SIFT10K | `lsh-rp`, BFS, early-stop min 192, no cache | 0.9990 | 3.8299 | 731.7332 ms | 561.7500 | 11.42% | 是 |
| SIFT100K | `lsh-rp`, BFS, early-stop min 192, no cache | 0.9640 | 2.4614 | 1678.1738 ms | 1934.1100 | 9.06% | 是 |

SIFT100K 达到 `Recall@10=0.9640`，超过 `>=0.95` 推荐线和 `>=0.85` 最低线。两组都在 `--enforce-memory-budget` 下运行，`memory_budget_pass=1`。

### 6.3 接近 20% 常驻内存占比对照

为验证“如果常驻内存占比接近 20% 会怎样”，在原配置上启用 agent page cache，将 resident ratio 拉到约 19%。

| 数据集 | cache pages | Resident/Raw | Recall@10 | QPS | P99 延迟 | SSD reads/query | cache hit rate | 结论 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| SIFT10K | 96 | 19.10% | 0.9990 | 8.6361 | 278.0768 ms | 550.7600 | 1.96% | 更快，P99 明显改善 |
| SIFT100K | 1250 | 19.06% | 0.9640 | 1.9070 | 2667.5456 ms | 1855.4200 | 4.07% | 读次数略降，但 P99 变差 |

结论：20% 应理解为上限，不是必须用满。SIFT10K 接近 20% 有收益；SIFT100K 读次数下降但尾延迟变差，说明预算分配需要按数据规模、cache 管理成本和真实 I/O 环境调优。当前 SIFT100K 推荐仍是 9.06% resident 的 no-cache 档。

### 6.4 SIFT PQ/ADC 迁移反例

合成数据上表现良好的 `PQ(M=4,K=16) + ADC + rerank64` 直接迁移到 SIFT10K 后：

| 指标 | 值 |
|---|---:|
| Recall@10 | 0.4460 |
| QPS | 4.6187 |
| P99 延迟 | 648.3881 ms |
| Resident/Raw | 12.86% |
| memory_budget_pass | 1 |

结论：该配置内存合规，但召回严重不足，不能作为 SIFT 主线。后续若要使用 PQ，需要提高候选集、调整 `M/K`、增大 `rerank_topk`，或只把 PQ 用作路由/压缩辅助。

## 7. 当前结论

1. 项目已经具备可执行的 20% 常驻内存限制机制，而不是仅在文档中声明受限内存。
2. `memory_budget_pass`、`memory_resident_ratio` 和 `memory_bytes_*` 字段可以清楚解释每个配置为什么合规或不合规。
3. SIFT10K 和 SIFT100K 在 WSL2 上均通过 20% 内存约束和召回验证，其中 SIFT100K `Recall@10=0.9640`。
4. 接近 20% 常驻内存不一定带来更优性能；20% 是约束上限，推荐配置应以 Recall、P99、reads/query 和合规性综合判断。
5. 当前 SIFT WSL2 测试路径位于 `/mnt/d`，尾延迟偏高，不能作为最终 NVMe 性能结论。
6. 动态读写路径已经能统计 delta、tombstone、WAL 和 compaction，但 mixed workload 的主图召回仍需提升。

## 8. Recall 逻辑核查

截图中本地合成数据表有多行 `Recall@10=1.0000`，这不是因为 Recall 计算被写死，也不是 graph 结果直接被当作 truth。核查结论如下：

| 核查项 | 结果 |
|---|---|
| Recall 计算 | `recall_at_k(results, truth, k)` 会逐 query 比较 result id 与 truth id 集合，不会直接返回 1 |
| graph truth 生成 | graph 模式无外部 truth 时，运行后用 exact brute force 为当前 base/query 生成 truth |
| exact truth 特例 | exact 模式可以用自身结果作为 correctness upper bound，因此 exact baseline 必然为 1 |
| SIFT 子集 truth | 官方 SIFT1M truth 越界时，会回退到当前子集 exact brute force truth，并输出 warning |
| 低质量参数反例 | 降低 search width / entry / router sample 后，Recall 会正常下降 |

低宽度 sanity check：

| 数据集 | 故意退化配置 | Recall@10 | 说明 |
|---|---|---:|---|
| Synthetic 2000x64 | degree=8, search_width=8, entry_count=1, routing_sample_count=1 | 0.0240 | Recall 明显下降，证明 synthetic graph 不是恒等满分 |
| SIFT10K | degree=8, search_width=8, entry_count=1, routing_sample_count=1 | 0.0920 | 真实 SIFT 子集上也会下降 |

为什么原 synthetic 表中多行是 1.0000：

- synthetic 数据由明显分离的 cluster center 生成，base 簇内噪声较小，query 也围绕同类中心采样，任务本身偏容易。
- M1/M2 synthetic 使用 exact graph build 或高搜索宽度，且 `search_width=768/256` 远大于低宽度反例，因此能覆盖足够候选。
- PQ/ADC synthetic 档启用了 rerank，候选质量足够时可以恢复 Top-K 排序。
- SIFT 与 mixed workload 并不是全 1：SIFT100K 为 `0.9640`，SIFT10K PQ/ADC 反例为 `0.4460`，mixed workload 为 `0.6319`。

报告口径修正：synthetic 的 `Recall@10=1.0000` 只作为机制与正确性 sanity check，不应作为最终算法效果主结论。正式汇报应优先引用 WSL2 SIFT10K/SIFT100K 与后续 SIFT1M full-base 结果。

## 9. 主要限制与下一步

| 优先级 | 事项 | 目标 |
|---|---|---|
| P0 | 在 WSL ext4 或原生 Linux NVMe 上复跑 SIFT10K/SIFT100K | 获取可信 P99/QPS/I/O 性能 |
| P0 | 跑 SIFT100K 多档 sweep | 比较 `search_width=384/512/768/1024` 与 early-stop min |
| P0 | 调优 mixed workload 动态召回 | 将 Recall@10 从 0.6319 提升到 `>=0.85` |
| P0 | 增加 Recall regression 测试 | 固化低宽度 Recall 应下降的 sanity check |
| P1 | 实现/验证原生 `O_DIRECT` 与 `io_uring` | 替代当前 `pread` fallback |
| P1 | SIFT PQ/ADC 专项调参 | 找到内存合规且 Recall>=0.95 的压缩路径 |
| P2 | SIFT1M full-base 实验 | 使用官方 `.ivecs`，形成最终展示结果 |

## 10. 证据文件

| 文件 | 内容 |
|---|---|
| `docs/SYNTHETIC_VALIDATION_REPORT.md` | 本地合成数据与受限内存机制验证 |
| `docs/WSL2_SIFT_VALIDATION_REPORT.md` | WSL2 SIFT10K/SIFT100K 验证与 near-20% 对照 |
| `docs/sift_experiment_report.md` | SIFT 实验历史汇总 |
| `archive/results/wsl-sift10k-lsh-budget-20260605-130016.txt` | SIFT10K no-cache 达标结果 |
| `archive/results/wsl-sift100k-lsh-budget-20260605-130120.txt` | SIFT100K no-cache 达标结果 |
| `archive/results/wsl-sift10k-lsh-cache20-20260605-131427.txt` | SIFT10K 接近 20% cache 对照 |
| `archive/results/wsl-sift100k-lsh-cache20-20260605-131500.txt` | SIFT100K 接近 20% cache 对照 |
| `archive/results/wsl-sift10k-m2-pq-budget-20260605-125914.txt` | SIFT10K PQ/ADC 反例 |
| `archive/results/m1-synthetic-pq-adc-rerank-budget-20260605.txt` | 合成数据 PQ/ADC 合规结果 |
| `archive/results/m1-synthetic-mixed-budget-20260605.txt` | 合成数据混合读写结果 |
| `archive/results/debug-recall-lowwidth-synthetic-20260609.txt` | synthetic 低宽度 Recall sanity check |
| `archive/results/debug-recall-lowwidth-sift10k-20260609.txt` | SIFT10K 低宽度 Recall sanity check |
