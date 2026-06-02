# AgentMem-Flow V6/V8 AutoDL 部署说明

## 目标

V6/V8/V9 的最终实验环境建议放在 AutoDL Linux 服务器上运行，用真实文件 I/O compaction 替代 V5 的时间片模拟，并用 V9 的 FreshLSH-Vamana + FreshVamana + StreamMerge 路径支撑 SIFT100K/SIFT1M。重点验证：

- Delta 写入是否能通过 WAL 持久化。
- 查询是否能同时合并 Main Index 和 Delta Index。
- aggressive compaction 是否会放大查询 P99。
- SLA-aware compaction 是否能在真实文件 I/O 干扰下控制查询尾延迟。
- `lsh-rp` 建图是否能替代 O(N^2) exact builder 完成 SIFT100K/SIFT1M 开发实验。
- 删除 tombstone、WAL delete record 和 StreamMerge 是否能在动态 workload 中闭环。
- early-stop 能否在 Recall@10 达标时降低 P99 和 SSD reads/query。
- 实验结果是否能完整归档，便于答辩复现。

## 推荐服务器环境

建议配置：

- Ubuntu 20.04/22.04。
- 8 核 CPU 或以上。
- 32 GB 内存或以上；SIFT1M 建议 64 GB 或以上。
- 本地 SSD 数据盘，优先把仓库和数据放在 AutoDL 的数据盘目录，例如 `/root/autodl-tmp` 或 `df -h` 显示的高速数据盘。
- `g++` 支持 C++17。

GPU 不是本项目必需资源，除非后续加入 GPU embedding 或 GPU brute force。

## 首次部署

在 AutoDL 终端中：

```bash
cd /root/autodl-tmp
git clone <your-repo-url> agent-aware
cd agent-aware
bash scripts/linux/build.sh
bash scripts/autodl/collect_env.sh
```

如果不是通过 git 上传，也可以用 `scp` 或 AutoDL 文件管理器上传整个仓库，然后在仓库根目录执行同样命令。

如果通过 zip 上传，脚本可执行位可能丢失。推荐先执行：

```bash
find scripts -name "*.sh" -exec chmod +x {} \;
```

当前脚本内部已经使用 `bash script.sh` 调用子脚本，即使忘记 `chmod +x` 也能运行；但补上可执行位更便于手动执行。

## V6 Synthetic 正式实验

Warm run：

```bash
cd /root/autodl-tmp/agent-aware
DATE_TAG=v6-warm bash scripts/autodl/run_v6_synthetic.sh
```

Strict cold run：

```bash
cd /root/autodl-tmp/agent-aware
bash scripts/autodl/drop_caches.sh
DATE_TAG=v6-cold RUN_TYPE=cold WARMUP_RUNS=0 bash scripts/autodl/run_v6_synthetic.sh
```

重要参数可通过环境变量覆盖：

```bash
BASE_COUNT=10000 \
QUERY_COUNT=1000 \
OPERATION_COUNT=2000 \
WRITE_RATIO=20 \
COMPACTION_IO_BYTES_PER_VECTOR=65536 \
QUERY_SIGNATURE_POLICY=simhash-pq \
SIMHASH_BITS=16 \
PQ_PREFIX_SUBSPACES=4 \
PQ_PREFIX_CENTROIDS=16 \
DELTA_INDEX_POLICY=ivf-flat \
DELTA_IVF_CENTROIDS=64 \
DELTA_IVF_PROBES=16 \
DELTA_IVF_TRAIN_ITERATIONS=8 \
DELTA_IVF_REBUILD_INTERVAL=64 \
SLA_P99_MS=2.0 \
DATE_TAG=v6-large \
bash scripts/autodl/run_v6_synthetic.sh
```

输出位置：

```text
archive/results/v6-autodl-synthetic-*.txt
archive/logs/v6-autodl-synthetic-*.log
archive/configs/v6-autodl-synthetic-*.json
archive/build_info/v6-autodl-*.txt
```

## SIFT100K / SIFT1M 实验模板

准备官方数据：

```text
sift_base.fvecs
sift_query.fvecs
sift_groundtruth.ivecs
```

运行方式：

```bash
cd /root/autodl-tmp/agent-aware
export SIFT_BASE=/root/autodl-tmp/sift/sift_base.fvecs
export SIFT_QUERY=/root/autodl-tmp/sift/sift_query.fvecs
export SIFT_TRUTH=/root/autodl-tmp/sift/sift_groundtruth.ivecs
BASE_LIMIT=100000 QUERY_LIMIT=1000 BUILD_INDEX=0 INDEX=/root/autodl-tmp/index/sift100k.idx \
DATE_TAG=sift100k-v6 \
bash scripts/autodl/run_v6_sift_template.sh
```

当前 SIFT 模板默认使用 V9 的 FreshLSH-Vamana 建图路径：

- `GRAPH_BUILD_POLICY=lsh-rp`
- `PACKING=bfs`
- `GRAPH_DEGREE=32`
- `SEARCH_WIDTH=1024`
- `SEARCH_EARLY_STOP=1`
- `SEARCH_EARLY_STOP_MIN=192`
- `ENTRY_COUNT=256`
- `ROUTING_SAMPLE_COUNT=4096`
- `LSH_TABLES=8`
- `LSH_BITS=14`
- `LSH_BUCKET_LIMIT=64`
- `ROBUST_PRUNE_ALPHA=1.2`

因此 SIFT100K 可以先尝试 `BUILD_INDEX=1` 现场建图。SIFT1M 仍建议先在 AutoDL 数据盘上单独构建并保留 index，后续查询实验使用 `BUILD_INDEX=0 INDEX=<prebuilt.idx>` 读取，避免每次重复建图。

只有小规模 sanity check 才建议显式设置 `GRAPH_BUILD_POLICY=exact` 或 `approx-rp`。Exact builder 复杂度接近 O(N²)，V8 `approx-rp + RobustPrune` 在较大候选集下仍偏重；SIFT100K/SIFT1M 正式运行优先使用 `lsh-rp`。

FreshVamana + LSM-style StreamMerge 动态实验：

```bash
BASE_LIMIT=100000 \
QUERY_LIMIT=1000 \
BUILD_INDEX=1 \
WORKLOAD_MODE=mixed \
OPERATION_COUNT=5000 \
WRITE_RATIO=20 \
DELETE_RATIO=50 \
COMPACTION_POLICY=stream-merge \
STREAM_MERGE_INDEX=/root/autodl-tmp/index/sift100k_streammerge.idx \
DATE_TAG=sift100k-v8-fresh \
bash scripts/autodl/run_v6_sift_template.sh
```

StreamMerge 写出的新 LTI 不会在同一进程热切换。要验证合并后的 index，请在后续运行中设置：

```bash
BUILD_INDEX=0 INDEX=/root/autodl-tmp/index/sift100k_streammerge.idx \
WORKLOAD_MODE=read-only OPERATION_COUNT=0 COMPACTION_POLICY=none \
DATE_TAG=sift100k-v8-merged \
bash scripts/autodl/run_v6_sift_template.sh
```

SIFT1M 必须优先使用官方 `.ivecs` 做 ground truth，不建议依赖 V0 暴力搜索。

## Cold / Warm 口径

Warm run：

- 使用 `RUN_TYPE=warm`。
- 至少 `WARMUP_RUNS=1`。
- 体现缓存预热后的稳态表现。

Cold run：

- 执行 `bash scripts/autodl/drop_caches.sh`。
- 使用 `RUN_TYPE=cold WARMUP_RUNS=0`。
- 体现磁盘 I/O 压力。

AutoDL 通常以 root 身份运行容器，如果 `drop_caches.sh` 失败，需要检查当前用户权限。
部分 AutoDL 容器即使显示为 root，也可能禁止写 `/proc/sys/vm/drop_caches`。此时脚本会提示 `cold-like` 并继续运行；报告中必须标注该结果不是 strict cold。

## 打包答辩证据

运行完实验后：

```bash
DATE_TAG=v6-final bash scripts/autodl/package_archive.sh
```

生成：

```text
archive/v6-autodl-evidence-v6-final.tar.gz
```

这个压缩包包含计划书、验证方法、版本报告、配置、日志、结果和环境信息。

## 当前 V6 限制

- V6 已支持真实文件 I/O compaction，但还不是完整 io_uring/O_DIRECT 后台合并。
- Delta 可使用 flat 或 V5.2 的 IVF-flat；大规模动态写入仍需要继续调 rebuild interval 或后续升级 HNSW。
- V9 已提供 `lsh-rp` 建图和 FreshVamana 删除 patch，但 SIFT1M 的正式 cold/warm 仍需在 AutoDL/WSL2 上实测并归档。

## 推荐验收标准

- Synthetic V6 warm/cold 均能完成归档。
- `compaction_io_bytes > 0`。
- `wal_records == insert_count`。
- Recall@10 不低于 0.95。
- SLA-aware compaction 的 P99 低于 aggressive compaction。
- SLA-aware compaction 的 `compaction_interference_ms_per_query` 低于 aggressive compaction。
- V8 StreamMerge 实验中 `delete_count == tombstone_count`。
- V8 StreamMerge 实验中 `wal_records == insert_count + delete_count`。
- V8 StreamMerge 实验中 `stream_merge_ops > 0` 且输出的新 LTI 可被下一次运行加载。
- V9 SIFT100K/SIFT1M 实验中 `graph_build_policy=lsh-rp`，并记录 early-stop 前后对照。
