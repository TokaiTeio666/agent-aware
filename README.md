# Agent-aware

Agent-aware 是一个面向大模型 Agent 长期记忆场景的向量检索 I/O 优化原型。项目目标是在全精度向量常驻 SSD、内存预算约束为数据集大小 10%-20% 的条件下，提供可复现的 Top-K 近似最近邻检索、缓存/预取/I/O 统计，以及动态写入路径验证。

静态链路由两个独立程序组成：`agent-aware-build` 加载 base、训练 PQ 并生成 V3 packed Vamana 索引，同时把 Vamana global medoid 写入配套 manifest；`agent-aware-search` 只加载 query/truth、已有索引和 manifest，使用该 single-medoid 执行在线 benchmark。搜索进程不会加载 base，也不会在索引或 manifest 缺失时隐式构建。

当前推荐口径（来源 `build/search_width_156_baseline.json`；SIFT1M、query 5000-5999、`search_width=156`、`beam_width=16`、io_uring、cache=15000 pages、关闭预取）：

| 指标 | 数值 |
| --- | ---: |
| dataset | SIFT, base=1,000,000, query=1,000, dim=128 |
| Recall@10 | `0.9748` |
| avg latency | `48.10 ms` |
| latency P95 | `62.39 ms` |
| latency P99 | `66.29 ms` |
| QPS | `20.79` |
| search width / beam width | `156` / `16` |
| I/O effective mode | `io_uring` |
| cache policy / pages | `graph-aware-2q` / `15000` |
| cache hit rate | `18.16%` |
| page reads / query | `109.82` |
| rerank reads / query | `6.36` |
| I/O submit calls / query | `12.51` |
| estimated resident ratio | `0.1985` |

`search_width` 对照使用相同二进制、索引、查询窗口和 I/O 参数：

| search width | Recall@10 | avg latency | P95 | P99 | QPS | reads/query |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `128` | `0.9610` | `49.86 ms` | `66.25 ms` | `70.23 ms` | `20.06` | `106.87` |
| **`156`** | **`0.9748`** | **`48.10 ms`** | **`62.39 ms`** | **`66.29 ms`** | **`20.79`** | `109.82` |
| `192` | `0.9815` | `99.12 ms` | `124.69 ms` | `129.59 ms` | `10.09` | `136.62` |

在当前已测点中，`156` 是 Recall、延迟和 I/O 的最佳平衡点。`128` 虽少读约 2.7 pages/query，但搜索阶段物化候选不足，使 exact rerank reads 从 `6.36/query` 增至 `28.68/query`，没有换来更低尾延迟；`192` 只增加 `0.67` 个百分点 Recall，却显著增加 I/O 和延迟。详细命令、原始字段和复验要求见 [`docs/experiments/search-width-tuning.md`](docs/experiments/search-width-tuning.md)。这些结果来自 WSL2 开发环境，其中 `156` 和 `192` 当前各记录一轮，正式答辩结论仍需三轮中位数和真实 NVMe 复测。

## 技术架构

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                         Agent-Aware Architecture                             │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Benchmark / Agent API                                                       │
│  agent-aware-build, agent-aware-search, bench_mixed_rw, StorageEngine        │
│                                │                                             │
│                                ▼                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │ Engine Layer                                                           │  │
│  │ PackedGraphEngine: base graph search + dynamic delta merge             │  │
│  └───────────────────────────────┬────────────────────────────────────────┘  │
│                                  │                                           │
│              ┌───────────────────┴───────────────────┐                       │
│              ▼                                       ▼                       │
│  ┌───────────────────────────────┐       ┌───────────────────────────────┐   │
│  │ In-Memory Components          │       │ Dynamic Write Layer           │   │
│  │ <= 20% resident budget        │       │ DynamicWriteManager           │   │
│  │                               │       │                               │   │
│  │  PQ Codes                     │       │  WAL append                   │   │
│  │  Graph Nav Data               │       │  MemTable / immutable view    │   │
│  │  PQ Codebooks                 │       │  SSTable + Manifest           │   │
│  │  Graph-Aware 2Q BufferPool    │       │  Compaction / recovery        │   │
│  │  VisitedBitmap                │       │                               │   │
│  └───────────────┬───────────────┘       └───────────────┬───────────────┘   │
│                  │                                       │                   │
│                  │ PQ ADC filter + exact rerank          │ updates/deletes   │
│                  │ pread / O_DIRECT / io_uring           │ delta records     │
│                  ▼                                       ▼                   │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │ SSD-Resident Components                                                │  │
│  │ Packed Vamana graph index                                              │  │
│  │ Disk node records: fixed 4KB pages                                     │  │
│  │ [NodeID][FullVector][NeighborIDs][NeighborPQCodes][Padding]            │  │
│  └───────────────────────────────┬────────────────────────────────────────┘  │
│                                  │                                           │
│                                  ▼                                           │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │ Query Runtime                                                          │  │
│  │ manifest single-medoid -> beam search -> QueryPageSession             │  │
│  │ post-expand: PQ ADC 粗排 -> top beam*2 节点聚页 -> io_uring 异步预取      │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

## 项目结构

```text
agent-aware/
├── CMakeLists.txt                         # 顶层构建入口，生成库和 benchmark 可执行文件
├── README.md                              # 项目入口、构建运行、实验结果和参数说明
├── PROJECT_PLAN.md                        # 总体计划、技术路线和验收标准
├── include/                               # 对外头文件根目录
│   ├── agent_aware.h                      # 聚合入口头
│   ├── core/                              # 基础类型、距离计算、PQ、预取和异步 page reader
│   │   ├── types.h
│   │   ├── brute_force.h
│   │   ├── pq_encoder.h
│   │   ├── async_page_reader.h
│   │   ├── query_page_session.h
│   │   └── prefetch_adaptive_controller.h
│   ├── data/                              # SIFT/fvecs/ivecs 读取与 synthetic 数据生成
│   ├── graph/                             # Vamana 构图、packed page 编码、SSD graph index
│   ├── dynamic/                           # WAL、MemTable、SSTable、Manifest、Compaction
│   └── engine/                            # StorageEngine / PackedGraphEngine 对外接口
├── src/                                   # C++ 实现
│   ├── core/                              # PQ ADC、SIMD/L2、io_uring/pread/O_DIRECT 读路径
│   ├── data/                              # 数据集加载实现
│   ├── graph/                             # Vamana 构图、single-medoid、packed graph search
│   ├── dynamic/                           # 动态写入、flush、recovery、compaction
│   ├── engine/                            # base graph + delta merge 查询引擎
│   ├── index_builder.cpp                  # 构建为 agent-aware-build，生成 V3 索引与 manifest
│   ├── sift_search_benchmark.cpp          # 构建为 agent-aware-search，仅执行 SSD 检索
│   └── mixed_rw_benchmark.cpp             # 构建为 bench_mixed_rw，输出混合读写 CSV/JSON
├── scripts/
│   ├── agent_aware.py                      # once/ab benchmark 统一入口
└── docs/
    ├── design/                            # SSD 存储、缓存、异步预取、动态写入设计
    ├── experiments/                       # SIFT1M、混合读写、参数调优实验报告
    ├── problem/                           # 赛题背景与问题定义
    └── roadmap in future/                 # 后续优化计划与答辩补强路线
```

## 核心特性

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| Recall/延迟平衡 | 已验证单轮 | SIFT1M `search_width=156` 达到 Recall@10 `0.9748`、avg `48.10 ms`、P99 `66.29 ms` |
| 内存预算报告 | 已实现估算 | 当前结果 estimated resident ratio `0.198505`；正式验收还需补进程 RSS 口径 |
| Vamana packed graph | 已实现 | Vamana 构图、BFS packing 和 SSD page 化存储 |
| PQ ADC 编码器 | 已实现 | 支持 PQ codes、codebooks、ADC 候选评分和 exact rerank |
| SIMD/L2 距离计算 | 已实现 | core 路径提供 L2 距离计算，查询中支持 direct distance/rerank |
| O_DIRECT / pread 读路径 | 已实现 | Linux 下支持 O_DIRECT；不满足条件时保持 `pread` fallback |
| Graph-Aware 2Q 缓存 | 已实现 | 支持 graph-aware-2q、hot page protection、缓存命中统计 |
| 动态写入路径 | 已实现 | WAL + MemTable + SSTable + Manifest + Compaction + recovery |
| immutable read view | 已实现 | reader 通过 atomic read view 读取动态层快照，降低读写锁竞争 |
| 混合读写 benchmark | 已实现 | `bench_mixed_rw` 支持并发读写、JSON 输出和读路径分段耗时 |
| SIFT 参数矩阵工具 | 已实现 | Python 脚本支持批量跑参数矩阵并生成曲线/汇总结果 |

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

构建验证：

```bash
./build/agent-aware-build --help
./build/agent-aware-search --help
./build/bench_mixed_rw --help
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
./build/agent-aware-build \
  --synthetic 1 \
  --base-limit 10000 \
  --synthetic-dim 32 \
  --synthetic-clusters 16 \
  --index-path indexes/synthetic_demo.idx
```

注意：如果使用 SIFT ground truth，又把 `--base-limit` 截断到小于完整 base 集合，truth 中可能出现超出当前 base 范围的 id。此时需要使用匹配该截断数据集的 truth，或加载完整 base。

## 使用方式

###  SIFT 运行

构建和 benchmark 是显式的两个阶段。Python 入口的 `once` 和 `ab` 只运行 `agent-aware-search`，要求索引及其 manifest 已经存在。

构建索引：

```bash
./build/agent-aware-build --sift-dir data/sift --index-path indexes/sift.idx
```

后续复用索引：

```bash
python3 scripts/agent_aware.py once
```

临时覆盖参数：

```bash
python3 scripts/agent_aware.py once \
  --extra-arg=--search-width --extra-arg=156 \
  --extra-arg=--beam-width --extra-arg=32
```

输出目录默认形如 `logs/sift_bench/once_YYYYMMDD-HHMMSS/`：

| 文件 | 内容 |
| --- | --- |
| `result.json` | `--output-json` 指定的结果 JSON |
| `run.log` | benchmark stdout/stderr；完整命令同时打印到终端 |

### 直接运行 SSD 检索主路径

SIFT1M 推荐 baseline 入口（复用已有索引，先关闭预取以固定 Recall/延迟口径）：

```bash
./build/agent-aware-search \
  --sift-dir data/sift \
  --query-offset 5000 \
  --query-limit 1000 \
  --enable-pq 1 \
  --rerank-topk 100 \
  --top-k 10 \
  --search-width 156 \
  --beam-width 16 \
  --io-mode io_uring \
  --io-batch 16 \
  --io-depth 32 \
  --cache-policy graph-aware-2q \
  --memory-budget-ratio 0.20 \
  --cache-pages 15000 \
  --index-path indexes/sift.idx \
  --run-id search-width-156-baseline \
  --output-json build/search_width_156_baseline.json
```

V3 索引保存完整向量页和 PQ 模型，header 保持不变。构建阶段将 global medoid ID 写入默认的 `<index>.manifest.json`；在线搜索严格校验 manifest 与索引的 version、vector count、dimension 和文件字节数后，从该 single-medoid 启动。可用 `--manifest PATH` 覆盖默认侧车路径。

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
  --index_path indexes/sift.idx \
  --num_operations 10000 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --topk 10 \
  --search_width 156 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode pread \
  --cache_policy graph-aware-2q \
  --output build/p5_mixed_rw_ssd.csv
```

SSD mixed-RW 会在场景计时前使用 `--data_path` 加载的 base 和 PQ 参数训练一次共享 `PqAdcModel`。所有 reader 复用该只读模型，训练耗时记录为 `benchmark_config.pq_training_ms`，不计入 read/write QPS。模型的向量数、维度和 subspaces 必须与 packed index metadata 一致；当前 `indexes/sift.idx` 使用默认 PQ 参数即可复现。

#### SIFT1M 动态混合读写结果

2026-07-11 在同一台 WSL2 主机上使用 4 reader、1 writer、30 秒 workload 完成配对实验。每个配置先预热一次，再正式运行三次；表中为中位数，括号内为最小值到最大值。写事件统一为 insert/update/delete = 70%/25%/5%，吞吐轮次关闭 exact Recall。

| 配置 | Read QPS | Write QPS | Read P95 (ms) | Read P99 (ms) | Delta search (ms/read) | Compaction 次数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 纯读，compaction 关 | 703.18 (695.36-703.68) | 0 | 6.70 (6.67-6.71) | 8.36 (8.23-8.84) | 0.002 | 0 |
| 95/5，compaction 关 | 647.05 (645.43-649.09) | 34.06 (34.00-34.19) | 7.23 (7.12-7.37) | 8.76 (8.63-8.95) | 0.205 | 0 |
| 95/5，compaction 开 | 653.09 (621.66-686.04) | 34.39 (32.75-36.12) | 7.16 (6.77-8.14) | 8.68 (8.21-9.57) | 0.206 | 1 |
| 80/20，compaction 开 | 578.23 (566.33-580.71) | 144.58 (141.62-145.19) | 8.31 (8.22-8.65) | 9.84 (9.66-10.27) | 0.912 | 9 (8-9) |

95/5、关闭 compaction 相对纯读的 read QPS 下降 8.0%，P95 增加 7.9%。95/5 下开启后台 compaction 的中位数没有退化，但三轮波动更大；80/20 相对 95/5 + compaction 的 read QPS 下降 11.5%，P95 增加 16.0%。独立正确性场景取得 29 个 exact 样本，Recall@10 为 0.9966，并覆盖 insert/update/delete 可见性。完整环境、三轮原始数据、复现方式和结论边界见 [SIFT1M 动态混合读写对照实验](docs/experiments/mixed-read-write.md)。

下面是 95/5 + 后台 compaction 的快速复现命令；纯读、compaction 开关和 80/20 的完整对照方法见专项报告。

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 1000 \
  --index_path indexes/sift.idx \
  --dynamic_dir build/sift1m_mixed_immutable_compaction_dynamic \
  --duration_sec 30 \
  --read_threads 4 \
  --write_threads 1 \
  --read_ratio 0.95 \
  --write_ratio 0.05 \
  --insert_ratio 0.70 \
  --update_ratio 0.25 \
  --delete_ratio 0.05 \
  --recall_sample_rate 0 \
  --enable_compaction 1 \
  --compaction_background 1 \
  --compaction_interval_ms 1000 \
  --topk 10 \
  --search_width 156 \
  --entry_count 64 \
  --beam_width 16 \
  --io_mode io_uring \
  --cache_policy graph-aware-2q \
  --output build/sift1m_mixed_rw_no_recall_compaction_immutable_view.json
```

不要在吞吐轮次开启 O(N) exact Recall 回算；正确性证据应使用单独的抽样限速场景。

## 静态链路参数说明

`agent-aware-build` 与 `agent-aware-search` 的参数边界是强制的：base、PQ 训练和图构建参数只属于构建阶段；在线检索只接受 query/truth、已有 index/manifest 和运行时搜索参数。搜索程序收到构建参数会直接报错。

### `agent-aware-build` 参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--synthetic` | `0` | `0/1` | 使用 synthetic base；builder 的布尔值使用 `0/1` |
| `--sift-dir` / `--base` | `data/sift` / 空 | 有效路径 | 选择 SIFT 目录或显式 base `.fvecs` |
| `--base-limit` | `1000000` | `>=1` | 构建使用的 base 向量数 |
| `--synthetic-dim`, `--synthetic-clusters` | `32`, `16` | `>=1` | synthetic 数据形状 |
| `--seed` | `42` | `>=0` | PQ 训练和 Vamana 构图随机种子 |
| `--index-path` | `indexes/sift.idx` | 可写路径 | V3 packed graph index |
| `--manifest` | `<index>.manifest.json` | 可写路径 | 保存格式、规模和 global medoid 的侧车文件 |
| `--rebuild` | `0` | `0/1` | 已存在索引是否允许覆盖；默认直接失败 |
| `--graph-degree` | `32` | `>=1`，推荐 `16-64` | Vamana 图最大邻居数，影响 Recall、内存和构图时间 |
| `--page-size` | `4096` | `>=4096` | packed graph page 大小；需要容纳 node record |
| `--pq-subspaces`, `--pq-centroids` | `32`, `256` | 与数据维度匹配 | PQ 模型结构，持久化进 V3 索引 |
| `--pq-train-limit`, `--pq-iterations` | `100000`, `8` | `>=1` | PQ 训练规模与迭代数 |

### `agent-aware-search` 数据与索引参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--sift-dir` | `data/sift` | 有效目录 | 解析 query 和 ground truth，不加载 base |
| `--query`, `--truth` | 空 | `.fvecs` / `.ivecs` 路径 | 覆盖 query 和可选 ground truth |
| `--query-offset`, `--query-limit` | `0`, `1000` | `>=0`, `>=1` | 在线查询窗口 |
| `--index-path` | `indexes/sift.idx` | 已有文件 | V3 packed graph index；缺失时直接失败 |
| `--manifest` | `<index>.manifest.json` | 已有文件 | 与索引严格校验后提供 single-medoid |
| `--output-json` | `logs/sift_bench/result.json` | 可写路径 | benchmark 结果 JSON |
| `--seed` | `42` | `>=0` | 实验复现元数据 |

### 搜索参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--top-k`, `--k` | `10` | `>=1`，计划推荐 `1-100` | 返回结果数量 |
| `--search-width` | `156` | `>=1`；SIFT1M 当前推荐 `156`，已测 `128/156/192` | 图搜索扩展预算；越大 Recall 通常越高，但需同时观察 rerank reads 与 I/O batching |
| `--beam-width` | `16` | `>=1`，有效值会 clamp 到 `1..search-width`；推荐 `8-32` | 每轮批量扩展候选数，影响 I/O 批处理 |
| `--num-search-threads` | `1` | `>=1` | 查询 worker 数；多线程会为每个 worker 打开独立 index/cache |

搜索入口固定为构建 manifest 中的 single-medoid，不提供 entry strategy/count 选择参数。

### PQ/ADC 与精排参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--enable-pq` | `1` | `1` | 搜索主路径要求 PQ+ADC 候选生成；`0` 已不再支持 |
| `--rerank-topk` | `100` | `0` 或 `>=top-k` | PQ 后精排候选池；`0` 表示关闭 exact rerank |

PQ offsets、codebooks 和全量 codes 均从索引恢复。PQ 结构参数属于 `agent-aware-build`，不能在搜索阶段覆盖。

### 预取、缓存与 I/O 参数

| 参数 | 默认值 | 可选范围 | 说明 |
| --- | --- | --- | --- |
| `--io-mode` | `io_uring` | `pread`、`odirect`、`io_uring` | 磁盘读模式；不满足条件时会 fallback 到 `pread` |
| `--io-batch` | `16` | `>=1` | 每次 batch submit 的请求上限 |
| `--io-depth` | `32` | `>=1`，计划推荐 `32-512` | io_uring queue depth |
| `--enable-prefetch` | `0` | `0/1` | 是否启用 topology 异步预取；正式 A/B 通过前默认关闭 |
| `--prefetch-policy` | `none` | `none`、`topology` | 确定性的 PQ/topology post-expand 策略 |
| `--prefetch-adaptive` | `1` | `0/1` | 根据 ready/pending/waste 反馈调节提交量 |
| `--prefetch-top-k` | `2` | `>=1` | 每个窗口最多提交页数；候选窗口仍为 `2*beam_width` |
| `--prefetch-max-inflight` | `2` | `>=1` | 单 query 最大未完成预取数 |
| `--prefetch-max-pages-per-query` | `0` | `>=0` | query 预取预算，`0` 表示不额外限制 |
| `--prefetch-trace` | 空 | CSV 路径 | 输出 post-expand 页排序、提交和 useful/wasted 标签 |
| `--cache-policy` | `graph-aware-2q` | `none`、`lru`、`2q`、`graph-aware-2q`、`agent` | page cache 淘汰策略 |
| `--cache-pages` | `0` | `>=0` | `0` 且 cache policy 非 `none` 时按内存预算自动计算 |
| `--memory-budget-ratio` | `0.20` | `(0,1]`，验收推荐 `0.10-0.20` | resident memory 预算占原始向量字节比例 |
| `--protect-hot-pages` | `1` | `0/1` | 是否保护热页；graph-aware 策略会利用 hub 入度信息 |
| `--hot-degree-threshold` | `0` | `>=0` | hub 热度阈值；`0` 表示由策略默认处理 |

### 预取 A/B 对比实验


```bash
python3 scripts/agent_aware.py ab --index indexes/sift.idx \
  --query-offset 5000 --query-limit 1000
```

脚本运行 `no_prefetch`、`topology_fixed`、`topology_adaptive` 三臂，默认在 cold/hot cache 下各执行 5 个 seed。`report.json` 只有在 P95/P99、Recall、demand wait、waste、读放大和 planner CPU 门槛全部满足时才输出 `acceptance_ok=true`。

### 预取 trace / 回放（可选）


```bash
# topology 与 oracle 离线对比
python3 scripts/replay_prefetch_policy.py \
  --trace logs/topology.csv --top-k 2 --output build/replay.json

# 自动 smoke
ctest --test-dir build --output-on-failure
```

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
| `--insert_ratio` | `1.0` | `>=0` | 写事件内 insert 比例，与 update/delete 比例归一化 |
| `--update_ratio` | `0.0` | `>=0` | 写事件内 update 比例 |
| `--delete_ratio` | `0.0` | `>=0` | 写事件内 delete 比例 |
| `--enable_flush` | `1` | `0/1` | 是否允许自动 flush 并在结尾 flush |
| `--enable_compaction` | `0` | `0/1` | 是否执行一次测量型 compaction |
| `--compaction_background` | `0` | `0/1` | 是否在 workload 运行期间周期性后台 compaction |
| `--compaction_interval_ms` | `1000` | `>0` | 后台 compaction 周期 |
| `--base_count` | `1000` | `>0` | synthetic base 数量 |
| `--query_count` | `256` | `>0` | synthetic query 数量 |
| `--dim` | `32` | `>0` | synthetic 维度 |
| `--memtable_flush_bytes` | `262144` | `>0` | MemTable flush 阈值 |
| `--dynamic_graph_degree` | `16` | `>0` | delta graph 邻居数 |
| `--write_queue_capacity` | `4096` | `>0` | 有界 mutation 队列容量；满时 writer 阻塞并统计 backpressure |
| `--wal_group_max_records` | `64` | `>0` | 单次 group commit 最大记录数 |
| `--wal_group_max_delay_us` | `1000` | `>=0` | coordinator 收集批次的最大等待时间 |
| `--version_index_shards` | `64` | `>0` | 动态版本索引 shard 数 |
| `--neighbor_candidate_limit` | `1024` | `>0` | 写入准备阶段的 Delta 邻居候选上限 |
| `--search_width` | `350` | `>0` | SSD graph 搜索扩展预算 |
| `--entry_count` | `64` | `>0` | SSD graph entry 数 |
| `--beam_width` | `16` | `>0` | SSD graph beam batch width |
| `--io_mode` | `pread` | `pread`、`odirect`、`io_uring` | SSD graph I/O 模式 |
| `--io_batch` | `16` | `>0` | I/O batch size |
| `--io_depth` | `32` | `>0` | I/O depth |
| `--cache_policy` | `graph-aware-2q` | `none`、`lru`、`2q`、`graph-aware-2q` | SSD graph cache policy |
| `--cache_pages` | `0` | `>=0` | cache page 数 |
| `--pq_subspaces` | `32` | `>0`，必须匹配 index 的 neighbor PQ code bytes | SSD graph ADC 使用的 PQ 子空间数 |
| `--pq_centroids` | `256` | `2-256` | PQ codebook 每个子空间的中心数 |
| `--pq_train_limit` | `100000` | `>0` | 场景启动前训练 PQ 的 base 采样上限 |
| `--pq_iterations` | `8` | `>0` | PQ k-means 迭代轮数 |
| `--rerank_topk` | `100` | `>=topk` | PQ 候选的全精度 rerank 池大小 |
| `--seed` | `42` | `>=0` | 随机种子 |

## 结果展示

### SIFT1M SSD 主路径示例

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
| `stats.distance_direct_calls`, `stats.rerank_reads` | 零拷贝距离计算和 rerank 统计 |

### 混合读写 CSV 示例

示例命令：

```bash
./build/bench_mixed_rw \
  --data_path data/sift/sift_base.fvecs \
  --base_count 1000000 \
  --query_count 256 \
  --index_path indexes/sift.idx \
  --num_operations 1000 \
  --topk 10 \
  --search_width 156 \
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
| `benchmark_config` | 本次实验的数据、索引、线程数、search/beam width、PQ 参数与训练耗时、Recall 抽样、compaction 和 git 状态 |
| `benchmark_config.pq_adc_enabled`, `effective_pq_subspaces` | SSD mixed-RW 是否成功注入 PQ/ADC，以及模型实际 subspaces |
| `benchmark_config.pq_code_bytes`, `pq_codebook_bytes`, `pq_training_ms` | 共享 PQ 模型的内存字节数和场景外训练耗时 |
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

1. 只加载 query 和可选 truth；base 不进入在线 benchmark 进程。
2. 打开 V3 索引并恢复 PQ offsets、codebooks 和全量 codes。
3. 索引不存在、格式不匹配或 manifest 校验失败时直接失败，提示先运行 `agent-aware-build`。
4. 根据 `--memory-budget-ratio` 估算 resident memory，并在未手动设置 `--cache-pages` 时自动计算 cache 容量。
5. 配置 `PackedDiskGraphIndex` 的 cache policy 和 I/O mode，从 manifest 读取 single-medoid。
6. 每个 query 构造 `DiskGraphSearchConfig`，执行 graph beam search。
7. 搜索优先命中 BufferPool；miss 时通过 `pread`、`odirect` 或 `io_uring` 读取 4KB page。
8. 命中 page 后直接从 page buffer 计算 L2 距离，并用 PQ/rerank 更新候选队列。
9. 开启 topology 策略时，在 post-expand 阶段聚合候选页；仅真实 io_uring 后端提交异步预取。
10. 聚合 Recall、QPS、延迟、缓存、I/O、预取和内存预算统计，写入 JSON。

### 动态写入流程

1. `DynamicWriteManager::open` 创建或恢复 manifest、WAL、MemTable 和 SSTable reader。
2. `insert/update/erase` 在调用线程准备记录并进入有界 MPSC 队列，commit coordinator 按最多 64 条或 1000us 组成批次。
3. coordinator 按出队顺序分配 sequence，批量追加 WAL 并执行一次 `fdatasync`，然后更新 MemTable 和 64-shard 版本索引，最后发布批次 `latest_sequence`。
4. MemTable 达阈值后原子切换并轮转编号 WAL segment，后台 flush worker 写 SSTable；manifest 只在提交点短暂加锁更新。
5. Compaction 锁内捕获输入 generation、锁外生成 SSTable、锁内校验并原子提交，避免持锁执行全量文件合并。
6. 查询时，`PackedGraphEngine` 先固定 `read_sequence`，再查 SSD base graph 和该 sequence 下的 update/delete/Delta，最后合并 Top-K，不会看到半个 group commit。
7. `bench_mixed_rw` 支持 insert/update/delete 比例并输出 queue、batch、WAL、MemTable、flush 和 coordinator 指标。

## 重要接口

### 数据与基础类型

```cpp
#include "data/dataset.h"
#include "core/types.h"

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
#include "core/pq_encoder.h"

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
#include "graph/disk_graph_index.h"

agent_aware::DiskGraphBuildConfig build;
build.degree = 32;
build.page_size = 4096;
build.pq_model = &pq;
agent_aware::PackedDiskGraphBuilder::build(base, "indexes/demo.idx", build);

agent_aware::PackedDiskGraphIndex index("indexes/demo.idx");
index.configure_cache("graph-aware-2q", 4096);
index.configure_io("io_uring", 16, 32);

agent_aware::DiskGraphSearchConfig search;
search.top_k = 10;
search.search_width = 156;
search.beam_width = 16;
search.entry_count = 64;
auto result = index.search_one(query, search);
```

### StorageEngine 集成接口

```cpp
#include "engine/storage_engine.h"

agent_aware::PackedGraphEngineConfig config;
config.index_path = "indexes/demo.idx";
config.cache_policy = "graph-aware-2q";
config.cache_pages = 4096;
config.io_mode = "pread";
config.search.search_width = 156;
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
#include "dynamic/dynamic_write_manager.h"

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
| 验证动态写入 | 调整 `--read_ratio`、`--write_ratio`、`--memtable_flush_bytes` | `write_qps`、`flush_duration_ms`、`recovery_time_ms` |

## 开源参考与合规说明

本项目在技术路线、系统设计思路和实验组织方式上参考了开源项目 [Agent-Mem-IO](https://github.com/ouyangyipeng/Agent-Mem-IO)。参考内容包括PQ 编码、Graph-Aware 2Q 缓存策略。

本项目没有直接复制 Agent-Mem-IO 的源代码和文档；核心代码由本队围绕赛题目标重新设计、实现、测试和验证。若后续引入第三方代码、文档或数据资产，将在对应文件、文档或提交说明中明确标注来源、使用目的和授权信息。项目将遵守所参考项目及其他第三方资源对应的开源协议要求。

## AI 工具使用说明

本项目开发过程中使用了 ChatGPT、OpenAI Codex / Codex CLI 等 AI 工具进行辅助。AI 主要用于赛题理解、方案讨论、代码结构分析、错误定位、重构建议、实验结果整理，以及 README 和设计文档草稿生成。

AI 生成内容均经过参赛队成员人工审核、修改、编译测试和实验验证。项目的最终方案、代码合并、实验运行、结果判断和答辩内容由参赛队成员负责。详细说明见 [docs/AI_USAGE_DISCLOSURE.md](docs/AI_USAGE_DISCLOSURE.md)。
