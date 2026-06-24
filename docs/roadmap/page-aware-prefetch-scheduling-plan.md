# Page-Aware Prefetch Scheduling Plan

## 实施状态（2026-06-23）

已落地：

- Phase 0 指标拆分：`unique_pages_touched`、`unique_demand_pages`、`unique_prefetch_pages`、`prefetch_only_pages`、`beam_unique_pages`、`beam_cached_or_pending_pages`、`page_aware_*`、`next_hop_hints_*` 已进入结果 JSON。
- Phase 1 JIT frontier prefetch：支持 `--jit-frontier-prefetch` 和 `--jit-window-multiplier`，在 beam 选择前预取 top frontier window。
- Phase 2 page-aware beam selection：支持 `--page-aware-beam`、window multiplier、新页上限、distance slack、reuse/availability bonus。
- Phase 3 next-hop hint promotion：支持 `--next-hop-hints`，next-hop 可以先进入 hint table，再由 JIT frontier 按复用/距离升级成预取。
- Phase 4 adaptive controller：adaptive gate 已从单一 useful ratio 扩展为 useful ratio + waste budget。

当前 SIFT1M 观察：

- page-aware beam 已能让 `submitted_reads/query` 低于 no-prefetch baseline，证明“低于 baseline 必须减少搜索页集合”的路径成立。
- JIT frontier 的 useful ratio 可以接近 100%，`prefetch_only_pages/query` 很低。
- 默认初始参数仍需要继续调优尾延迟；page-aware only 降低 submitted/q，但 P95 不稳定，JIT 组合降低 demand/q 明显但可能引入调度开销。

后续调参重点：

- 在 `max_new_pages_per_batch`、`distance_slack_ratio`、`prefetch_width`、`jit_window_multiplier` 上做小矩阵。
- 优先寻找同时满足 `submitted/q < baseline`、Recall@10 下降不超过 0.001、P95/P99 不差于 baseline 的组合。

## 背景结论

SIFT1M 预取测试已经证明两件事：

1. 预取机制本身可以做到不放大 I/O。`frontier adaptive` 的 useful ratio 达到 100%，`submitted_reads/query` 回到 no-prefetch 基线。
2. 只优化 prefetch 候选质量，无法让 `submitted_reads/query` 低于 no-prefetch 基线。搜索扩展页集合不变时，预取只是把未来 demand read 提前提交，理论下界就是 no-prefetch 的读页集合。

因此下一阶段目标不能再表述为“继续调 next-hop 预取参数”。真正要做的是把搜索调度改成 page-aware，让 beam 扩展本身少触碰页面，同时保留异步预取对尾延迟的收益。

## 目标

核心目标：

- 在 Recall@10 基本不变的前提下，让 `submitted_reads/query` 低于 no-prefetch 基线。
- 保留或提升 adaptive prefetch 带来的 P95/P99 收益。
- 降低无效 next-hop/fallback 预取，避免把 I/O 放大藏在 `prefetch_reads` 里。

建议验收口径：

| 指标 | 目标 |
| --- | --- |
| Recall@10 | 相对 baseline 下降不超过 0.001 |
| `submitted_reads/query` | 低于 no-prefetch baseline，首阶段目标 -3% |
| `demand_reads/query` | 低于 no-prefetch baseline，首阶段目标 -8% |
| `unique_pages_touched/query` | 低于 no-prefetch baseline |
| `prefetch_useful_pages / prefetch_reads` | frontier/JIT 路径 >= 80% |
| `prefetch_wasted_pages/query` | 明确下降，目标 < 1 page/query |
| P95/P99 | 不差于 no-prefetch，优先保留 >= 5% 改善 |

## 非目标

- 不以牺牲 Recall 换 I/O 降低。
- 不把 submitted/q 下降建立在 query 数过少或 cache warm 偶然性上。
- 不继续扩大 `prefetch_width/depth` 来追 tail latency，除非同时证明 total submitted reads 不上升。
- 不把 next-hop 预取做成新的无界候选队列。

## 关键判断

### 1. 预取时机问题

当前 next-hop 的触发点是“扩展节点后立刻看邻居”。这时邻居只是被发现，还没有证明会进入下一批 beam。它适合降低未来等待，但容易读到不会扩展的页。

更合理的触发时机是：

- page 已经出现在候选堆的近端窗口；
- page 在 top candidate window 中有多个候选复用；
- page 即将进入下一批 beam；
- page 已经 pending/ready/cache，选择它不会新增 I/O。

### 2. 低于 baseline 需要改搜索页集合

如果不改变 beam 选择，最终要扩展的节点页集合基本固定。此时预取最多把 `demand_reads` 转成 `prefetch_reads`，不能让 `submitted_reads` 低于基线。

要低于 baseline，需要 page-aware beam selection：

- 从 top candidate window 中选择一批候选；
- 在距离质量相近时优先选择同页候选；
- 优先选择 cache/pending/ready 页上的候选；
- 限制每轮新增 page 数，而不是只限制候选数。

## 阶段计划

### Phase 0: 指标拆分和基线固定

目的：避免把“提前读”和“多读”混在一起。

新增或确认指标：

- `unique_pages_touched/query`
- `unique_demand_pages/query`
- `unique_prefetch_pages/query`
- `prefetch_only_pages/query`
- `prefetch_use_lag_expansions`
- `prefetch_use_lag_us`
- `beam_unique_pages/query`
- `beam_cached_or_pending_pages/query`
- `page_aware_candidate_skips`
- `page_aware_distance_slack_rejects`

固定测试矩阵：

- SIFT1M, query=100, single thread
- no-prefetch baseline
- current strict next-hop adaptive
- frontier adaptive
- page-aware beam without prefetch
- page-aware beam with JIT prefetch

验收：

- 指标能解释 `submitted_reads = demand_reads + prefetch_reads`。
- 每组输出都能看出 unique page set 是否真的变小。

### Phase 1: JIT Frontier Prefetch

目的：把预取时机从“发现邻居时”推迟到“候选即将被 beam 消费时”。

设计：

- 在每轮 beam 选择前，先从候选堆窥探 top window。
- 对 top window 做页级 coalesce。
- 只预取下一轮高概率会进入 batch 的页。
- 不再从远端 frontier 做 fallback。
- 保留 async pending/ready 命中统计。

建议参数：

| 参数 | 初始值 |
| --- | --- |
| `jit_window_multiplier` | 2 |
| `jit_prefetch_width` | min(beam_width, 8) |
| `jit_min_candidates_per_page` | 1 或 2 |
| `jit_distance_slack` | 0.0 到 0.02 扫描 |

预期：

- useful ratio 明显高于 next-hop。
- submitted/q 接近 baseline。
- P95/P99 优于 no-prefetch。

### Phase 2: Page-Aware Beam Selection

目的：让搜索本身少读页，这是 submitted/q 低于 baseline 的必要条件。

设计：

1. 从候选堆取出 `beam_window = beam_width * window_multiplier` 个候选。
2. 按 page 分组，统计：
   - page 内候选数；
   - page 最佳距离；
   - page 是否 cached/pending/ready；
   - page 是否已经 materialized；
   - page 是否来自 next-hop hint。
3. 计算 page score：

```text
score(page) =
  best_distance_rank
  - reuse_bonus * log1p(candidate_count)
  - availability_bonus
  + distance_slack_penalty
```

4. 选择 batch 时优先复用少量页面：
   - 先选距离最优页；
   - 在 slack 范围内补同页候选；
   - 限制每轮新增 page 数；
   - 不满足 slack 的候选放回堆。

建议参数：

| 参数 | 初始值 |
| --- | --- |
| `page_aware_beam` | 0/1 |
| `beam_window_multiplier` | 2 |
| `max_new_pages_per_batch` | ceil(beam_width / 2) |
| `distance_slack_ratio` | 0.01 |
| `reuse_bonus` | 0.25 |
| `availability_bonus` | 0.5 |

验收：

- `beam_unique_pages/query` 下降。
- `submitted_reads/query` 低于 no-prefetch baseline。
- Recall@10 下降不超过 0.001。

### Phase 3: Next-Hop Hint, Not Immediate Read

目的：保留 topology 信息，但不让 next-hop 直接提交低确定性 I/O。

设计：

- 扩展节点后，不立即预取所有 next-hop 页。
- 把 next-hop 转成 hint：
  - node id
  - page id
  - ADC distance
  - source expanded node
  - first_seen expansion
- hint 只有满足以下条件才升级成 prefetch：
  - page 进入 JIT frontier window；
  - page 在 hint table 中被多次命中；
  - page 与当前 top frontier 距离差在 slack 内；
  - page 已经 pending/ready/cache。

建议参数：

| 参数 | 初始值 |
| --- | --- |
| `next_hop_hint_min_reuse` | 2 |
| `next_hop_hint_ttl_expansions` | 32 |
| `next_hop_promote_slack_ratio` | 0.02 |

预期：

- next-hop useful ratio 上升。
- `prefetch_only_pages/query` 下降。
- P99 保留收益。

### Phase 4: Adaptive Controller 改造

目的：adaptive 不只看 useful ratio，还要看是否增加 total submitted reads。

当前 adaptive 只按 useful ratio 开关 width，下一步改为多指标控制：

```text
if submitted_delta > budget and useful_ratio < target:
  reduce next-hop width
elif tail_latency_bad and useful_ratio >= target:
  increase JIT/frontier width
elif demand_reads下降但submitted不下降:
  prefer JIT/frontier, disable immediate next-hop
```

建议窗口指标：

- `window_submitted_per_query`
- `window_demand_per_query`
- `window_prefetch_only_per_query`
- `window_useful_ratio`
- `window_p95/p99`

验收：

- adaptive 不再把低收益 next-hop 维持在高 width。
- 不同 query 段落下 submitted/q 不出现持续上升。

## 实验矩阵

第一轮只做小矩阵，避免组合爆炸：

| 实验 | prefetch | beam | 目的 |
| --- | --- | --- | --- |
| A | off | normal | baseline |
| B | frontier adaptive | normal | 证明预取不放大 I/O |
| C | strict next-hop adaptive | normal | 当前优化点对照 |
| D | off | page-aware | 验证搜索页集合是否下降 |
| E | JIT frontier | page-aware | 目标组合 |
| F | next-hop hint + JIT | page-aware | 验证 topology hint 增益 |

每个实验输出：

- JSON 结果；
- command.txt；
- `submitted/q`, `demand/q`, `prefetch/q`;
- `unique_pages_touched/q`;
- Recall@10;
- QPS, avg, P95, P99;
- useful/wasted prefetch;
- page-aware skip/slack 统计。

## 风险和回退

| 风险 | 表现 | 回退策略 |
| --- | --- | --- |
| page-aware beam 损伤 Recall | Recall@10 下降 | 缩小 slack，降低 reuse bonus，扩大 search_width |
| JIT 预取太晚 | pending hit 多，ready hit 少 | 提前一个 batch window 预取，增加 width |
| next-hop hint 太保守 | P99 收益消失 | 降低 hint reuse 门槛，允许 slack 内提前读 |
| adaptive 振荡 | width 频繁 0/4 切换 | 加 hysteresis 和最小保持窗口 |
| 指标误判 | submitted/q 下降但 recall 掉 | 所有 I/O 指标必须和 Recall 一起看 |

## 推荐实现顺序

1. Phase 0 指标先落地。
2. 实现 page-aware beam 的只读实验模式，不启用 prefetch。
3. 若 page-aware beam 能降低 `unique_pages_touched/query`，再接 JIT frontier prefetch。
4. 把 next-hop immediate prefetch 改为 hint table。
5. 最后重做 adaptive controller。

## 审核点

请重点确认以下决策：

- 是否接受“submitted/q 低于 baseline 必须改搜索调度”的判断。
- page-aware beam 是否允许在距离 slack 内重排候选。
- Recall@10 允许的最大下降是否设为 0.001。
- 首阶段 submitted/q 目标是否定为 -3%。
- next-hop 是否从 immediate prefetch 改成 hint promotion。
- 是否优先做 page-aware beam，而不是继续调 prefetch 参数。
