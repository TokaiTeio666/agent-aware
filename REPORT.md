# AgentMem-Flow 实验报告

生成日期：2026-06-04

数据来源：
- `archive/results/*.txt`
- `archive/build_info/v9-freshlsh-local-2026-05-29.txt`
- `docs/iterations/v9-analysis.md`
- `docs/sift_experiment_report.md`

## 1. 实验目标

本报告用于汇总 `AgentMem-Flow` 当前仓库内已经归档、可复核的实验结果，重点覆盖以下内容：

- SIFT10K / SIFT100K 图检索效果与延迟表现
- V9 `FreshLSH-Vamana` 构图与 early-stop 低延迟搜索
- PQ + ADC + rerank 的压缩与召回权衡
- FreshVamana + StreamMerge 动态写入/删除 smoke 验证

说明：本报告只记录仓库中已有的真实归档结果，不补造尚未执行的 SIFT1M full-base 数据。

## 2. 实验环境

| 参数 | 值 |
|------|-----|
| 主机系统 | Microsoft Windows NT 10.0.26200.0 |
| 终端 | PowerShell |
| 编译器 | `g++.exe (MinGW-W64) 8.1.0` |
| 处理器线程数 | 28 |
| 构建命令 | `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1` |
| Git 状态 | `ff382de`，工作区非干净 |
| 实验定位 | 本机 Windows smoke / regression 验证 |
| 正式建议环境 | WSL2 Ubuntu ext4 或 AutoDL Linux SSD |

## 3. 数据集与验证范围

| 工作负载 | Base Count | Query Count | 说明 |
|---------|-----------:|------------:|------|
| SIFT10K smoke | 10,000 | 100 | 特性对比与接口验证 |
| SIFT10K full | 10,000 | 1,000 | 顺序图检索验证 |
| SIFT100K full | 100,000 | 1,000 | 参数扫描与推荐配置验证 |
| Dynamic smoke | 合成混合负载 | 读写混合 | FreshVamana + StreamMerge 删除路径 smoke |
| SIFT1M full | 1,000,000 | 未执行 | 仅有脚本，暂无正式结果 |

Ground truth 说明：
- SIFT10K / SIFT100K 子集实验会先检查官方 `sift_groundtruth.ivecs` 是否越过当前 `base_count`。
- 若出现越界 id，则自动切换为当前已加载子集的 exact brute force truth。
- 因此，当前子集结果可用于开发期验证，但不能替代 SIFT1M full-base 官方结论。

## 4. 核心参数配置

### 4.1 V9 FreshLSH-Vamana 关键参数

| 参数 | SIFT10K | SIFT100K | 说明 |
|------|--------:|---------:|------|
| `graph_build_policy` | `lsh-rp` | `lsh-rp` | LSH 候选生成 + 快速裁剪 |
| `graph_degree` | 32 | 32 | 图最大邻接度 |
| `lsh_tables` | 8 | 8 | 多表 SimHash LSH |
| `lsh_bits` | 10 | 14 | 数据规模越大，bits 越高 |
| `lsh_bucket_limit` | 64 | 64 | 单桶候选上限 |
| `search_width`（高召回） | 1024 | 1536 | 构图/高召回验证档 |
| `search_width`（低延迟） | 128 | 384 | 低延迟搜索档 |
| `search_early_stop_min` | 128 | 192 | 自适应停止最小展开数 |
| `entry_count` | 256 | 512 | 查询入口点采样规模 |
| `routing_sample_count` | 4096 | 8192 | 路由样本规模 |

### 4.2 SIFT 特性验证相关参数

| 参数 | 配置 |
|------|------|
| 布局 | `layout=packed`, `packing=bfs` |
| I/O 模式 | 当前已验证 `pread`；`odirect` / `io_uring` 仅接口回退 |
| PQ 配置 | `pq_m=8`, `pq_ks=256`, `pq_code_bytes_per_vector=8` |
| ADC | 已实现距离表构建，支持 `rerank_topk=64` |
| 随机种子 | 42 |

## 5. 关键实验结果

### 5.1 V9 构图与低延迟结果（2026-05-29）

| 实验 | Build s | Recall@10 | P99 ms | SSD Reads / Query | Expanded / Query |
|------|--------:|----------:|-------:|------------------:|-----------------:|
| SIFT10K `lsh-rp` | 0.3640 | 1.0000 | 7.6510 | 769.4700 | 1024.0000 |
| SIFT10K `early128` | N/A | 0.9990 | 6.0019 | 467.1600 | 128.0000 |
| SIFT100K `lsh-rp` | 5.7700 | 0.9990 | 60.6911 | 4436.4700 | 1536.0000 |
| SIFT100K `w384 + early192` | N/A | 0.9760 | 30.5251 | 1941.5500 | 192.0000 |
| FreshLSH StreamMerge smoke | 0.0301 | 0.9848 | 0.3479 | 18.5952 | 96.0000 |

补充结论：
- V8 SIFT10K `approx-rp` 建图为 8.9465s，V9 `lsh-rp` 为 0.3640s，约 24.6x 加速。
- SIFT100K 在本地 Windows smoke 中已能现场建图，`graph_build_seconds=5.7700`。

### 5.2 SIFT10K 特性验证结果（2026-06-02）

| 配置 | Recall@10 | QPS | P99 ms | SSD Reads / Query | 备注 |
|------|----------:|----:|-------:|------------------:|------|
| BFS baseline | 0.9980 | 307.0676 | 5.3552 | 378.7400 | 基线 smoke |
| Hotpath packing | 0.9980 | 65.8301 | 30.1505 | 505.8900 | 当前实现未体现收益 |
| Adaptive early stop | 0.9980 | 92.8972 | 19.1791 | 379.6900 | `expanded/query=64.67` |
| PQ8 + ADC | 0.5760 | 291.9099 | 7.4577 | 388.4400 | 近似排序召回损失明显 |
| PQ8 + ADC + rerank64 | 0.9620 | 389.6484 | 4.7913 | 388.7700 | PQ 码占用 80,000B，对比原始 5,120,000B |

### 5.3 SIFT100K 顺序验证结果（2026-06-02）

| 配置 | Recall@10 | QPS | P99 ms | SSD Reads / Query |
|------|----------:|----:|-------:|------------------:|
| exact | 1.0000 | 164.2455 | 9.6533 | n/a |
| graph `128/64` | 0.8563 | 108.3395 | 17.0001 | 1132.5090 |
| graph `256/96` | 0.9085 | 86.7212 | 21.8221 | 1404.6090 |
| graph `512/128` | 0.9404 | 74.3867 | 24.2162 | 1630.9750 |
| graph `768/160` | 0.9553 | 70.9621 | 27.0958 | 1826.7430 |
| graph `1024/192` | 0.9651 | 31.5805 | 84.8826 | 1998.5390 |

当前已验证推荐档：
- 如果要求 `Recall@10 >= 0.95`，`SEARCH_WIDTH=768`、`SEARCH_EARLY_STOP_MIN=160` 是更稳健的当前推荐配置。
- 如果优先追求更低 P99，V9 的 `w384 + early192` 是值得继续复测的低延迟候选档，但它来自另一组 V9 构图实验，建议在 Linux 主机上统一复验。

### 5.4 动态写入 / 删除 smoke（2026-05-29）

| 指标 | 值 |
|------|----|
| Dynamic Recall@10 | 0.9848 |
| Delete Count | 45 |
| Tombstone Count | 45 |
| WAL Records | 90 |
| StreamMerge Ops | 1 |
| 总耗时 | 0.0301s |

说明：该结果表明 FreshVamana 删除 patch 与 StreamMerge 路径在 smoke 级别已打通，删除计数与 tombstone 计数一致。

## 6. 关键分析

### 6.1 构图效率

- V9 `FreshLSH-Vamana` 已明显缓解原先 exact / `approx-rp` 构图瓶颈。
- 在本地验证里，SIFT10K 构图时间从 8.9465s 降到 0.3640s，SIFT100K 也能在 5.77s 内完成建图。

### 6.2 低延迟搜索

- early-stop 有效，但不能无限压缩展开数。
- 在 V9 SIFT100K 中，`w384 + early192` 将 P99 从 34.3073ms 降到 30.5251ms，同时保持 Recall@10 = 0.9760。
- `early128` 虽然更低延迟，但召回降到 0.9530，只适合对 Recall 下限要求较低的场景。

### 6.3 PQ / ADC 压缩收益与代价

- PQ8 码将原始向量常驻空间从 5,120,000B 压到 80,000B，压缩比为 64x。
- 但纯 PQ + ADC 会把 SIFT10K Recall@10 拉低到 0.5760，无法直接作为高质量默认方案。
- 加入 `rerank64` 后，Recall@10 恢复到 0.9620，说明“PQ 预筛 + 原始向量重排”是当前更合理的折中路线。

### 6.4 当前尚未兑现的系统能力

- `odirect` 与 `io_uring` 参数已具备接口和显式回退字段，但本仓库当前验证结果仍是回退到 `pread`。
- 因此，当前报告不能宣称已经完成 native `O_DIRECT` 或 native `io_uring` 后端优化。

## 7. 诚信声明与局限

- 截至 2026-06-04，SIFT1M full-base cold/warm 正式实验尚未执行完毕。
- 当前多数结果来自本地 Windows smoke / regression，不应直接替代 Linux SSD 环境下的最终对外结论。
- SIFT10K / SIFT100K 子集实验存在官方 truth 越界后切换 exact brute force fallback 的情况，结论应限定在“开发验证”范围。
- Hotpath packing 在当前已验证 smoke 中没有带来正向收益，不能写成已证实优化点。
- native `O_DIRECT` / `io_uring` 尚未落地，不能将相关参数开关等同于真实系统性能提升。

## 8. 结论与下一步

当前仓库已经形成一条可复核的实验链路：
- 图索引构建可在 SIFT10K / SIFT100K 上稳定运行，并已有可归档的 Recall、QPS、P99 与 reads/query 指标。
- V9 `FreshLSH-Vamana` 证明了更快的构图路径和可控的 early-stop 低延迟搜索。
- PQ + ADC 已完成接口验证，但高召回场景仍建议配合 rerank 使用。
- FreshVamana + StreamMerge 动态删除 smoke 已经跑通。

下一步建议：
1. 在 WSL2 Ubuntu ext4 或 AutoDL Linux SSD 上执行 `scripts/linux/run_sift1m_full.sh`，补齐 SIFT1M full-base cold/warm 正式结果。
2. 实现并验证 native `O_DIRECT` 和 native `io_uring`，再复测 SIFT10K / SIFT100K / SIFT1M。
3. 将 V9 低延迟档与 2026-06-02 的 SIFT100K 推荐档放到同一 Linux 主机、同一脚本路径下顺序复验，输出统一结论。
