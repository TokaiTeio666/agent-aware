# AgentMem-Flow V3 分析报告

## 版本信息

- 版本：V3 - Agent-Aware Page Cache
- 日期：2026-05-28
- 状态：已完成

## 版本目标

V3 的目标是在 V2 packed page layout 基础上增加应用级 page cache，验证缓存策略是否能进一步降低 SSD page miss、提升 warm run 稳态性能，并为后续 Query Path Cache 提供缓存基础。

本版本对比三种策略：

- `none`：无全局 page cache。
- `lru`：按最近访问时间淘汰。
- `agent`：综合访问频率、最近访问时间和 page 内节点密度进行淘汰。

## 对应计划书问题

计划书位置：`docs/PROJECT_PLAN.md`。

| 计划书问题 | 状态 | 本版本说明 |
|---|---|---|
| P4：普通缓存不理解 Agent 访问局部性 | 部分解决 | V3 实现 Agent-aware page cache，并在 warm run 中优于 LRU |
| P3：随机读放大 | 继续优化 | 在 V2 packed layout 的 21.4100 reads/query 基础上，Agent cache 降到 16.0800 |
| P0：验证方法不完整 | 部分解决 | V3 增加 cache hit rate、hits/query、misses/query，并设置 pass criteria |
| P9：归档不完整 | 部分解决 | V3 已归档配置、日志、结果和 build info |

## 验证定义

- Recall@10：系统返回 Top-10 与 ground truth Top-10 的交集比例，对所有 query 取平均。
- 1-Recall@10：`1 - Recall@10`。
- Ground Truth 来源：synthetic workload 使用 V0 exact brute force。
- Run 类型：warm run，执行 1 轮 warmup 后记录正式指标。
- 全局验证规范：`docs/VALIDATION_METHOD.md`。

## 实现范围

新增内容：

- packed index 跨 query 全局 page cache。
- `--cache-policy none|lru|agent`。
- `--cache-pages N`。
- page cache hit/miss/request 统计。
- `page_cache_hit_rate` 输出。
- `scripts/run_v3_cache_compare.ps1`。

Agent-aware score：

```text
score = frequency * 1000 + recency + page_density * 0.01
```

未包含内容：

- 显式 session id。
- 显式 semantic cluster id。
- query path cache 信号。
- Linux 严格 cold run。

这些会在 V4 及后续 Agent-style workload 中继续增强。

## 验证方式

构建命令：

`powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

对比脚本：

`powershell -ExecutionPolicy Bypass -File .\scripts\run_v3_cache_compare.ps1`

主实验 workload：

- synthetic dataset
- base_count = 2000
- query_count = 200
- dim = 64
- clusters = 32
- seed = 42
- k = 10
- layout = packed
- packing = coaccess
- cache_pages = 24
- warmup_runs = 1

## 通过标准

| 标准 | 是否通过 | 说明 |
|---|---|---|
| 构建成功 | 是 | `scripts/build.ps1` 可生成 `build/agentmem_flow.exe` |
| cache hit rate 可观测 | 是 | LRU 为 0.1950，Agent 为 0.2489 |
| Agent cache hit rate 高于 LRU | 是 | 0.2489 > 0.1950 |
| Agent SSD reads/query 低于 LRU | 是 | 16.0800 < 17.2350 |
| Agent P99 低于无缓存和 LRU | 是 | Agent 0.2548 ms，LRU 0.2908 ms，无缓存 0.4955 ms |
| Recall@10 不下降 | 是 | 三种策略均为 0.9985 |
| 结果、配置、日志、环境信息已归档 | 是 | 已补充 `archive/configs`、`archive/logs`、`archive/build_info` |

## 指标结果

Warm run 对比：

| 策略 | Cache Pages | Recall@10 | Hit Rate | Avg ms | P95 ms | P99 ms | SSD Reads / Query |
|---|---:|---:|---:|---:|---:|---:|---:|
| none | 0 | 0.9985 | 0.0000 | 0.2245 | 0.2799 | 0.4955 | 21.4100 |
| LRU | 24 | 0.9985 | 0.1950 | 0.1837 | 0.2381 | 0.2908 | 17.2350 |
| Agent-aware | 24 | 0.9985 | 0.2489 | 0.1761 | 0.2286 | 0.2548 | 16.0800 |

相对 V2 no-cache packed layout：

```text
reads/query: 21.4100 -> 16.0800
reduction: 24.89%
p99 latency: 0.4955 ms -> 0.2548 ms
reduction: 48.58%
```

相对 LRU：

```text
hit rate: 0.1950 -> 0.2489
reads/query: 17.2350 -> 16.0800
p99 latency: 0.2908 ms -> 0.2548 ms
```

## 结果分析

V3 达到了最小可实现目标：cache hit rate 可观测，并且 Agent-aware cache 在 warm run 中相对 LRU 获得更高命中率、更低 page miss 和更低 P99 latency。

LRU 相比无缓存已经降低了 `ssd_reads/query`，但 P99 改善有限。Agent-aware 策略保留访问频率更高的 packed pages，因此在重复查询序列 warm run 中比 LRU 更稳定。当前策略还没有显式使用 session id、semantic cluster 或 query path 信息，因此只能算 V3 的最小 Agent-aware 版本；后续 V4 可以把 Query Path Cache 的路径热度反馈给 cache score。

## 风险与局限

- 当前 workload 仍是 synthetic，不是真实 Agent trace。
- 当前 Agent-aware score 主要使用 frequency 和 recency，语义/会话/路径信号尚未显式建模。
- 当前实验是 Windows warm run，不是 Linux strict cold run。
- cache 容量只用 24 pages 做一次对比，后续应补 cache size sweep。

## 归档结果

- `archive/results/v3-cache-2026-05-28.txt`
- `archive/configs/v3-cache-2026-05-28.json`
- `archive/logs/v3-cache-2026-05-28.log`
- `archive/build_info/v3-cache-2026-05-28.txt`
- `archive/validation/validation-method-2026-05-28.md`

## 下一步

进入 V4：实现 Query Path Cache。V4 应为 query 生成 signature，复用相似 query 的入口点或候选 frontier，并将 path hotness 反馈给 V3 Agent-aware cache。

