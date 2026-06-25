# 学习型预取排序路线计划

---

## 1. 核心路线

整体路线是：

```text
PQ+ADC 候选生成
  -> page-level / node-level 候选聚合
  -> 构造排序特征
  -> Learning-to-Rank 预取收益排序
  -> top-K / threshold / io_depth 限流
  -> async prefetch
  -> ready_hit / pending_hit / unused 体系评估
```

一句话：

```text
把预取点选择建模为搜索状态下的 page-level ranking 问题。
```

模型回答的问题不是：

```text
哪个 node 离 query 最近？
```

而是：

```text
当前搜索状态下，哪些 page/node 现在最值得预取？
```

---

## 2. 与当前策略的关系

策略：

```text
当前节点 cn
  -> 取 cn 的图邻居
  -> PQ ADC 估算距离
  -> 生成候选池
  -> 聚合 candidate pages
  -> 为每个 page 构造特征
  -> ranking model 输出 prefetch_score
  -> 按 score 排序
  -> 经过 threshold / top-K / io_depth / cache_pressure 限流
  -> submit_async_batch()
```

---

## 3. 为什么第一阶段直接做 Ranking

预取本质不是单点分类，而是在每个 search step 的一组候选里选出最值得提交 I/O 的前几个。

因此最贴合的形式是：

```text
group = 一个 query 在一个 search step 产生的所有 candidate pages
ranker = 对 group 内 page 按预取收益排序
```

不采用三套 XGBoost 版本：

- 不单独做 binary classifier。
- 不单独做 regression scorer。
- 不做 classifier / regression / ranking 三线并行。

第一版直接做：

```text
Learning-to-Rank
```

---

## 4. 候选单位

优先使用 page-level。

当前项目中一个 node record 是 4KB，因此短期可以认为：

```text
page_id ~= node_id
```

但接口仍按 page-level 设计：

```text
(query_id, step_id, page_id) -> prefetch_score
```

---

## 5. Ranking 样本定义

每一行样本代表一个候选 page：

```text
query_id
step_id
page_id
features...
label
group_id = query_id + step_id
```

每个 `group_id` 内包含同一个搜索时刻的所有候选 page。

模型训练目标：

```text
在每个 group 内，把真正有预取收益的 page 排到前面。
```

---

## 6. 标签设计

标签使用收益等级，而不是简单的 0/1。

推荐第一版标签：

```text
3 = demand 到来前已经 ready，且被使用，收益最高
2 = 后续被使用，但 ready 时间接近 demand，收益中等
1 = 后续被使用，但 prefetch 太晚，只形成 pending hit
0 = 未使用、重复提交、已缓存仍提交、或造成明显 cache pollution
```

更具体的生成逻辑：

```text
if was_used && ready_time <= demand_time:
    label = 3
elif was_used && ready_time > demand_time && ready_time - demand_time <= small_delta:
    label = 2
elif was_used:
    label = 1
else:
    label = 0
```

如果能统计 cache pollution，可以进一步降级：

```text
if evicted_before_use:
    label = 0

if caused_hot_page_eviction:
    label = 0
```

关键原则：

```text
标签必须体现 timeliness。
```

不能只判断：

```text
这个 page 后面有没有被访问
```

因为预取真正要优化的是：

```text
是否在 demand 到来前完成，并产生净收益。
```

---

## 7. 特征路线

### 7.1 PQ / query-aware 特征

这些特征保留 PQ+ADC 的几何相似度信号：

| 特征 | 含义 |
|---|---|
| `min_pq_distance_on_page` | page 内候选最小 PQ ADC 距离 |
| `avg_pq_distance_on_page` | page 内候选平均 PQ ADC 距离 |
| `min_pq_rank_on_page` | page 内最佳候选 PQ rank |
| `avg_pq_rank_on_page` | page 内平均 PQ rank |
| `pq_distance_to_worst_ratio` | `min_pq_distance / worst_result_distance` |
| `candidate_count_after_pq_filter` | PQ 筛选后的候选数量 |

### 7.2 搜索状态特征

| 特征 | 含义 |
|---|---|
| `search_step` | 当前 query 搜索步数 |
| `visited_count` | 已访问节点数 |
| `frontier_size` | candidate heap 大小 |
| `result_size` | 当前结果集大小 |
| `current_candidate_distance` | 当前弹出节点距离 |
| `worst_result_distance` | 当前 result heap 最差距离 |
| `ef_search` | 搜索宽度 |
| `beam_width` | 当前 batch 宽度 |

### 7.3 Page 聚合特征

| 特征 | 含义 |
|---|---|
| `num_candidates_on_page` | page 上候选节点数量 |
| `num_top_beam_candidates_on_page` | page 上 top beam 候选数量 |
| `contains_top1_candidate` | 是否包含 PQ 排名第一候选 |
| `contains_topk_candidate` | 是否包含 PQ top-k 候选 |
| `candidate_rank_span` | page 内候选 rank 范围 |

### 7.4 图结构特征

| 特征 | 含义 |
|---|---|
| `node_degree_avg` | page 内候选平均出度 |
| `node_degree_max` | page 内候选最大出度 |
| `node_in_degree_avg` | page 内候选平均入度 |
| `is_neighbor_of_current_node` | 是否当前扩展节点的一跳邻居 |
| `is_second_hop_candidate` | 是否二跳候选 |
| `is_hub_node` | 是否高入度 hub |

### 7.5 缓存 / I/O 特征

| 特征 | 含义 |
|---|---|
| `is_cached` | page 是否已在 BufferPool |
| `is_inflight` | page 是否已有 pending async read |
| `cache_hit_rate_recent` | 最近窗口 cache hit rate |
| `page_recent_access_count` | page 最近访问次数 |
| `page_global_hotness` | page 全局热度 |
| `io_queue_depth` | 当前 io_uring pending 深度 |
| `pending_reads` | 当前 pending async reads 数 |
| `estimated_io_latency_us` | 估计 I/O 延迟 |
| `cache_pressure` | BufferPool 压力 |

---

## 8. Trace 采集路线

第一阶段先完成 trace，而不是直接接模型。

需要记录：

```text
query_id
step_id
group_id
candidate_node_id
candidate_page_id
pq_distance
pq_rank
beam_rank
current_distance
worst_result_distance
visited_count
frontier_size
is_cached_at_decision
is_inflight_at_decision
io_queue_depth
cache_pressure
prefetch_decision_time
was_prefetched
prefetch_submit_time
prefetch_ready_time
demand_time
was_demanded
was_ready_before_demand
was_evicted_before_use
sync_fallback_used
```

输出格式：

```text
CSV 第一版即可；
数据量变大后可切换 Parquet。
```

采集位置：

- 候选生成处：`src/benchmark.cpp` 的 `diskann_search_enhanced()`。
- 异步提交处：`DiskIndexReader::submit_async_batch()`。
- completion 处：`DiskIndexReader::wait_async_batch()`。
- demand 读取处：`DiskIndexReader::compute_distance_direct()`。

---

## 9. 离线训练与 Replay 路线

当前闭环只保留 XGBoost 路线：

```text
C++ PQ+ADC benchmark 采 trace
  -> Python 构造 group-wise ranking dataset
  -> 训练 XGBoost text-dump ranker
  -> 离线 replay xgboost 策略和 oracle 上界
  -> no-prefetch / xgboost 在线 A-B
```

训练目标：

```text
rank:ndcg
```

group：

```text
query_id + step_id
```

离线 replay 做两件事：

1. 在每个 group 内按模型分数选择 top-K。
2. 用 trace 中的真实未来访问结果估计 ready hit、pending hit、unused。

离线 replay 的目标不是精确预测最终 QPS，而是判断：

```text
xgboost 排序与 oracle 上界的差距，以及 top-K / threshold / io_depth 限流是否过紧或过松。
```

---

## 10. 在线接入路线

C++ 在线路径已经接入 XGBoost text-dump 推理。`PrefetchPlanner` 的职责是：

```text
PQ+ADC 候选页聚合
page-level feature 构造
XGBoost text-dump score(feature)
score threshold
top-K
max inflight reads
cache pressure guard
duplicate / inflight suppression
async submit
```

在线提交条件：

```text
score >= score_threshold
rank <= prefetch_top_k
!is_cached
!is_inflight
io_inflight < max_prefetch_inflight
cache_pressure <= max_cache_pressure
```

伪代码：

```cpp
auto pages = aggregate_candidate_pages(pq_adc_candidates);

for (auto& page : pages) {
    auto feature = build_prefetch_feature(query_state, page, cache_state, io_state);
    page.score = xgboost_ranker.score(feature);
}

std::sort(pages.begin(), pages.end(), by_score_desc);

for (auto& page : pages) {
    if (page.score < score_threshold) break;
    if (submitted >= prefetch_top_k) break;
    if (io_inflight >= max_prefetch_inflight) break;
    if (page.is_cached || page.is_inflight) continue;
    if (cache_pressure_high && page.score < high_confidence_threshold) continue;

    submit_prefetch(page);
    submitted++;
}
```

---

## 11. 命令行开关

当前保留的开关：

```bash
--enable-pq 1
--prefetch-policy none
--prefetch-policy xgboost
--prefetch-model <path>
--prefetch-top-k <n>
--prefetch-score-threshold <float>
--prefetch-max-inflight <n>
--prefetch-trace <path>
--prefetch-stats
```

默认行为：

```text
--enable-pq 1
--prefetch-policy xgboost
```

非 PQ 搜索候选路径已放弃；`xgboost` 策略需要显式提供 `--prefetch-model`。

---

## 12. 评价体系

### 12.1 离线 Ranking 指标

| 指标 | 含义 |
|---|---|
| `NDCG@K` | 排序质量 |
| `Precision@K` | top-K 中高收益 page 占比 |
| `Recall@K` | 高收益 page 被 top-K 覆盖比例 |
| `MRR` | 第一个高收益 page 的排名 |
| feature importance | 判断模型是否依赖合理特征 |

### 12.2 在线 Prefetch 指标

| 指标 | 公式 | 期望 |
|---|---|---|
| `ready_hit_rate` | `prefetch_ready_hit / prefetch_submitted` | 上升 |
| `pending_hit_rate` | `prefetch_pending_hit / prefetch_submitted` | 下降 |
| `unused_rate` | `prefetch_unused / prefetch_submitted` | 下降 |
| `demand_saved_rate` | `prefetch_ready_hit / demand_reads` | 上升 |
| `read_amplification` | `submitted_reads / demand_reads` | 不明显上升 |
| `evicted_before_use_rate` | `prefetch_evicted_before_use / prefetch_submitted` | 下降 |
| `duplicate_submit_rate` | `duplicate_pending / prefetch_submitted` | 接近 0 |

### 12.3 在线系统指标

| 指标 | 期望 |
|---|---|
| Search time | 下降 |
| QPS | 上升 |
| Avg latency | 下降 |
| P95 / P99 / P99.9 | 下降 |
| Recall@10 | 不下降 |
| submitted_reads | 不明显膨胀 |
| model inference time | 小于节省的 I/O 等待时间 |
| CPU overhead | 可控 |

---

## 13. Baseline 与对照

必须比较：

| 策略 | 说明 |
|---|---|
| No prefetch | `--prefetch-policy none` |
| XGBoost prefetch | PQ+ADC 候选页聚合 + XGBoost 收益打分 + 在线限流 |
| Oracle ranking | 离线知道未来访问，用于估计上界 |

不再保留非 PQ、current、learned 线性策略作为预取路线。

---

## 14. Ablation

为证明 ranking 的收益来源，做以下消融：

| 实验 | 目的 |
|---|---|
| 去掉 PQ 特征 | 判断 PQ+ADC 是否仍是主信号 |
| 去掉 cache/I/O 特征 | 判断系统状态是否带来收益 |
| 去掉 page 聚合特征 | 判断 page-level 聚合价值 |
| xgboost + no throttle | 判断限流是否必要 |
| xgboost + throttle | 完整策略 |

预期更稳的结果：

```text
xgboost + throttle
  > xgboost + no throttle
  > no prefetch
```

---

## 15. 参数矩阵

| 参数 | 建议取值 |
|---|---|
| dataset size | 1M |
| query count | 1k |
| beam_width | 8、16、32 |
| threads | 1、4、8 |
| prefetch_top_k | 4、8、16、32 |
| score threshold | 低、中、高三档 |
| max inflight | 32、64、128、256 |

---

## 16. 实施阶段

### 当前落地状态（2026-06）

已完成：

1. `PrefetchPlanner` 已重写为 PQ+ADC page-level ranking pipeline：候选聚合、特征构造、XGBoost text-dump ranker 打分、top-K / score threshold / max inflight throttle、再提交 async prefetch。
2. `QueryPageSession` 已记录 `prefetch_ready_hit`、`prefetch_pending_hit`、`prefetch_unused`（JSON 中同时保留 `prefetch_only_pages`）、`prefetch_evicted_before_use`、threshold / inflight skip 等预取统计。
3. `agent-aware` 已收敛到 `--prefetch-policy none|xgboost`，并支持 `--prefetch-model`、`--prefetch-top-k`、`--prefetch-score-threshold`、`--prefetch-max-inflight`、`--prefetch-trace`。
4. `--prefetch-trace <path>` 已输出 group-wise CSV，并在 query 结束时补齐 `was_prefetched`、`prefetch_submit_time`、`prefetch_ready_time`、`demand_time`、`was_ready_before_demand`、`label`。

补充说明：

1. `scripts/train_prefetch_ranker.py` 已支持真实 `xgboost` 包训练；缺少包时会导出兼容 text-dump 格式的 boosted-stump 模型，保证闭环 smoke 可运行。
2. `scripts/replay_prefetch_policy.py` 已按 XGBoost 模型和 oracle 上界 replay，输出 ready/pending/unused 体系指标。
3. `scripts/run_prefetch_closed_loop_smoke.sh` 已提供 PQ+ADC trace -> replay/train -> xgboost online 的闭环 smoke。
4. `candidate_node_id` 在当前 trace 中保持空列，训练第一版以 page-level 特征为主。

### P0：补齐观测指标

目标：

```text
先能判断每个预取是否真正有用。
```

任务：

1. 记录 `prefetch_ready_hit`。
2. 记录 `prefetch_pending_hit`。
3. 记录 `prefetch_unused`。
4. 记录 `prefetch_evicted_before_use`。
5. 记录 `demand_reads` / `extra_reads`。
6. 记录 `duplicate_submit` / `inflight_hit`。

验收：

```text
benchmark 能输出完整 prefetch stats。
```

### P1：Trace 数据集

目标：

```text
从 benchmark run 中生成 ranking 训练数据。
```

任务：

1. 增加 `--prefetch-trace <path>`。
2. 输出 group-wise CSV。
3. 按 query_id 切分 train / validation / test。
4. 生成 label 0-3。

验收：

```text
能生成可训练的 ranking dataset。
```

### P2：离线 Ranking 模型

目标：

```text
证明 xgboost ranking 能接近 oracle 上界，并能降低 pending/unused 或改善 ready-before-demand。
```

任务：

1. 写 Python 训练脚本。
2. 优先使用 `xgboost` 包训练；无依赖环境使用兼容 text-dump boosted-stump fallback。
3. 输出 NDCG@K、Precision@K、Recall@K。
4. 做离线 replay。

验收：

```text
xgboost replay 与 oracle gap 可解释；
predicted unused_rate / pending_rate 不高于 no-throttle 版本。
```

### P3：C++ 在线接入

目标：

```text
让 benchmark 只保留 PQ+ADC + xgboost prefetch policy。
```

任务：

1. 增加 XGBoost text-dump 推理器。
2. 删除 non-PQ 候选生成搜索路线。
3. 删除 `current` / `learned` 线性策略入口。
4. 保留 top-K / threshold / io_depth / max inflight 限流。
5. 增加命令行策略校验。

验收：

```text
--prefetch-policy xgboost 可以跑完 benchmark；
--prefetch-policy current/learned 会报错；
--enable-pq 0 会报错；
Recall@10 不下降。
```

### P4：在线 A/B

目标：

```text
证明 xgboost ranking 在真实搜索中带来收益。
```

比较：

```text
no-prefetch vs xgboost，并用 oracle replay 估计上界
```

验收：

```text
ready_hit_rate 上升；
unused_rate 下降；
P95/P99 下降；
submitted_reads 不明显增加；
Recall@10 不下降。
```

### P5：模型路线扩展

只有当 XGBoost ranking 任务已经验证有效后，再考虑替换模型：

```text
更深的 XGBoost / LightGBM / LambdaMART
  -> 小型 MLP ranker
  -> 序列模型
  -> GNN ranker
```

扩展原则：

```text
任务定义不变，只替换 ranker 实现。
```

---

## 17. 风险与约束

| 风险 | 说明 | 缓解 |
|---|---|---|
| 标签只学到 future access | 没体现 ready-before-demand | 标签必须包含 timeliness |
| 模型推理抵消收益 | CPU 开销过高 | 限制候选池；小模型；批量推理 |
| cache pollution | 高分但低复用 page 污染 BufferPool | 加 cache pressure 和 unused 反馈 |
| 数据泄漏 | 同一 query step 泄漏到训练和测试 | 按 query_id 切分 |
| 只适配小数据 | 20k 有效但 1M 失效 | 多规模矩阵验证 |
| 在线收益低于离线 | replay 不能完全模拟调度 | 必须做在线 A/B |

---

## 18. 最小可行闭环

MVP：

```text
1. 增加 prefetch trace
2. 生成 group-wise ranking dataset
3. 训练一个 XGBoost text-dump 模型
4. 离线 replay xgboost vs oracle
5. 在线跑 no-prefetch vs xgboost A-B
```

MVP 成功标准：

```text
NDCG@K / Precision@K 可解释
xgboost 与 oracle gap 小
predicted unused_rate 可控
ready_before_demand 高于 pending_at_demand
```
