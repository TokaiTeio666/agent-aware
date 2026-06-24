# XGBoost 预取提前触发点方案

## 1. 背景

当前预取链路已经收敛到单路线：

```text
PQ+ADC 候选生成
  -> page-level 特征聚合
  -> XGBoost 预取收益打分
  -> top-K / threshold / max-inflight / io_depth 限流
  -> ready_hit / pending_hit / unused 评估
```

但在线 A/B 暴露的主要问题仍是触发太晚：预取提交后很快遇到 demand，同一页常常还在 pending 状态，形成 `pending_hit`，没有转化为真正的 `ready_hit`。

本方案只解决一个核心问题：

```text
让高置信 page 在 demand 前至少提前一个搜索批次被提交。
```

不重新引入非 PQ 路线，不恢复 `current/learned/frontier/next-hop` 策略入口。

---

## 2. 目标与非目标

### 目标

1. 提高 `was_ready_before_demand` 和在线 `prefetch_ready_hit`。
2. 降低 `prefetch_pending_hit / prefetch_submitted`。
3. 不明显提高 `prefetch_unused`、`submitted_reads`、`io_submit_syscalls`。
4. 保持 `--prefetch-policy none|xgboost` 的外部接口不变。
5. 所有提前触发仍使用 XGBoost page-level score 和统一限流器。

### 非目标

1. 不恢复非 PQ exact neighbor candidate 路线。
2. 不新增 `current`、`learned`、`frontier`、`next-hop` 公开策略。
3. 不把预取改成无界背景扫描。
4. 不为了 ready hit 牺牲 recall 或大幅放大 I/O。

---

## 3. 现状触发点

当前主要触发位置在 `PackedDiskGraphIndex` 扩展 candidate 后：

```text
expand current node
  -> 得到 neighbors
  -> PQ+ADC 打分并入候选队列
  -> submit_next_hop_prefetch(...)
  -> 下一批 demand 可能马上读取同一批 page
```

对应代码入口：

| 位置 | 当前作用 |
|---|---|
| `src/graph/disk_graph_index.cpp` | 在 candidate expansion 后调用 `QueryPageSession::submit_next_hop_prefetch` |
| `src/core/query_page_session.cpp` | 收集 page candidates、调用 `PrefetchPlanner`、提交 async read |
| `src/core/prefetch_planner.cpp` | page-level 聚合、XGBoost 打分、限流 |

问题是：

```text
submit_time 与 demand_time 距离太短
ready_time 通常晚于 demand_time
```

所以必须把触发点前移到“页即将成为 demand 的前一阶段”。

---

## 4. 核心设计

### 4.1 两阶段预取

把预取拆成两个阶段：

```text
候选发现阶段：尽早发现未来可能 demand 的 page
提交决策阶段：仍由 XGBoost + throttle 决定是否提交
```

关键变化是：

```text
发现候选的时间提前；
打分和限流逻辑不分叉。
```

也就是说，不新增一套启发式预取策略，而是把更多“早期可见的 PQ+ADC page candidates”交给现有 `PrefetchPlanner`。

### 4.2 统一触发接口

建议把当前语义较窄的提交入口抽象成通用接口：

```cpp
enum class PrefetchTrigger {
  WarmupEntry,
  PreBeamWindow,
  PostExpandLookahead,
  RerankLookahead,
};

void submit_ranked_prefetch(
    PrefetchTrigger trigger,
    const std::vector<std::uint32_t>& candidate_nodes,
    const std::unordered_set<std::uint32_t>& excluded_nodes,
    const PrefetchPlanner::SearchContext& context);
```

短期可以保留 `submit_next_hop_prefetch` 作为兼容包装，但 trace 和统计里应该记录真实 `trigger`。

---

## 5. 提前触发点

### T0：entry warmup 触发

**位置：** entry seeds 确定、ADC table 建好之后，正式 beam search 前。

**输入候选：**

```text
entry seed pages
entry seed 的 PQ 可见邻居页
```

**目的：**

为第一批 expansion 预热，避免 query 开头全部同步 demand。

**限流建议：**

```text
top_k = min(prefetch_top_k, entry_count)
max_inflight = min(configured_max_inflight, io_depth / 4)
reuse threshold >= 1
```

**风险：**

entry seeds 分散时可能产生 unused，所以只在 `cache_pages` 较小、entry_count 较高或历史 trace 显示首批 pending 多时启用。

---

### T1：pre-beam frontier window 触发

**位置：** 每轮 beam 选择前。

当前流程一般是先从 frontier/candidate heap 选出下一批 beam，然后 demand 读取这些 beam page。新的触发点应插在：

```text
candidate heap 已有 PQ+ADC 分数
  -> 取 top beam_window candidates
  -> 按 page 聚合并 XGBoost 打分
  -> 提交高分 page prefetch
  -> 再进入 demand batch load / expansion
```

**输入候选：**

```text
candidate heap top (beam_width * window_multiplier)
```

**推荐默认：**

```text
window_multiplier = 2
trigger_interval_batches = 1
top_k = 1 或 2
max_inflight = 1 到 2 起步
```

**为什么最重要：**

这些 page 已经进入 PQ+ADC frontier，但还没有被 demand 读取。它们比 expansion 后的 next-hop 更早，并且比 entry warmup 更准。

**验收重点：**

```text
T1 的 ready_hit_rate > PostExpandLookahead
T1 的 unused_rate 不高于 0.30
```

---

### T2：post-expand lookahead 触发

**位置：** 当前 expansion 完成后、下一轮 batch load 前。

这接近当前实现，但要调整目标：不是预取刚刚发现的所有邻居，而是预取“已经进入 frontier 且预计下一轮之后才 demand”的 page。

**输入候选：**

```text
new_neighbors
+ updated frontier top window
- current batch pages
- already materialized pages
```

**限流建议：**

```text
只提交 earliest_demand_step_estimate >= current_step + 1 的 page
如果 estimate == current_step，则不提交，因为已经太晚
```

**目的：**

补充 T1 漏掉的二跳或聚合后高复用 page。

---

### T3：rerank lookahead 触发

**位置：** PQ search 即将结束、exact rerank pool 已经形成但 full vector page 还未全部 materialize 时。

**输入候选：**

```text
rerank_topk candidate pages
```

**目的：**

把最终 exact rerank 的同步读变成异步读。

**限流建议：**

```text
top_k = min(prefetch_top_k, rerank_topk page count)
score threshold 可以低一些
max_inflight 不能抢占 demand I/O
```

**注意：**

如果 rerank pool 页面基本已经被搜索过程读过，T3 收益会很小。它只作为低风险补充，不作为 P0。

---

## 6. 调度与限流规则

所有触发点共用一个 scheduler，不允许各自绕过限流：

```text
if policy != xgboost:
    return
if score < threshold:
    skip_score_threshold
if page cached/materialized:
    skip_materialized
if page pending/ready:
    skip_pending
if inflight >= max_prefetch_inflight:
    skip_inflight_full
if io_queue_depth >= io_depth_guard:
    skip_io_depth_full
if reuse_count < min_candidates_per_page:
    skip_low_page_reuse
submit_async(page)
```

建议新增两个保护参数：

| 参数 | 默认 | 说明 |
|---|---:|---|
| `--prefetch-early-trigger` | `pre-beam` | `off/pre-beam/all` |
| `--prefetch-io-depth-guard-ratio` | `0.5` | 预取最多占用 `io_depth * ratio` |

如果先不加 CLI，也可以用内部默认：

```text
T1 enabled
T0/T2/T3 disabled
io_depth_guard_ratio = 0.5
```

---

## 7. Trace 字段补强

为了判断“提前触发是否真的提前”，trace 需要新增字段：

| 字段 | 含义 |
|---|---|
| `trigger` | `warmup_entry/pre_beam/post_expand/rerank` |
| `first_seen_step` | page 第一次成为候选的搜索步 |
| `submit_step` | page 被提交预取的搜索步 |
| `demand_step` | page 第一次被 demand 的搜索步 |
| `estimated_demand_step` | 提交时估计的 demand step |
| `lead_steps` | `demand_step - submit_step` |
| `submit_to_demand_us` | demand_time - submit_time |
| `ready_before_demand_us` | demand_time - ready_time，正数代表真正提前 |
| `skip_reason` | 未提交原因 |

核心验收不要只看 JSON 的 ready hit，而要以 trace 的：

```text
was_ready_before_demand
submit_to_demand_us
ready_before_demand_us
lead_steps
```

为准。

---

## 8. XGBoost 特征补强

提前触发后，模型需要知道“现在提交是否来得及”。建议补充以下 page-level 特征：

| 特征 | 含义 |
|---|---|
| `first_seen_step_delta` | 当前 step - page 第一次出现 step |
| `estimated_demand_step_delta` | 预计 demand step - 当前 step |
| `frontier_rank_min` | page 内候选在 frontier 中的最佳 rank |
| `frontier_rank_avg` | page 内候选平均 frontier rank |
| `beam_window_hit_count` | page 在 pre-beam window 中出现次数 |
| `trigger_id` | 触发点类别编码 |
| `recent_ready_hit_rate` | 最近窗口 ready hit 比率 |
| `recent_pending_hit_rate` | 最近窗口 pending hit 比率 |
| `prefetch_queue_occupancy` | 当前预取队列占用 |

标签仍保持 timeliness：

```text
3 = ready before demand
2 = used but ready very close to demand
1 = pending at demand
0 = unused / evicted before use / duplicate
```

---

## 9. 实施阶段

### P0：只做 T1 pre-beam 触发（已完成）

改动：

1. 在 beam selection 前取 frontier top window。
2. 调用统一 XGBoost planner。
3. trace 增加 `trigger=pre_beam`。
4. 默认 `--prefetch-early-trigger pre-beam`，并保留 `off/all` 用于对照。
5. 推荐在线实验先用 `top_k=1/max_inflight=1` 的保守限流。

验收：

```text
ready_hit_rate 上升
pending_hit_rate 下降
unused_rate <= 当前 xgboost
submitted_reads 增幅 <= 10%
```

当前 P0 小 smoke 观察：

```text
trace trigger = pre_beam
top_k=1/max_inflight=1 时 ready_hit_rate > pending_hit_rate
top_k=2/max_inflight=2 会重新拉高 pending 和尾延迟
```

### P1：加入 T2 post-expand lookahead

改动：

1. 当前 expansion 后保留 lookahead，但排除本轮马上 demand 的 page。
2. 引入 `estimated_demand_step_delta >= 1` 过滤。
3. trace 对比 `pre_beam` 与 `post_expand` 两个 trigger 的贡献。

验收：

```text
post_expand 不能拉高 overall waste_rate
pre_beam 仍是 ready hit 主来源
```

### P2：加入 T0 entry warmup

改动：

1. 在 ADC table 建好后，对 entry seed 相关 page 做极小预算 warmup。
2. 只对 query 开头 pending 明显的 case 打开。

验收：

```text
首批 demand pending 下降
unused_rate 不明显上升
```

### P3：加入 T3 rerank lookahead

改动：

1. rerank pool 形成后预取尚未 materialized 的 page。
2. 预取优先级低于 demand 和 T1。

验收：

```text
rerank_reads 同步等待下降
总体 io_submit_syscalls 不明显上升
```

---

## 10. 实验命令

### 训练真实 XGBoost

```bash
python3 scripts/train_prefetch_ranker.py \
  --trace build/prefetch_closed_loop_trace.csv \
  --model-out build/prefetch_xgboost_early.txt \
  --metrics-out build/prefetch_xgboost_early_metrics.json \
  --backend xgboost \
  --top-k 2 \
  --max-inflight 2
```

### 离线 replay

```bash
python3 scripts/replay_prefetch_policy.py \
  --trace build/prefetch_closed_loop_trace.csv \
  --model build/prefetch_xgboost_early.txt \
  --top-k 1 2 4 \
  --out-json build/prefetch_xgboost_early_replay.json \
  --out-md build/prefetch_xgboost_early_replay.md
```

### 在线 A/B

```bash
python3 scripts/run_prefetch_ab.py \
  --binary ./build/agent_aware_flow \
  --model build/prefetch_xgboost_early.txt \
  --out-dir build/prefetch_ab_early \
  --search-width 32 \
  --beam-width 4 \
  --entry-count 8 \
  --prefetch-top-k 1 \
  --prefetch-max-inflight 1 \
  --query-limit 100 \
  --base-limit 20000 \
  --extra-args "--synthetic 1 --synthetic-dim 16 --synthetic-clusters 4 --graph-degree 8 --pq-subspaces 4 --pq-centroids 16 --pq-train-limit 512 --pq-iterations 2 --rerank-topk 20 --io-depth 8 --io-batch 4 --cache-pages 4"
```

---

## 11. 成功标准

在同一数据集、同一索引、同一模型训练流程下：

| 指标 | 当前问题 | 成功标准 |
|---|---|---|
| `ready_hit_rate` | 低于或接近 pending | 提升 30% 以上 |
| `pending_hit_rate` | 高于 ready | 低于 ready，或至少下降 20% |
| `waste_rate` | 约 0.27-0.32 | 不高于当前 |
| `submitted_reads` | 预取放大 | 相比 no-prefetch 增幅 <= 10%-15% |
| `p95_latency_ms` | 小样本不稳 | 多次 repeat 中中位数下降 |
| `recall@10` | 不允许退化 | 不下降 |

如果出现：

```text
ready_hit_rate 上升，但 p95 不降
```

优先检查：

1. `io_depth_guard_ratio` 是否过高，预取抢了 demand。
2. `prefetch_top_k/max_inflight` 是否过大。
3. `unused_rate` 是否同步上升。
4. `submit_to_demand_us` 是否只是变大，但 ready 仍晚于 demand。

---

## 12. 推荐先做的最小改动

第一版只实现：

```text
T1 pre-beam frontier window trigger
+ trace trigger/lead_steps 字段
+ top_k=1, max_inflight=1 保守限流
```

理由：

1. T1 最接近“已经被 PQ+ADC 证明可能访问，但尚未 demand”的理想时刻。
2. 代码改动小，不需要重写 search loop。
3. 它能直接验证“发得太晚”是否真是主瓶颈。
4. 如果 T1 仍不能提高 ready-before-demand，说明瓶颈不在触发点，而在 I/O 队列、page cache 或 batch 调度。
