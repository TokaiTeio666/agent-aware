# 20% 内存约束规划与验收方案

## 1. 目标与核心定位

本项目的核心性能能力统一表述为：

> 在检索进程真实峰值常驻内存不超过原始全精度向量数据 20% 的条件下，实现百万级、可持续写入的端侧 ANN 检索，并保持 Recall@10 不低于 95% 和稳定的尾延迟。

PQ、Graph-Aware 2Q、异步 I/O、拓扑预取和 LSM 动态写入均为实现该目标的技术手段，不单独作为项目的核心性能结论。

项目内部以 15%-16% 为正常运行目标，以 20% 为不可突破的验收硬上限。预留的 4%-5% 用于线程栈、allocator 碎片、临时查询对象、动态写入波动和运行库驻留页等难以完全静态估算的开销。

## 2. 指标定义

### 2.1 分母：原始全精度向量数据

内存比例的分母固定为未压缩、全精度向量数据的逻辑大小：

```text
DatasetBytes = VectorCount * Dimension * sizeof(float)
```

不得使用 packed index 文件大小、PQ 文件大小、数据目录总大小或压缩后大小作为分母。

SIFT1M 的分母为：

```text
1,000,000 * 128 * 4 = 512,000,000 bytes
20% hard limit       = 102,400,000 bytes = 97.66 MiB
16% operating target =  81,920,000 bytes = 78.13 MiB
15% design target    =  76,800,000 bytes = 73.24 MiB
```

所有计算先使用 bytes，展示时可以同时给出 MiB。不得混用十进制 MB 与二进制 MiB 后再计算比例。

### 2.2 分子：真实峰值 PSS

正式验收指标定义为：

```text
PeakMemoryRatio = WholeProcessPeakPssBytes / DatasetBytes
```

`WholeProcessPeakPssBytes` 是从索引打开、预热到持续负载结束期间观测到的进程最大 PSS，不是单次空闲快照，也不是代码中的组件预算之和。

Whole-process PSS 应包含：

- PQ codes、codebook、节点目录和访问标记；
- Graph-Aware 2Q 页面缓存；
- MemTable、immutable table、可见 delta 和动态索引结构；
- 查询候选集、重排缓冲区和每线程工作区；
- 线程栈、allocator arena 和内存碎片；
- 引擎依赖库及已驻留的文件映射页；
- benchmark 进程内其他实际驻留页。

`estimated_resident_ratio` 继续保留，但只作为配置阶段的预算估算，不作为 20% 约束的通过证据。

### 2.3 辅助口径

为区分引擎开销和测试壳开销，同时报告：

```text
EngineIncrementalPeakPss = WholeProcessPeakPss - IdleHarnessPss
```

硬约束以 `WholeProcessPeakPss` 为准，`EngineIncrementalPeakPss` 仅用于定位优化空间。

HarmonyOS 完整应用中的 ArkTS、ArkUI、RDB 和嵌入模型内存单独报告为 `FullAppPss`。检索引擎 20% 验收使用独立原生 benchmark 进程；完整应用 PSS 作为产品资源指标展示，避免将嵌入模型大小混入检索引擎算法对比。

## 3. SIFT1M 内存规划

SIFT1M 的硬上限为 97.66 MiB。建议按以下范围规划：

| 组件 | 建议预算 | 说明 |
|---|---:|---|
| PQ codes、codebook、节点目录 | 35-38 MiB | 固定常驻结构，启动时实测 |
| Graph-Aware 2Q 页面缓存 | 22-28 MiB | 根据剩余预算计算，不使用固定大缓存 |
| Delta/MemTable | 5-8 MiB | 到达高水位后触发 flush |
| 查询及每线程工作区 | 3-5 MiB | 并发度变化时重新计算 |
| 线程栈、allocator、依赖库 | 8-12 MiB | 通过空壳与实际 PSS 差值校准 |
| 安全余量 | 8-15 MiB | 吸收峰值和估算误差 |
| 正常运行目标 | 73-78 MiB | 原始数据的 15%-16% |
| 验收硬上限 | 97.66 MiB | 原始数据的 20% |

当前 `15000 * 4096` 页约占 58.6 MiB，仅缓存本身就消耗约 12% 的原始数据大小，不适合作为严格 20% PSS 约束下的默认配置。第一轮真实 PSS 测试建议从 `6000-7500` 页开始，再根据固定结构和运行时开销校准。

缓存页数应由剩余预算计算：

```text
CacheBudget = HardBudget
            - FixedResident
            - DeltaReserve
            - QueryWorkspaceReserve
            - RuntimeReserve
            - SafetyMargin

CachePages = floor(CacheBudget / PageSize)
```

当 `CacheBudget <= 0` 时，配置必须失败或退化到无缓存模式，不能只输出 warning 后继续运行。

## 4. 运行时预算控制

预算控制器至少维护以下水位：

| 水位 | 动作 |
|---:|---|
| `< 16%` | 正常运行，允许按策略使用缓存和动态层预算 |
| `>= 16%` | 停止扩大缓存，记录高水位事件 |
| `>= 18%` | 主动淘汰冷页，加速 MemTable flush，禁止非必要预取 |
| `>= 19%` | 限制新增查询并发和动态写入积压 |
| `>= 20%` | 判定本轮失败；拒绝继续增加受控内存并输出诊断信息 |

控制器应区分两类数据：

- 快速内部计数器：用于每次分配前判断预算，覆盖缓存、MemTable 和工作区等可控内存；
- 周期性真实 PSS：用于发现 allocator、线程栈和库驻留等内部计数器未覆盖的开销。

PSS 采样不应位于每次查询的关键路径。建议由独立 telemetry 线程每 100-500 ms 采样一次。

## 5. 测量方法

### 5.1 Linux/WSL

使用 `/proc/<pid>/smaps_rollup` 中的 `Pss` 作为进程 PSS。测试脚本每 100-500 ms 记录：

- 时间戳；
- RSS；
- PSS；
- 缓存驻留页数；
- MemTable/delta 字节数；
- 活跃查询数；
- 已完成查询和写入数；
- compaction/flush 状态。

同时记录 cgroup `memory.current`（环境支持时）、effective I/O mode、O_DIRECT 是否生效、page reads 和设备读取计数。这样可以识别依赖 OS Page Cache 获得 warm-cache 性能的情况。

### 5.2 HarmonyOS

真机报告必须固定并记录：

- 机型和系统版本；
- SDK/NDK 版本；
- PSS 采集工具与命令；
- 应用性能模式、温度和可用存储；
- 原生引擎 PSS 与完整应用 PSS；
- 平台实际使用的 I/O backend。

HarmonyOS 和 WSL 的数据分表展示，不使用 WSL 结果代替真机结论。

### 5.3 单轮流程

每轮测试执行以下固定流程：

1. 启动空 benchmark，记录 `IdleHarnessPss`。
2. 打开已构建的 packed index，记录打开后 PSS。
3. 执行固定查询集预热，预热不计入性能结果，但计入内存峰值。
4. 运行不少于 10 分钟的稳定负载；最终稳定性验收运行 30 分钟。
5. 每 100-500 ms 采集一次内存和引擎 telemetry。
6. 负载结束后 flush、close，确认后台线程退出。
7. 输出性能 JSON、PSS 时间序列和系统环境信息。

每个正式配置运行三轮：性能指标报告三轮中位数和范围；20% 硬约束采用三轮中最大的 Peak PSS 判定。

## 6. 核心实验矩阵

### 6.1 内存档位曲线

使用相同索引、查询集、Recall 参数和并发度，只改变真实内存预算：

| 预算档位 | Peak PSS Ratio | Recall@10 | QPS | P95 | P99 | Reads/query | Cache hit |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 8% | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |
| 12% | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |
| 16% | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |
| 20% | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |

该实验用于回答：在不同内存约束下，系统以多少额外 SSD I/O 和尾延迟换取内存节省，以及 16% 是否能够成为推荐运行点。

### 6.2 同预算 A/B 对照

至少完成以下两组：

| 方案 | I/O | 缓存 | 预取 |
|---|---|---|---|
| 基线 | 同步 `pread` | LRU | 关闭 |
| 最终方案 | 平台有效异步后端 | Graph-Aware 2Q | 自适应拓扑预取 |

两组必须满足：

- 使用相同的 Whole-process Peak PSS 上限，而不是只设置相同缓存页数；
- 使用相同索引、查询窗口、Recall 目标、并发度和 cold/hot 条件；
- 记录 requested 和 effective I/O mode；
- 预取产生的 pending、useful 和 wasted page 均计入统计。

建议优化验收目标：在 Recall 不下降且 Peak PSS 不增加的前提下，相对基线达到 P95 降低至少 15% 或 QPS 提升至少 20%。

### 6.3 持续动态负载

最终能力使用 80/20 读写负载验证：

- 运行时间不少于 30 分钟；
- Recall@10 不低于 0.95；
- Whole-process Peak PSS 不超过 20%；
- 推荐运行点的目标不超过 16%；
- P99 不随运行时间持续恶化；
- PSS 时间序列无持续正向增长趋势；
- MemTable 能按预算 flush；
- compaction 后内存能够回落；
- 重启恢复后结果可见性正确。

## 7. 验收门槛

### 7.1 硬门槛

- SIFT1M `WholeProcessPeakPssBytes / 512000000 <= 0.20`；
- Recall@10 `>= 0.95`；
- 30 分钟持续负载无 OOM、崩溃或不可恢复错误；
- requested/effective I/O mode、缓存配置和原始 PSS 轨迹可追溯；
- 三轮测试中任意一轮超过 20% 均判定该配置不通过。

### 7.2 推荐竞争力目标

- 推荐配置 Peak PSS ratio `<= 0.16`；
- 相同真实内存预算下，P95 降低 `>= 15%` 或 QPS 提升 `>= 20%`；
- 80/20 持续负载下 P99 和 PSS 无明显漂移；
- 内存档位变化时 Recall、延迟和 reads/query 的变化可解释。

## 8. 输出产物

每轮原始 JSON 至少包含：

```text
dataset_bytes
memory_budget_bytes
idle_harness_pss_bytes
open_pss_bytes
whole_process_peak_pss_bytes
engine_incremental_peak_pss_bytes
peak_memory_ratio
fixed_resident_bytes
cache_resident_bytes
delta_resident_bytes
query_workspace_reserved_bytes
requested_io_mode
effective_io_mode
recall_at_10
qps
latency_p50_ms
latency_p95_ms
latency_p99_ms
reads_per_query
cache_hit_rate
prefetch_useful_pages
prefetch_wasted_pages
run_duration_sec
commit
dirty_worktree
```

同时保存：

- PSS/RSS 时间序列 CSV；
- benchmark 完整命令；
- 系统和存储设备信息；
- 三轮汇总表；
- 内存档位曲线；
- 同预算 A/B 对照图；
- 30 分钟动态负载的 PSS、P99 和 delta 大小时间序列。

正式答辩数据必须来自固定 clean commit。开发阶段允许 dirty worktree，但不得作为最终结论。

## 9. 实施顺序

1. 增加独立 PSS sampler 和时间序列输出。
2. 将当前估算报告拆分为固定结构、缓存、动态层、查询工作区和 runtime reserve。
3. 将缓存页数改为剩余预算驱动，并加入配置失败条件。
4. 为 MemTable、预取和查询并发增加预算高水位动作。
5. 先完成单线程 16%/20% 两个档位的真实 PSS 校准。
6. 完成 8%/12%/16%/20% 内存曲线。
7. 在相同 Peak PSS 下完成基线与最终方案 A/B。
8. 完成 30 分钟 80/20 动态负载和三轮正式复测。
9. 在 HarmonyOS 真机复用相同口径重新验证。

## 10. 对外结论模板

答辩时优先使用以下表述：

> Agent-aware 面向端侧 Agent 长期记忆，将全精度向量和图节点主体常驻 SSD。在 SIFT1M 上，检索进程 Whole-process Peak PSS 为原始向量数据的 `<实测值>`，Recall@10 为 `<实测值>`；在持续 80/20 读写负载下，P99 为 `<实测值>`，内存和尾延迟未随运行时间持续增长。相同真实内存预算下，Graph-Aware 2Q 与自适应预取使 `<P95 降低值或 QPS 提升值>`。

在正式数据完成前不得将 `estimated_resident_ratio` 写成真实内存结果，也不得把 requested `io_uring` 写成 effective I/O mode。
