# agent-aware

## 当前版本摘要（2026-06-21）

本版本已经完成 SIFT1M SSD 主图上的高并发混合读写闭环，并完成一次关键读路径优化：`DynamicWriteManager` 的读侧 `snapshot/latest_record(s)/search_delta_l2_at` 已改为 immutable read view 发布模型。Reader 通过 atomic `shared_ptr<const DynamicReadView>` 读取动态层快照，不再在查询 base 覆盖记录时和 writer、flush、compaction 抢全局 `mutex_`。

当前可展示能力：

| 能力 | 当前状态 |
| --- | --- |
| SIFT1M SSD packed Vamana 主图 | 已构建并可复用 `indexes/sift1m_vamana_pq100_p4096_sm.idx` |
| Recall@10 | SSD 主路径 `0.9940`；动态 Recall evidence `1.0` |
| 内存约束 | SIFT1M 主路径 resident ratio `0.199992` |
| 高并发纯读 | 8 reader 下 `56.55 read_qps`，P95 `178 ms` |
| 混合读写 no compaction | 4 reader + 1 writer 下 `23.50 read_qps`、`217.05 write_qps`、P95 `167 ms` |
| 混合读写 compaction | 4 reader + 1 writer + 后台 compaction 下 `28.86 read_qps`、`221.33 write_qps`、P95 `178 ms` |
| 动态层一致性 | WAL/MemTable/SSTable/manifest/compaction/recovery 均通过测试 |
| 读路径诊断 | JSON 输出 `base_search/latest_record_lookup/delta_search/merge/exact_recall` 分段耗时 |

关键结果文件：

| 文件 | 用途 |
| --- | --- |
| `logs/sift_bench/codex_sift1m_once_20260620-022610/result.json` | SIFT1M SSD 主路径 Recall/QPS/内存比例结果 |
| `build/sift1m_readonly_t1.json` ~ `build/sift1m_readonly_t8.json` | 纯读扩展性 baseline |
| `build/sift1m_mixed_rw_no_recall_no_compaction_immutable_view.json` | 混合读写主路径，不含 Recall exact 回算，不开 compaction |
| `build/sift1m_mixed_rw_no_recall_compaction_immutable_view.json` | 混合读写主路径，不含 Recall exact 回算，开启后台 compaction |
| `build/sift1m_dynamic_recall_immutable_view.json` | 动态 Recall 单独验证实验 |

更完整的版本说明与结果分析见：

- `docs/changelog.md`
- `docs/experiments/sift1m-mixed-rw-immutable-view.md`

## 面向赛事评分的阅读顺序

赛题评审由性能指标、创新性、代码质量、文档完整性四项组成，各占 25%。本仓库推荐按下面顺序阅读，先看约束和证据，再进入实现细节。

| 顺序 | 文档 | 用途 |
| ---: | --- | --- |
| 1 | `docs/competition/problem.md` | 赛题原始要求、内存约束、Recall 要求和 4 项评分权重 |
| 2 | `PROJECT_PLAN.md` | 项目总计划、系统架构、关键模块和验收标准 |
| 3 | `docs/competition/scoring-and-defense.md` | 90 分目标拆解、评分证据矩阵、答辩材料组织 |
| 4 | `docs/experiments/sift1m-mixed-rw-immutable-view.md` | SIFT1M 主结果、混合读写、读路径优化前后对比 |
| 5 | `docs/changelog.md` | 当前版本能力边界、关键改动、已验证命令和遗留工作 |
| 6 | `docs/README.md` | 全部阶段计划、结果报告和下一步路线索引 |

## 文档导航

| 文档 | 用途 |
| --- | --- |
| `PROJECT_PLAN.md` | 项目总计划、架构路线、验收目标 |
| `docs/README.md` | 阶段计划、结果报告和下一步工作的统一索引 |
| `docs/design/ssd-storage-path.md` | SSD 4KB record、磁盘索引读写、O_DIRECT 链路设计 |
| `docs/design/cache-zero-copy.md` | 缓存、同页复用、direct distance、零拷贝优化计划 |
| `docs/design/async-prefetch.md` | io_uring、异步 page 读取和拓扑预取计划 |
| `docs/design/beam-width-io-uring.md` | `beam_width` 与 io_uring 批量读取语义归档 |
| `docs/design/dynamic-write.md` | WAL/MemTable/SSTable/Compaction 动态写入主计划 |
| `docs/roadmap/dynamic-write-task-breakdown.md` | P5 动态写入拆分任务和验收清单 |
| `docs/experiments/high-concurrency-mixed-rw.md` | 高并发混合读写、动态 Recall、一致性方案 |
| `docs/roadmap/next-sift1m-mixed-rw-optimization.md` | SIFT1M mixed RW 下一步优化计划 |
| `docs/experiments/param-tuning-and-sift-scale-test.md` | 参数调优与 SIFT 规模化测试矩阵 |
| `docs/design/fresh-streaming-ann.md` | Fresh Streaming ANN 创新点和后续动态层路线 |

agent-aware 是一个面向大模型 Agent 长期记忆场景的向量检索 I/O 优化原型。项目目标是在全精度向量常驻 SSD、内存预算约束为数据集大小 10%-20% 的条件下，提供可复现的 Top-K 近似最近邻检索、缓存/预取/I/O 统计，以及动态写入路径验证。

当前主线实现围绕 `agent_aware_flow` benchmark 展开：SIFT 或合成数据加载后，系统训练 PQ ADC 模型、构建或复用 Vamana packed graph index，再通过 Graph-Aware 2Q BufferPool、O_DIRECT/io_uring/pread 读路径和拓扑预取完成查询，并输出 JSON 结果。动态写入由 `bench_mixed_rw` 和 `StorageEngine`/`DynamicWriteManager` 验证，覆盖 WAL、MemTable、SSTable、flush、compaction 和 base/delta Top-K merge。

## 项目结构

| 路径 | 说明 |
| --- | --- |
| `include/agent_aware/core`, `src/agent_aware/core` | 基础类型、暴力检索、SIMD/L2 距离、PQ ADC、异步 page reader、预取规划 |
| `include/agent_aware/data`, `src/agent_aware/data` | SIFT `.fvecs`/`.ivecs` 读取与 synthetic 数据生成 |
| `include/agent_aware/graph`, `src/agent_aware/graph` | Vamana/近似图构建、packed page 编码、SSD graph index、entry point 选择 |
| `include/agent_aware/storage`, `src/agent_aware/storage` | 4KB disk record、disk index reader/writer、LSM 基础组件 |
| `include/agent_aware/dynamic`, `src/agent_aware/dynamic` | WAL、MemTable、SSTable、Manifest、Compaction、动态写入管理 |
| `include/agent_aware/engine`, `src/agent_aware/engine` | 对上层 Agent 或 benchmark 暴露的 `StorageEngine` 统一接口 |
| `tools/benchmarks/sift_search_benchmark.cpp` | SSD 检索主 benchmark，输出 JSON |
| `tools/benchmarks/mixed_rw_benchmark.cpp` | P5 混合读写 benchmark，输出 CSV 或 JSON |
| `tests/unit` | core、storage、dynamic、graph 分组单元测试 |
| `scripts/linux/run_sift1m_once.sh` | SIFT1M 一键构建/运行/归档脚本 |
| `PROJECT_PLAN.md` | 项目计划、技术路线、参数调优范围和验收标准 |
| `docs/README.md` | 文档导航、阶段计划完成度、结果报告索引和后续优先级 |

## 环境与构建

推荐在 Linux 或 WSL2 环境运行。`io_uring` 是可选能力；如果系统没有 `liburing`，`io_uring` 模式会自动回退到 `pread`，结果 JSON 会记录 fallback 原因。

依赖：

| 依赖 | 说明 |
| --- | --- |
| CMake >= 3.16 | 构建工程 |
| C++17 编译器 | GCC/Clang/MSVC 均可，Linux 路径能力更完整 |
| `liburing-dev` | 可选，用于启用 io_uring 后端 |

构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

常用 CMake 选项：

| 选项 | 默认值 | 可选值 | 说明 |
| --- | --- | --- | --- |
| `ENABLE_DIRECT_IO` | `ON` | `ON`/`OFF` | 是否启用 Linux O_DIRECT 支持 |
| `AGENT_AWARE_REQUIRE_LIBURING` | `OFF` | `ON`/`OFF` | 为 `ON` 时，缺少 liburing 会直接配置失败 |

测试：

```bash
./build/agent_aware_tests
./build/test_disk_record
./build/test_disk_index_rw
./build/test_memtable
./build/test_dynamic_wal
./build/test_sstable
./build/test_compaction
./build/test_dynamic_insert
./build/test_entry_selector
```

## 数据准备

默认 SIFT 数据目录为 `data/sift`，需要包含：

| 文件 | 说明 |
| --- | --- |
| `sift_base.fvecs` | base 向量 |
| `sift_query.fvecs` | query 向量 |
| `sift_groundtruth.ivecs` | ground truth，用于计算 Recall@K |

如果 SIFT 文件不在默认目录，可使用 `--sift-dir` 指定目录，或分别用 `--base`、`--query`、`--truth` 指定路径。

开发和 CI 可使用 synthetic 数据：

```bash
./build/agent_aware_flow \
  --synthetic 1 \
  --base-limit 10000 \
  --query-limit 20 \
  --synthetic-dim 32 \
  --synthetic-clusters 16 \
  --rebuild-index 1 \
  --index-path indexes/synthetic_demo.idx \
  --output-json logs/sift_bench/synthetic_demo/result.json
```

注意：如果使用 SIFT ground truth，又把 `--base-limit` 截断到小于完整 base 集合，truth 中可能出现超出当前 base 范围的 id。此时需要使用匹配该截断数据集的 truth，或加载完整 base。

## 使用方式

### 一键 SIFT 运行

`scripts/linux/run_sift1m_once.sh` 会构建 `agent_aware_flow`、执行 benchmark，并保存命令、stdout JSON、结果 JSON 和耗时信息。

第一次建图：

```bash
REBUILD_INDEX=1 bash scripts/linux/run_sift1m_once.sh
```

后续复用索引：

```bash
bash scripts/linux/run_sift1m_once.sh
```

临时覆盖参数：

```bash
EXTRA_ARGS="--search-width 512 --beam-width 32 --io-depth 64" \
  bash scripts/linux/run_sift1m_once.sh
```

输出目录默认形如 `logs/sift_bench/sift1m_once_YYYYMMDD-HHMMSS/`：

| 文件 | 内容 |
| --- | --- |
| `command.txt` | cwd、完整命令、`REBUILD_INDEX`、`EXTRA_ARGS` |
| `stdout.json` | 程序 stdout 中的完整 JSON |
| `result.json` | `--output-json` 指定的结果 JSON |
| `stderr.log` | 错误输出 |
| `time.txt` | start/end epoch、wall seconds、退出状态 |

### 直接运行 SSD 检索主路径

SIFT1M 推荐入口：

```bash
./build/agent_aware_flow \
  --sift-dir data/sift \
  --base-limit 1000000 \
  --query-limit 100 \
  --graph-degree 32 \
  --build-policy vamana \
  --packing-strategy bfs \
  --enable-pq 1 \
  --pq-subspaces 32 \
  --pq-centroids 256 \
  --pq-train-limit 100000 \
  --pq-iterations 8 \
  --rerank-topk 100 \
  --top-k 10 \
  --search-width 350 \
  --entry-strategy single-medoid \
  --entry-count 64 \
  --io-mode io_uring \
  --io-batch 16 \
  --io-depth 32 \
  --cache-policy graph-aware-2q \
  --memory-budget-ratio 0.20 \
  --prefetch-policy next-hop \
  --prefetch-width 4 \
  --rebuild-index 1 \
  --index-path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --output-json logs/sift_bench/sift1m_demo/result.json
```

复用索引时，把 `--rebuild-index 1` 改为 `--rebuild-index 0` 或直接省略。

### 混合读写 benchmark

不传 `--index_path` 时，`bench_mixed_rw` 使用内存精确 base path 与动态 delta merge，适合验证 WAL/MemTable/SSTable 正确性：

```bash
./build/bench_mixed_rw \
  --num_operations 10000 \
  --topk 10 \
  --output build/p5_mixed_rw.csv
```

传入 SSD packed graph index 时，查询走 `PackedGraphEngine`，写入进入 dynamic manager：

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 256 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --num_operations 10000 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --topk 10 \
  --search_width 350 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode pread \
  --cache_policy graph-aware-2q \
  --output build/p5_mixed_rw_ssd.csv
```

当前版本推荐用三组实验分开展示性能与正确性：

1. 纯读 baseline：关闭 writer、Recall exact、compaction，只测 SSD 主图并发读扩展性。

```bash
for t in 1 2 4 8; do
  ./build/bench_mixed_rw \
    --data_path data/sift/sift_base.fvecs \
    --base_count 1000000 \
    --query_count 1000 \
    --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
    --dynamic_dir build/sift1m_readonly_t${t}_dynamic \
    --duration_sec 30 \
    --read_threads "$t" \
    --write_threads 0 \
    --read_ratio 1 \
    --write_ratio 0 \
    --recall_sample_rate 0 \
    --enable_compaction 0 \
    --topk 10 \
    --search_width 350 \
    --entry_count 64 \
    --beam_width 16 \
    --io_mode io_uring \
    --cache_policy graph-aware-2q \
    --output build/sift1m_readonly_t${t}.json
done
```

2. 混合读写主路径：关闭 Recall exact，避免把 O(N) exact 回算计入吞吐；可分别对比 compaction 开/关。

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --dynamic_dir build/sift1m_mixed_immutable_compaction_dynamic \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 1 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --recall_sample_rate 0 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --compaction_interval_ms 1000 \
  --topk 10 \
  --search_width 350 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_mixed_rw_no_recall_compaction_immutable_view.json
```

3. 动态 Recall evidence：单独开启 Recall 抽样和限速，用于验证 read_sequence + visible set + base/delta merge 正确性，不用于主路径吞吐解释。

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --dynamic_dir build/sift1m_dynamic_recall_immutable_view_dynamic \
  --duration_sec 30 \
  --read_threads 1 \
  --write_threads 1 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --recall_sample_rate 1 \
  --recall_max_samples_per_sec 1 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --compaction_interval_ms 1000 \
  --topk 10 \
  --search_width 350 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_dynamic_recall_immutable_view.json
```

## `agent_aware_flow` 参数说明

布尔参数接受 `1/0`、`true/false`、`on/off`、`yes/no`。多数参数同时支持连字符和下划线别名，例如 `--search-width` 与 `--search_width` 等价。下表的默认值按当前 `Args` 结构体和实际 JSON 输出整理。

### 数据参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--synthetic` | `0` | `0/1` | 是否使用 synthetic 数据；为 `0` 时加载 SIFT/fvecs |
| `--sift-dir` | `data/sift` | 有效目录 | 自动解析 `sift_base.fvecs`、`sift_query.fvecs`、`sift_groundtruth.ivecs` |
| `--base` | 空 | `.fvecs` 路径 | 手动指定 base 文件，会关闭 synthetic |
| `--query` | 空 | `.fvecs` 路径 | 手动指定 query 文件，会关闭 synthetic |
| `--truth` | 空 | `.ivecs` 路径 | 手动指定 ground truth；为空则不计算 Recall |
| `--base-limit` | `1000000` | `>=1` | 加载 base 向量数 |
| `--query-limit` | `100` | `>=1` | 加载 query 向量数 |
| `--synthetic-dim` | `32` | `>=1` | synthetic 向量维度 |
| `--synthetic-clusters` | `16` | `>=1`，实际不超过 `base-limit` | synthetic 簇数量 |
| `--seed` | `42` | `>=0` | 随机种子，用于数据、PQ、构图和 entry selection |

### 构图与索引参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--index-path` | `indexes/sift1m_vamana_pq100_p4096_sm.idx` | 可写路径 | packed graph index 文件 |
| `--output-json` | `logs/sift_bench/result.json` | 可写路径 | benchmark 结果 JSON |
| `--rebuild-index` | `0` | `0/1` | 是否强制重建索引；为 `0` 且 index 存在时复用 |
| `--graph-degree` | `32` | `>=1`，推荐 `16-64` | Vamana 图最大邻居数，影响 Recall、内存和构图时间 |
| `--page-size` | `4096` | `>=4096` | packed graph page 大小；需要容纳 node record |
| `--build-policy` | `vamana` | `exact`、`approx-rp`、`lsh-rp`、`lsh-vamana`、`vamana` | 构图策略；`exact` 适合小规模基线，`vamana` 是当前主路径 |
| `--packing-strategy` | `bfs` | `random`、`bfs`、`coaccess`、`hotpath` | page 内节点排列策略；影响局部性和缓存命中 |

### 搜索参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--top-k`, `--k` | `10` | `>=1`，计划推荐 `1-100` | 返回结果数量 |
| `--search-width` | `350` | `>=1`，计划推荐 `100-800` | 图搜索扩展预算；越大 Recall 通常越高，I/O 越多 |
| `--beam-width` | `16` | `>=1`，有效值会 clamp 到 `1..search-width`；推荐 `8-32` | 每轮批量扩展候选数，影响 I/O 批处理 |
| `--entry-count` | `64` | `>=1` | entry point 数量上限；`single-medoid` 实际只返回 1 个全局 medoid |
| `--entry-strategy` | `single-medoid` | `single-medoid`、`evenly-spaced`、`hybrid` | entry point 选择策略 |
| `--entry-hub-count` | `15` | `>=0` | `hybrid` 策略下按入度选取的 hub entry 数 |
| `--entry-cluster-count` | `0` | `>=0` | `hybrid` 策略下 cluster medoid 数；`0` 表示填满剩余 entry |
| `--entry-cluster-train-limit` | `100000` | `>=1` | cluster medoid 训练采样上限 |
| `--entry-cluster-iterations` | `4` | `>=1` | cluster k-means 迭代轮数 |
| `--num-search-threads` | `1` | `>=1` | 查询 worker 数；多线程会为每个 worker 打开独立 index/cache |

### PQ/ADC 参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--enable-pq` | `1` | `0/1` | 是否训练 PQ 并在搜索中启用 ADC 候选评分 |
| `--pq-subspaces` | `32` | `>=1`，有效值不超过向量维度 | PQ 子空间数；计划中常见调优范围为 `8-16`，当前 SIFT 默认用 `32` 提升近似精度 |
| `--pq-centroids` | `256` | `2-256` | 每个子空间聚类中心数 |
| `--pq-train-limit` | `100000` | `>=pq-centroids` | PQ 训练样本上限 |
| `--pq-iterations` | `8` | `>=1` | PQ k-means 迭代次数 |
| `--rerank-topk` | `100` | `0` 或 `>=top-k` | PQ 后精排候选池；`0` 表示关闭 exact rerank |

### 预取、缓存与 I/O 参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--enable-prefetch` | `1` | `0/1` | 是否尝试启用异步预取 |
| `--prefetch-depth` | `1` | `0` 或 `1` | 当前实现只支持 0/1；`0` 等价关闭预取 |
| `--prefetch-width` | `4` | `>=1` | 每次 frontier/next-hop 预取宽度 |
| `--prefetch-policy` | `next-hop` | `none`、`frontier`、`next-hop`、`frontier-next-hop` | 预取策略 |
| `--io-mode` | `io_uring` | `pread`、`odirect`、`io_uring` | 磁盘读模式；不满足条件时会 fallback 到 `pread` |
| `--io-batch` | `16` | `>=1` | I/O batch size |
| `--io-depth` | `32` | `>=1`，计划推荐 `32-512` | io_uring queue depth |
| `--cache-policy` | `graph-aware-2q` | `none`、`lru`、`2q`、`graph-aware-2q`、`agent` | page cache 淘汰策略 |
| `--cache-pages` | `0` | `>=0` | `0` 且 cache policy 非 `none` 时按内存预算自动计算 |
| `--memory-budget-ratio` | `0.20` | `(0,1]`，验收推荐 `0.10-0.20` | resident memory 预算占原始向量字节比例 |
| `--protect-hot-pages` | `1` | `0/1` | 是否保护热页；graph-aware 策略会利用 hub 入度信息 |
| `--hot-degree-threshold` | `0` | `>=0` | hub 热度阈值；`0` 表示由策略默认处理 |

## `bench_mixed_rw` 参数说明

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--data_path` | 空 | `.fvecs` 路径 | 可选 base 数据；为空时生成 synthetic base |
| `--index_path` | 空 | packed graph index 路径 | 为空时走内存精确 base；非空时走 SSD graph base。使用 SSD graph 时，应保证 index 与 `--data_path`/`--base_count`/维度匹配 |
| `--dynamic_dir` | `build/p5_mixed_rw_dynamic` | 可写目录 | WAL/SSTable/manifest 输出目录 |
| `--output` | `build/p5_mixed_rw.csv` | 可写路径 | 输出路径；扩展名为 `.json` 时输出结构化 JSON，否则输出 CSV |
| `--num_operations` | `1000` | `>0` | 每个场景执行的 operation 事件数 |
| `--duration_sec` | `0` | `>=0` | 大于 0 时启用定时并发模式 |
| `--read_threads` | `0` | `>=0` | 并发 reader 线程数；定时模式下每个 reader 独立打开 SSD graph engine |
| `--write_threads` | `0` | `>=0` | 并发 writer 线程数；写入进入 `DynamicWriteManager` |
| `--read_ratio` | 未设置 | `>=0` | 自定义单场景读比例；和写比例会归一化 |
| `--write_ratio` | 未设置 | `>=0` | 自定义单场景写比例；未设置时默认跑 read-heavy/balanced/write-heavy |
| `--recall_sample_rate` | `1.0` | `[0,1]` | 动态 Recall 抽样概率；吞吐实验建议设为 `0` |
| `--recall_max_samples_per_sec` | `0` | `>=0` | Recall exact 回算全局限速；`0` 表示不限速 |
| `--topk` | `10` | `>0` | 查询 Top-K |
| `--insert_batch_size` | `1` | `>0` | 每个写事件插入向量数 |
| `--enable_flush` | `1` | `0/1` | 是否允许自动 flush 并在结尾 flush |
| `--enable_compaction` | `0` | `0/1` | 是否执行一次测量型 compaction |
| `--compaction_background` | `0` | `0/1` | 是否在 workload 运行期间周期性后台 compaction |
| `--compaction_interval_ms` | `1000` | `>0` | 后台 compaction 周期 |
| `--base_count` | `1000` | `>0` | synthetic base 数量 |
| `--query_count` | `256` | `>0` | synthetic query 数量 |
| `--dim` | `32` | `>0` | synthetic 维度 |
| `--memtable_flush_bytes` | `262144` | `>0` | MemTable flush 阈值 |
| `--dynamic_graph_degree` | `16` | `>0` | delta graph 邻居数 |
| `--search_width` | `350` | `>0` | SSD graph 搜索扩展预算 |
| `--entry_count` | `64` | `>0` | SSD graph entry 数 |
| `--beam_width` | `16` | `>0` | SSD graph beam batch width |
| `--io_mode` | `pread` | `pread`、`odirect`、`io_uring` | SSD graph I/O 模式 |
| `--io_batch` | `16` | `>0` | I/O batch size |
| `--io_depth` | `32` | `>0` | I/O depth |
| `--cache_policy` | `graph-aware-2q` | `none`、`lru`、`2q`、`graph-aware-2q` | SSD graph cache policy |
| `--cache_pages` | `0` | `>=0` | cache page 数 |
| `--seed` | `42` | `>=0` | 随机种子 |

## 结果展示

### SIFT1M SSD 主路径示例

示例结果来自 `logs/sift_bench/sift1m_once_20260618-181128/result.json`。运行参数如下：

```bash
./build/agent_aware_flow \
  --sift-dir data/sift \
  --base-limit 1000000 \
  --query-limit 100 \
  --graph-degree 32 \
  --page-size 4096 \
  --build-policy vamana \
  --packing-strategy bfs \
  --enable-pq 1 \
  --pq-subspaces 32 \
  --pq-centroids 256 \
  --pq-train-limit 100000 \
  --pq-iterations 8 \
  --rerank-topk 100 \
  --top-k 10 \
  --search-width 350 \
  --entry-strategy single-medoid \
  --entry-count 64 \
  --io-mode io_uring \
  --io-batch 16 \
  --io-depth 32 \
  --cache-policy graph-aware-2q \
  --memory-budget-ratio 0.20 \
  --prefetch-policy next-hop \
  --prefetch-width 4 \
  --index-path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --output-json logs/sift_bench/sift1m_once_20260618-181128/result.json
```

核心结果：

| 指标 | 数值 |
| --- | ---: |
| dataset | SIFT, base=1,000,000, query=100, dim=128 |
| Recall@10 | `0.9940` |
| QPS | `2.4062` |
| avg query latency | `415.597 ms` |
| P95 / P99 latency | `522.014 ms` / `543.594 ms` |
| effective search width / beam width | `350` / `16` |
| entry strategy / seed count | `single-medoid` / `1` |
| I/O effective mode | `io_uring` |
| prefetch | enabled, `next-hop`, width=`4`, depth=`1` |
| cache policy / pages | `graph-aware-2q` / `15186` |
| cache hit rate | `0.5024` |
| page reads per query | `918.99` |
| visited per query | `5291.59` |
| expanded per query | `350.00` |
| estimated resident ratio | `0.199992` |
| warnings | none |

结果 JSON 会同时保留参数、有效参数和统计值。常用字段：

| 字段 | 说明 |
| --- | --- |
| `status` | 是否完成 |
| `dataset_mode`, `base_count`, `query_count`, `dim` | 数据集信息 |
| `requested_*`, `effective_*` | 请求参数与最终生效参数 |
| `io_requested_mode`, `io_effective_mode`, `io_fallback_reason` | I/O 模式与 fallback 原因 |
| `memory_budget_ratio`, `estimated_resident_ratio`, `cache_pages_auto` | 内存预算报告 |
| `qps`, `avg_query_latency_ms`, `latency_p95_ms`, `latency_p99_ms` | 性能指标 |
| `recall_at_k`, `recall_queries` | 召回率指标 |
| `stats.page_cache_hits`, `stats.cache_hit_rate` | 缓存命中统计 |
| `stats.io_submits`, `stats.uring_submit_count`, `stats.uring_cqe_count` | I/O 提交和完成统计 |
| `stats.prefetch_submitted_pages`, `stats.prefetch_useful_pages`, `stats.prefetch_wasted_pages` | 预取有效性统计 |
| `stats.distance_direct_calls`, `stats.rerank_reads` | 零拷贝距离计算和 rerank 统计 |

### 混合读写 CSV 示例

示例命令：

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 256 \
  --index_path indexes/sift1m_vamana_pq100_p4096_sm.idx \
  --num_operations 1000 \
  --topk 10 \
  --search_width 350 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode pread \
  --cache_policy graph-aware-2q \
  --output build/p5_mixed_rw_ssd.csv
```

CSV 字段含义：

| 字段 | 说明 |
| --- | --- |
| `scenario` | `read-heavy`、`balanced`、`write-heavy` 或 `custom` |
| `read_ratio`, `write_ratio` | 读写比例 |
| `read_qps`, `write_qps`, `insert_throughput` | 查询、写入和插入吞吐 |
| `avg_latency`, `p50_latency`, `p95_latency`, `p99_latency` | operation 级延迟 |
| `recall_at_10` | 与精确 base/delta merge 对比的 Recall@10 |
| `memtable_insert_avg_us` | 写事件平均耗时 |
| `flush_duration_ms`, `sstable_count`, `delta_record_count` | flush/SSTable/delta 统计 |
| `memory_usage_mb`, `disk_usage_mb`, `recovery_time_ms` | 动态路径资源与恢复统计 |
| `search_mode` | `memory_exact` 或 `ssd_graph` |
| `graph_reads_per_read`, `cache_hit_rate` | SSD graph 查询读放大和缓存命中率 |

当 `--output` 使用 `.json` 后缀时，`bench_mixed_rw` 会额外输出结构化配置和读路径分段耗时：

| 字段 | 说明 |
| --- | --- |
| `benchmark_config` | 本次实验的数据、索引、线程数、search width、beam width、Recall 抽样、compaction 和 git 状态 |
| `read_breakdown.dynamic_snapshot_*_ms` | 读取动态层 read sequence / read view 的耗时 |
| `read_breakdown.base_search_*_ms` | SSD packed graph 主图查询耗时 |
| `read_breakdown.latest_record_lookup_*_ms` | 检查 base topK 是否被动态 update/delete 覆盖的耗时 |
| `read_breakdown.delta_search_*_ms` | 动态 delta 层向量搜索耗时 |
| `read_breakdown.merge_*_ms` | base + delta Top-K merge 耗时 |
| `read_breakdown.exact_recall_*_ms` | 动态 Recall exact truth 回算耗时；只用于正确性实验 |
| `read_breakdown.search_mutex_wait_*_ms` | packed graph 内部 search mutex 等待时间 |
| `read_breakdown.page_read_wait_*_ms` | demand page read 等待时间 |

## 具体工作流程

### SSD 检索流程

1. 解析 CLI 参数，校验 `prefetch_depth`、PQ、内存预算等约束。
2. 通过 `load_dataset` 加载 SIFT/fvecs，或生成 synthetic 数据。
3. 如果 `--enable-pq=1`，用 base 训练 `PqAdcModel`，常驻 PQ codes 和 codebooks。
4. 根据 `--memory-budget-ratio` 估算 resident memory，并在未手动设置 `--cache-pages` 时自动计算 cache 容量。
5. 如果 `--rebuild-index=1` 或 index 不存在，调用 `PackedDiskGraphBuilder::build` 构建 packed graph index。
6. 打开 `PackedDiskGraphIndex`，配置 cache policy 和 I/O mode。
7. 根据 `--entry-strategy` 选择 entry point；`hybrid` 会结合 medoid、hub 和 cluster medoid。
8. 每个 query 构造 `DiskGraphSearchConfig`，执行 graph beam search。
9. 搜索时优先命中 BufferPool；miss 时通过 `pread`、`odirect` 或 `io_uring` 读取 4KB page。
10. 命中 page 后直接从 page buffer 计算 L2 距离，并用 PQ/rerank 更新候选队列。
11. 如果有效 I/O mode 为 `io_uring` 且预取策略非 `none`，按 frontier 或 next-hop 提交异步预取。
12. 聚合所有 query 的 Recall、QPS、延迟、缓存、I/O、预取、内存预算统计，写入 JSON。

### 动态写入流程

1. `DynamicWriteManager::open` 创建或恢复 manifest、WAL、MemTable 和 SSTable reader。
2. `insert/update/erase` 先追加 WAL，再写入 active MemTable。
3. 写入成功后发布 immutable `DynamicReadView`，reader 通过 atomic `shared_ptr` 获得动态层快照，避免和 writer/flush/compaction 抢全局 mutex。
4. MemTable 超过 `memtable_flush_bytes` 时 flush 为 SSTable，并轮转 WAL；后台 compaction 通过 manifest 发布新 SSTable 视图。
5. 查询时，`PackedGraphEngine` 先查 SSD base graph，再按 `read_sequence` 查 base id 的 update/delete 覆盖记录和 delta records，最后调用 `merge_base_and_delta_l2` 合并 Top-K。
6. `bench_mixed_rw` 以 read-heavy、balanced、write-heavy 或自定义比例生成读写事件，记录吞吐、延迟、Recall、flush、恢复、compaction 和读路径分段耗时。

## 重要接口

### 数据与基础类型

```cpp
#include "agent_aware/data/dataset.h"
#include "agent_aware/core/types.h"

agent_aware::LoadedDataset load_dataset(const agent_aware::DatasetLoadConfig& config);
agent_aware::VectorSet load_fvecs(const std::string& path, std::size_t limit = 0);
std::vector<std::vector<std::uint32_t>> load_ivecs(
    const std::string& path, std::size_t limit = 0);
agent_aware::SyntheticData generate_synthetic(
    const agent_aware::SyntheticConfig& config);
```

`VectorSet` 按行连续保存 float 向量，`SearchResult` 保存 `id` 和 squared L2 distance。

### PQ ADC

```cpp
#include "agent_aware/core/pq_encoder.h"

agent_aware::PQEncoder pq;
agent_aware::PQTrainingConfig config;
config.subspaces = 32;
config.centroids = 256;
config.train_limit = 100000;
config.iterations = 8;
auto stats = pq.train(base, config);

auto adc_table = pq.build_adc_table(query);
float approx_distance = pq.adc_distance(id, adc_table);
```

### Packed graph index

```cpp
#include "agent_aware/graph/disk_graph_index.h"

agent_aware::DiskGraphBuildConfig build;
build.degree = 32;
build.page_size = 4096;
build.build_policy = "vamana";
build.packing_strategy = "bfs";
agent_aware::PackedDiskGraphBuilder::build(base, "indexes/demo.idx", build);

agent_aware::PackedDiskGraphIndex index("indexes/demo.idx");
index.configure_cache("graph-aware-2q", 4096);
index.configure_io("io_uring", 16, 32);

agent_aware::DiskGraphSearchConfig search;
search.top_k = 10;
search.search_width = 350;
search.beam_width = 16;
search.entry_count = 64;
auto result = index.search_one(query, search);
```

### StorageEngine 集成接口

```cpp
#include "agent_aware/engine/storage_engine.h"

agent_aware::PackedGraphEngineConfig config;
config.index_path = "indexes/demo.idx";
config.cache_policy = "graph-aware-2q";
config.cache_pages = 4096;
config.io_mode = "pread";
config.search.search_width = 350;
config.search.entry_count = 64;
config.search.beam_width = 16;

agent_aware::PackedGraphEngine engine(std::move(config));
auto result = engine.search_one(query, 10);
```

`StorageEngine` 的统一抽象包含：

```cpp
virtual EngineSearchResult search_one(const float* query, std::size_t top_k) = 0;
virtual void insert(std::uint32_t id, const float* vector);
virtual void update(std::uint32_t id, const float* vector);
virtual void erase(std::uint32_t id);
```

### 动态写入接口

```cpp
#include "agent_aware/dynamic/dynamic_write_manager.h"

agent_aware::dynamic::DynamicWriteOptions options;
options.dynamic_dir = "build/dynamic";
options.memtable_flush_bytes = 256 * 1024;
options.dynamic_graph_degree = 16;

agent_aware::dynamic::DynamicWriteManager manager(options);
manager.open();
manager.insert(node_id, vector, dim);
manager.update(node_id, vector, dim);
manager.erase(node_id);
manager.flush();
auto delta = manager.search_delta_l2(query, dim, 10);
manager.close();
```

## 调参与验收建议

| 目标 | 优先调整参数 | 观察指标 |
| --- | --- | --- |
| 提高 Recall | 增大 `--search-width`、`--beam-width`、`--graph-degree`，或提高 `--rerank-topk` | `recall_at_k`、`visited`、`page_reads` |
| 降低 I/O | 启用 PQ、提高缓存页、使用 `graph-aware-2q`、调整 packing | `node_reads/query`、`cache_hit_rate`、`distance_direct_calls` |
| 控制内存 | 降低 `--cache-pages` 或 `--memory-budget-ratio`，降低 `pq_subspaces`/`graph_degree` | `estimated_resident_ratio`、`warnings` |
| 观察 io_uring 收益 | 对比 `--io-mode pread` 与 `--io-mode io_uring` | `qps`、`latency_p95_ms`、`io_submit_syscalls` |
| 评估预取 | 对比 `--prefetch-policy none`、`next-hop`、`frontier-next-hop` | `prefetch_useful_pages`、`prefetch_wasted_pages` |
| 验证动态写入 | 调整 `--read_ratio`、`--write_ratio`、`--memtable_flush_bytes` | `write_qps`、`flush_duration_ms`、`recovery_time_ms` |

正式报告建议固定记录：完整命令、git commit、dirty files、数据路径、随机种子、index path、JSON/CSV 结果、硬件环境和是否发生 I/O fallback。
