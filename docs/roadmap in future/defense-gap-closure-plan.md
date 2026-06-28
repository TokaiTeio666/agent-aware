# 答辩前关键缺口补强计划

> 关联文档：主计划 `PROJECT_PLAN.md` §8.1 P9、I/O 尾延迟 `docs/roadmap/recall-to-io-tail-latency-plan.md`、预取排序 `docs/design/XGBOOST_PREFETCH_PLAN.md`
>
> 最后更新：2026-06-25

## 当前背景

截至 2026-06-25，项目已经具备 SIFT1M SSD packed Vamana 主路径、Graph-Aware 2Q cache、io_uring fallback、WAL/MemTable/SSTable 动态写入、immutable read view、XGBoost 学习型预取排序、early trigger 提前触发、mixed RW benchmark 和动态 Recall evidence。当前最需要补强的不是再铺很多新功能，而是把结果从”单点可展示”推进到”曲线可解释、环境可信、大 delta 不退化”。

本计划聚焦三件最需要补的事：

1. 补 `search_width / beam_width / cache_ratio / io_mode` 参数矩阵，输出 Recall、QPS、P95、I/O 次数曲线。
2. 在真实 NVMe 或比赛环境复跑 SIFT1M 主路径和 mixed RW，降低 WSL/虚拟化结果被质疑的风险。
3. 将 delta linear scan 升级为 delta memory graph 或分块 immutable delta，解决写入规模变大后的查询退化。

## 总体优先级

| 优先级 | 任务 | 目标 | 主要产物 |
| --- | --- | --- | --- |
| P0 | 参数矩阵和曲线 | 证明性能不是单点调参结果 | `logs/sift_bench/matrix_*/*.json`、CSV 汇总、Recall/QPS/P95/I/O 曲线 |
| P0 | 真实 NVMe/比赛环境复测 | 证明结果不依赖 WSL 或虚拟化 | 环境记录、三轮复现实验、主路径和 mixed RW JSON |
| P1 | delta 增长压力测试 | 找到 linear scan 的退化阈值 | delta 1k/10k/50k/100k 对照表 |
| P1/P2 | delta memory graph 或分块 immutable delta | 降低大 delta 下 `delta_search_ms` 和 read P95 | 新增实现、单测、mixed RW 对照实验 |

> 注：XGBoost 预取排序和 early trigger 提前触发已落地闭环（详见 `docs/design/XGBOOST_PREFETCH_PLAN.md` 和 `docs/design/07-prefetch-early-trigger-plan.md`），本计划不再重复覆盖预取路线。

## Phase 1：参数矩阵与曲线

### 目标

把当前 SIFT1M 主路径从单点结果扩展为可解释的参数曲线，回答评审最可能追问的四个问题：

| 问题 | 要用什么回答 |
| --- | --- |
| Recall 是否靠过大的 `search_width` 堆出来 | `search_width -> Recall/P95/I/O` 曲线 |
| `beam_width` 是否真的提升 I/O batching | `beam_width -> QPS/P95/page_reads` 曲线 |
| cache 是否在 10%-20% 内仍有效 | `cache_ratio -> hit_rate/P95` 曲线 |
| `io_uring` 是否比 `pread/odirect` 更好 | `io_mode -> QPS/P95/page_read_wait` 对照 |

### 推荐矩阵

不要一开始跑完整笛卡尔积。先按 staged matrix 控制时间，再对关键组合补三轮复现。

| 阶段 | 固定项 | 扫描项 | 产物 |
| --- | --- | --- | --- |
| M1 search sweep | `beam_width=16`、`cache_ratio=0.20`、`io_mode=io_uring` | `search_width=128/192/256/350/512` | Recall/P95/I/O 拐点 |
| M2 beam sweep | M1 选出的 2 个 `search_width` | `beam_width=8/16/32` | beam 对 QPS 与尾延迟影响 |
| M3 cache sweep | 最优 `search_width/beam_width`、`io_uring` | `cache_ratio=0.10/0.15/0.20` | 内存约束内的收益 |
| M4 I/O sweep | 最优搜索与 cache | `io_mode=pread/odirect/io_uring` | 真实 I/O 策略对照 |
| M5 confirm | 最终 2 组候选参数 | 每组跑 3 次 | 稳定性和方差 |

### 主路径命令模板

`agent-aware` 支持 `--search-width`、`--beam-width`、`--memory-budget-ratio`、`--io-mode`、`--cache-policy` 和 `--output-json`。复用已有 packed index 时保持 `REBUILD_INDEX=0` 或不设置。

```bash
for sw in 128 192 256 350 512; do
  TAG="matrix_sw${sw}_bw16_cache020_iouring"
  EXTRA_ARGS="--query-limit 1000 \
    --search-width ${sw} \
    --beam-width 16 \
    --memory-budget-ratio 0.20 \
    --io-mode io_uring \
    --cache-policy graph-aware-2q" \
  TAG="$TAG" bash scripts/linux/run_sift1m_once.sh
done
```

### mixed RW 命令模板

参数矩阵的主结论以纯搜索为主，mixed RW 只复跑最终候选参数，避免实验量爆炸。

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --dynamic_dir build/matrix_mixed_dynamic \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 1 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --recall_sample_rate 0 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --topk 10 \
  --search_width 256 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/matrix_mixed_sw256_bw16_iouring.json
```

### 必须汇总的字段

| 类别 | 字段 |
| --- | --- |
| 参数 | `search_width`、`beam_width`、`memory_budget_ratio/cache_pages`、`io_mode`、`cache_policy` |
| 准确性 | `recall_at_k` 或 `recall_at_10`、`recall_queries/samples` |
| 吞吐延迟 | `qps/read_qps`、`latency_p95_ms/read_p95_ms`、`latency_p99_ms/read_p99_ms` |
| I/O | `node_reads`、`page_read_wait_*_ms`、`graph_reads_per_read`、`cache_hit_rate` |
| 复现 | `git_commit`、`dirty_status`、`command.txt`、环境信息 |

### 验收标准

| 项目 | 标准 |
| --- | --- |
| 曲线完整 | 至少有 Recall、QPS、P95、I/O 次数四张图 |
| 参数可解释 | 能指出最终参数为何不是盲选 |
| 三轮稳定 | 最终候选三轮 Recall 波动 `<= 0.005`，P95 无数量级跳变 |
| 文档更新 | 将最终推荐参数写回 `docs/experiments/param-tuning-and-sift-scale-test.md` |

## Phase 2：真实 NVMe/比赛环境复测

### 目标

把 WSL/虚拟化环境中的结果降级为“开发验证”，把正式结论建立在真实 NVMe 或比赛环境上。

### 环境要求

| 项目 | 要求 |
| --- | --- |
| 文件系统 | 原生 Linux ext4/xfs，避免 `/mnt/c`、`/mnt/d`、网络盘 |
| 磁盘 | 真实 NVMe SSD，记录型号和容量 |
| 构建 | Release build，记录 compiler、CMake、liburing 是否可用 |
| 数据 | SIFT1M base/query/groundtruth 与 index 路径固定 |
| 运行 | 每个关键 case 跑 3 次，保留 stdout/stderr/result JSON |

### 必跑 case

| case | 命令来源 | 目标 |
| --- | --- | --- |
| SIFT1M main path | `scripts/linux/run_sift1m_once.sh` | Recall、QPS、P95、resident ratio |
| pure read t1/t4/t8 | `bench_mixed_rw --write_threads 0` | 读并发扩展性 |
| mixed no compaction | `bench_mixed_rw --enable_compaction 0` | 写入对读路径影响 |
| mixed background compaction | `bench_mixed_rw --enable_compaction 1 --compaction_background 1` | 后台整理抖动 |
| dynamic recall evidence | `--recall_sample_rate > 0` | 动态可见性正确性 |

### 环境记录模板

每轮实验目录必须包含：

```text
command.txt
result.json
stdout.json 或 stdout.log
stderr.log
time.txt
env.txt
```

`env.txt` 建议记录：

```bash
date
uname -a
lsblk -o NAME,MODEL,SIZE,ROTA,MOUNTPOINT
lscpu
free -h
g++ --version
cmake --version
git rev-parse --short HEAD
git status --short
```

### 验收标准

| 项目 | 标准 |
| --- | --- |
| 主路径可信 | SIFT1M Recall@10 达标，resident ratio 在预算内 |
| mixed RW 可信 | read/write QPS、P95/P99、compaction 指标完整 |
| 环境可审计 | 每个结果能追溯命令、commit、磁盘和系统信息 |
| 结果可说明 | 如果 NVMe 与 WSL 差异明显，文档中解释差异来源 |

## Phase 3：Delta 增长压力测试

### 目标

先量化 linear scan 何时成为瓶颈，再决定是否必须上 delta memory graph。不要在没有退化曲线前直接改算法。

### 测试矩阵

| delta_count | 目的 |
| ---: | --- |
| 1k | 当前小 delta 基线 |
| 10k | 常规写入压力 |
| 50k | 约 5% SIFT1M 更新量，接近必须优化阈值 |
| 100k | 极端压力，用于证明退化趋势 |

每组记录：

```text
read_qps
read_p95_ms
read_p99_ms
delta_search_avg/p95/p99_ms
merge_avg/p95/p99_ms
latest_record_lookup_p95_ms
graph_reads_per_read
cache_hit_rate
```

### 判断标准

| 结果 | 结论 |
| --- | --- |
| 50k 以内 P95 可接受 | linear scan 可以作为当前提交版，报告明确适用范围 |
| 50k 后 `delta_search_p95_ms` 接近 base search | 需要分块 immutable delta |
| 50k 后 read P95 超过小 delta 2 倍 | 需要 delta memory graph 或 rebuild policy |
| 100k 查询不可接受 | 必须给出 merge/rebuild 触发阈值 |

## Phase 4：Delta 查询优化路线

### 方案 A：分块 immutable delta

这是较低风险方案，适合先做。

核心思路：

```text
MemTable/recent writes
  -> 小规模线性扫描

Immutable delta partitions
  -> 每块保存 centroid / min-max sequence / record count
  -> 查询时先选 top-M partitions
  -> 只扫描候选 partitions
```

任务拆分：

| 任务 | 内容 | 验收 |
| --- | --- | --- |
| A1 metadata | 为 SSTable/immutable chunk 增加 centroid、record_count、sequence range | recovery 后 metadata 可加载 |
| A2 partition select | 查询时按 centroid 距离选择 top-M chunks | 输出 scanned_partition_count |
| A3 bounded scan | 只扫描 active chunks + top-M immutable chunks | `delta_search_ms` 随 delta 增长变缓 |
| A4 recall guard | 对比 full delta scan 的 Recall | Recall 不明显下降 |

优点：改动小，能直接缓解 immutable delta 规模增长。缺点是 recall 依赖 partition summary，可能需要调 `top_m_partitions`。

### 方案 B：Delta memory graph

这是更强方案，适合在 A 之后或时间充足时做。

核心思路：

```text
new write
  -> WAL/MemTable
  -> append DeltaGraphIndex
  -> query searches base graph + delta graph
  -> merge topK
```

建议接口：

```cpp
class DeltaGraphIndex {
public:
    void insert(const DynamicRecord& record);
    std::vector<DynamicRecord> search_l2(
        const float* query,
        std::uint32_t dim,
        std::size_t topk,
        std::size_t search_width) const;
    std::size_t size() const;
};
```

任务拆分：

| 任务 | 内容 | 验收 |
| --- | --- | --- |
| B1 graph container | 保存 delta node、vector、neighbors、deleted/version 信息 | 插入和 get 单测通过 |
| B2 incremental insert | 为新点找候选邻居，维护有限出度 | 小规模 recall 不低于 linear scan 太多 |
| B3 graph search | 用 `delta_search_width` 搜索 delta graph | 10k/50k delta P95 优于 linear scan |
| B4 snapshot consistency | 与 `read_sequence` 和 immutable view 对齐 | mixed RW 下无可见性回退 |
| B5 fallback | graph 不可用或 delta 很小时回退 linear scan | 小 delta 不引入额外开销 |

### 推荐决策

| 时间 | 推荐选择 |
| --- | --- |
| 1 天以内 | 只完成 Phase 3 压力测试，文档说明退化阈值 |
| 2-3 天 | 优先做方案 A：分块 immutable delta |
| 4 天以上 | 做方案 B：Delta memory graph，并保留方案 A/rebuild 作为兜底 |

## 最终交付物

| 交付物 | 文件或目录 |
| --- | --- |
| 参数矩阵原始结果 | `logs/sift_bench/matrix_*`、`build/matrix_*.json` |
| 参数矩阵汇总 | `docs/experiments/param-tuning-and-sift-scale-test.md` |
| 真实 NVMe 复测报告 | `docs/experiments/sift1m-mixed-rw-immutable-view.md` 增补章节 |
| delta 压力测试报告 | `docs/experiments/high-concurrency-mixed-rw.md` 增补章节 |
| delta 优化设计更新 | `docs/design/fresh-streaming-ann.md` |
| 答辩清单更新 | `docs/competition/scoring-and-defense.md` |

## 一页结论

答辩前最值得投入的顺序是：

```text
先补参数矩阵曲线
再到真实 NVMe/比赛环境复测
然后用 delta 压力测试证明 linear scan 的边界
最后按时间选择分块 immutable delta 或 delta memory graph
```

这样补齐后，项目能够同时回答性能、可信环境、动态写入退化三个高风险问题。
