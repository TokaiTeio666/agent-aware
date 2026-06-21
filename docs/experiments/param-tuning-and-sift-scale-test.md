# 参数调优与 SIFT 大规模测试计划文档

## 当前实现状态（2026-06-21）

本计划中的 SIFT1M 主路径验证已经完成一轮：当前结果显示 Recall@10 `0.9940`、resident ratio `0.199992`，并补充了纯读并发 baseline 与 mixed RW evidence。完整参数矩阵尚未系统扫描，后续应优先固定 index、分离 build/search 成本，并输出 search width、beam width、io depth、prefetch width、memory budget 的曲线。

| 项目 | 状态 | 当前结果 |
| --- | --- | --- |
| SIFT1M 主路径 | 已完成一轮 | `logs/sift_bench/codex_sift1m_once_20260620-022610/result.json` |
| 纯读并发 baseline | 已完成一轮 | `build/sift1m_readonly_t1.json` ~ `build/sift1m_readonly_t8.json` |
| mixed RW benchmark | 已完成一轮 | `build/sift1m_mixed_rw_no_recall_compaction_immutable_view.json` |
| dynamic Recall evidence | 已完成一轮 | `build/sift1m_dynamic_recall_immutable_view.json` |
| 系统参数矩阵 | 未完成 | 下一步按本计划批量扫描 |

## 1. 文档目标

本文档用于指导 `agent-aware` 项目在 SIFT 数据集上的参数调优与大规模测试工作。当前项目已经完成图索引、磁盘页读取、QueryPageSession、page cache / prefetch / O_DIRECT / io_uring 等模块化改造，下一阶段重点不再是继续无边界重构，而是通过系统化实验确认：

1. 当前实现是否真正具备 DiskANN 式近似最近邻检索能力；
2. Recall@10 是否能稳定达到 `>= 0.95`；
3. P50 / P95 / P99 延迟是否随参数变化呈现合理趋势；
4. 内存预算是否可控，`memory_budget_pass=1` 是否稳定；
5. SIFT1M 及更大规模数据下，索引构建、磁盘搜索、缓存命中、随机读放大是否符合预期。

---

## 2. 当前问题判断

当前阶段的核心风险不是“没有参数可调”，而是参数之间缺少成体系的测试矩阵。尤其是以下问题需要通过实验确认：

| 问题 | 现象 | 需要验证的方向 |
|---|---|---|
| search_width 过大 | search_width 可能被调到 1024 甚至更高 | Recall 是否只是靠暴力扩展候选堆堆出来 |
| 延迟不稳定 | P99 可能明显高于 P50/P95 | 是否存在大量随机小读、cache miss、同步等待 |
| io_uring 加速不明显 | 开启异步 I/O 后可能更慢 | 是否缺少批量提交、pending depth、下一跳预取 |
| 内存预算失败 | cache / visited / pinned page 可能超出预算 | 需要明确每部分内存账本 |
| SIFT 小规模有效但大规模不稳定 | 10K/100K 可跑，1M 甚至更大规模延迟变差 | 需要分阶段放大测试 |

---

## 3. 总体调优原则

参数调优应遵循以下顺序：

```text
先固定环境 → 再固定构建参数 → 再调搜索参数 → 再调缓存/I/O参数 → 最后做大规模压力测试
```

不要一开始同时修改所有参数，否则无法判断性能变化来自图质量、搜索宽度、缓存、I/O 还是线程调度。

推荐实验顺序：

1. 建立 SIFT100K 基线；
2. 调搜索参数，确认 Recall-延迟曲线；
3. 固定搜索参数后调图构建参数；
4. 固定图参数后调缓存与 I/O 参数；
5. 在 SIFT1M 上复跑最优配置；
6. 逐步扩大数据量或 query 数量，做 P99 和内存预算压力测试；
7. 输出最终推荐配置。

---

## 4. 测试环境规范

### 4.1 运行路径要求

最终性能测试必须在 WSL ext4 或原生 Linux 文件系统中进行，不建议使用 `/mnt/c` 或 `/mnt/d`。

推荐路径：

```bash
~/agent-aware
~/data/sift
~/data/sift_index
~/logs/sift_bench
```

原因：

- `/mnt/d` 属于 Windows 挂载路径，文件系统 I/O 额外开销较大；
- 随机读、O_DIRECT、mmap、page cache 行为可能与原生 Linux 不一致；
- P99 / QPS / I/O 延迟测试容易失真。

### 4.2 编译规范

建议使用 Release 编译：

```bash
cd ~/agent-aware
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

如果项目支持开关，建议分别测试：

```bash
-DAGENTMEM_ENABLE_IO_URING=ON
-DAGENTMEM_ENABLE_ODIRECT=ON
-DAGENTMEM_ENABLE_PREFETCH=ON
```

每次测试必须记录：

```text
git commit hash
build type
compiler version
CPU 型号
内存大小
磁盘类型
Linux / WSL 版本
是否位于 ext4
是否开启 O_DIRECT
是否开启 io_uring
```

---

## 5. 数据集阶段划分

### 5.1 阶段一：功能验证集

| 数据规模 | 目的 | 通过标准 |
|---|---|---|
| SIFT10K | 快速验证构建和搜索流程 | 程序无崩溃，Recall 计算正确 |
| SIFT100K | 初步调参 | Recall@10 能接近或超过 0.90 |
| SIFT1M | 正式测试 | Recall@10 >= 0.95，P99 可接受，内存预算通过 |

### 5.2 阶段二：压力测试集

如果已经具备更大数据，可以继续做：

| 数据规模 | 目的 |
|---|---|
| SIFT2M / SIFT5M | 验证扩展趋势 |
| SIFT10M | 验证真实磁盘 ANN 能力 |
| 多轮 SIFT1M | 验证结果稳定性 |

如果暂时没有 SIFT10M，不必强行扩大。优先把 SIFT1M 的 Recall、P99、内存账本做扎实。

---

## 6. 指标体系

### 6.1 必须输出的核心指标

| 指标 | 说明 | 目标 |
|---|---|---|
| Recall@1 | Top1 命中率 | 越高越好 |
| Recall@10 | Top10 召回率 | `>= 0.95` |
| P50 latency | 中位延迟 | 越低越好 |
| P95 latency | 尾部延迟 | 越低越好 |
| P99 latency | 极端尾部延迟 | 重点优化对象 |
| QPS | 每秒查询数 | 越高越好 |
| memory_budget_pass | 是否通过内存预算 | 必须为 1 |
| peak RSS | 进程峰值内存 | 必须记录 |
| page cache hit rate | 页缓存命中率 | 越高越好 |
| disk read count/query | 每 query 磁盘读次数 | 越低越好 |
| bytes read/query | 每 query 读取字节数 | 越低越好 |
| visited nodes/query | 每 query 访问节点数 | 越低越好 |
| distance computations/query | 距离计算次数 | 越低越好 |

### 6.2 推荐输出的辅助指标

| 指标 | 用途 |
|---|---|
| queue push/pop 次数 | 判断候选队列是否膨胀 |
| unique pages/query | 判断 page 去重效果 |
| prefetched pages/query | 判断预取是否过度 |
| prefetch useful ratio | 判断预取是否有效 |
| io submit batch size | 判断 io_uring 是否真正批量化 |
| io pending depth | 判断是否有异步并发 |
| cache eviction count | 判断 cache 是否太小 |
| rerank candidate count | 判断全精度 rerank 成本 |

---

## 7. 参数分类

### 7.1 图构建参数

| 参数 | 作用 | 推荐测试范围 |
|---|---|---|
| `R` | 每个节点最大邻居数 | 32 / 48 / 64 / 96 |
| `L_build` | 构建时搜索宽度 | 64 / 100 / 128 / 200 |
| `alpha` | RobustPrune 剪枝强度 | 1.1 / 1.2 / 1.4 |
| `entry_point` | 图搜索入口 | medoid / random / sampled medoid |
| `degree cap` | 磁盘页中最大邻居数量 | 与 R 对齐 |

调参原则：

- `R` 越大，Recall 越容易提升，但索引更大、I/O 更重；
- `L_build` 越大，图质量越好，但构建时间更长；
- `alpha` 太小可能剪枝过强，太大可能邻居冗余；
- 初期不要追求最低延迟，先让图质量达到可用水平。

推荐第一组构建参数：

```text
R = 64
L_build = 100 或 128
alpha = 1.2
```

---

### 7.2 搜索参数

| 参数 | 作用 | 推荐测试范围 |
|---|---|---|
| `search_width` | 搜索候选宽度 | 32 / 64 / 96 / 128 / 192 / 256 / 384 / 512 |
| `beam_width` | 每轮扩展宽度 | 4 / 8 / 16 / 32 |
| `topk` | 返回近邻数量 | 固定 10 |
| `rerank_k` | 全精度重排候选数 | 20 / 50 / 100 / 200 |
| `early_stop` | 提前停止策略 | on / off |
| `max_visited` | 最大访问节点数 | 1K / 2K / 4K / 8K |

调参原则：

- `search_width` 不应无脑调到 1024；
- 先画出 `search_width → Recall / P99` 曲线；
- 如果 512 才能到 0.95，说明图质量或候选扩展策略可能有问题；
- 如果 128~256 即可达到 0.95，说明图质量基本可用；
- `beam_width` 增大可能减少轮数，但会增加单轮 I/O 压力；
- `rerank_k` 太小会影响最终精排，太大会增加全精度读取成本。

推荐第一组搜索参数：

```text
topk = 10
search_width = 128
beam_width = 16
rerank_k = 100
early_stop = on
```

---

### 7.3 缓存参数

| 参数 | 作用 | 推荐测试范围 |
|---|---|---|
| `page_cache_size_mb` | 磁盘页缓存大小 | 64 / 128 / 256 / 512 / 1024 |
| `hot_page_cache` | 热点页缓存 | on / off |
| `path_cache` | 搜索路径缓存 | on / off |
| `cache_policy` | 淘汰策略 | LRU / Clock / TinyLFU |
| `pin_entry_pages` | 固定入口附近页面 | on / off |

调参原则：

- 先测试无 cache 或小 cache，得到真实磁盘读基线；
- 再逐步增大 cache，看 hit rate 与 P99 是否同步改善；
- 如果 cache 变大但 P99 没改善，可能是读取路径太分散；
- 如果 cache 命中率高但 Recall 下降，说明缓存逻辑可能影响了正确性；
- `pinned pages` 必须在 query 结束时释放或统一归还给内存账本。

推荐第一组缓存参数：

```text
page_cache_size_mb = 256
hot_page_cache = on
path_cache = off
pin_entry_pages = on
```

---

### 7.4 I/O 参数

| 参数 | 作用 | 推荐测试范围 |
|---|---|---|
| `use_odirect` | 绕过系统 page cache | on / off |
| `use_io_uring` | 异步 I/O | on / off |
| `io_depth` | 异步 pending 深度 | 1 / 4 / 8 / 16 / 32 |
| `batch_submit` | 批量提交 I/O | on / off |
| `prefetch_depth` | 下一跳预取深度 | 0 / 1 / 2 / 4 |
| `read_alignment` | O_DIRECT 对齐 | 4KB / page size |

调参原则：

- `io_uring` 只有在批量提交和 pending depth 足够时才可能收益明显；
- 如果仍然是一读一等，io_uring 可能比同步读更慢；
- `O_DIRECT` 需要保证 buffer、offset、size 对齐；
- 预取深度太大会造成无效读放大，反而拉高 P99；
- 优先统计 `prefetch useful ratio`，不要只看是否开启预取。

推荐第一组 I/O 参数：

```text
use_odirect = on
use_io_uring = off
prefetch_depth = 0
```

先建立同步 O_DIRECT 基线，再开启：

```text
use_odirect = on
use_io_uring = on
io_depth = 8
batch_submit = on
prefetch_depth = 1
```

---

## 8. 实验矩阵设计

### 8.1 第一阶段：搜索宽度扫描

固定构建参数：

```text
R = 64
L_build = 128
alpha = 1.2
page_cache_size_mb = 256
beam_width = 16
rerank_k = 100
```

扫描：

| 实验编号 | search_width |
|---|---|
| SW-01 | 32 |
| SW-02 | 64 |
| SW-03 | 96 |
| SW-04 | 128 |
| SW-05 | 192 |
| SW-06 | 256 |
| SW-07 | 384 |
| SW-08 | 512 |

记录：

```text
Recall@10
P50 / P95 / P99
QPS
visited nodes/query
disk reads/query
bytes read/query
memory_budget_pass
```

目标：

- 找到 Recall@10 第一次达到 0.95 的最小 search_width；
- 如果 512 仍然达不到 0.95，转向图构建参数优化；
- 如果 128 或 192 已达到 0.95，进入延迟优化。

---

### 8.2 第二阶段：图构建参数扫描

固定搜索参数为第一阶段得到的最小可用 search_width，例如：

```text
search_width = 128 或 192
beam_width = 16
rerank_k = 100
```

扫描：

| 实验编号 | R | L_build | alpha |
|---|---:|---:|---:|
| GB-01 | 32 | 64 | 1.2 |
| GB-02 | 48 | 100 | 1.2 |
| GB-03 | 64 | 128 | 1.2 |
| GB-04 | 64 | 200 | 1.2 |
| GB-05 | 96 | 128 | 1.2 |
| GB-06 | 64 | 128 | 1.1 |
| GB-07 | 64 | 128 | 1.4 |

记录：

```text
build time
index size
avg degree
Recall@10
P99
disk reads/query
```

目标：

- 找到图质量与索引大小的平衡点；
- 如果提高 R 明显提升 Recall 但 P99 明显恶化，需要考虑页布局和邻居压缩；
- 如果提高 L_build 对 Recall 提升明显，说明之前图构建搜索不足；
- 如果 alpha 变化影响很大，需要检查 RobustPrune 实现是否正确。

---

### 8.3 第三阶段：beam_width 与 rerank_k 扫描

固定：

```text
R = 最优值
L_build = 最优值
alpha = 最优值
search_width = 最小达标值
```

扫描：

| 实验编号 | beam_width | rerank_k |
|---|---:|---:|
| BR-01 | 4 | 50 |
| BR-02 | 8 | 50 |
| BR-03 | 16 | 100 |
| BR-04 | 32 | 100 |
| BR-05 | 16 | 200 |
| BR-06 | 32 | 200 |

目标：

- 找到 P99 最低且 Recall 不下降的组合；
- 如果 beam_width 增大导致 P99 增大，说明单轮 I/O 压力过高；
- 如果 rerank_k 增大 Recall 不变但延迟变高，应降低 rerank_k；
- 如果 rerank_k 太小导致 Recall 下降，说明粗排候选质量不足。

---

### 8.4 第四阶段：缓存参数扫描

固定最优图参数和搜索参数，扫描：

| 实验编号 | page_cache_size_mb | hot_page_cache | path_cache |
|---|---:|---|---|
| CA-01 | 64 | off | off |
| CA-02 | 128 | on | off |
| CA-03 | 256 | on | off |
| CA-04 | 512 | on | off |
| CA-05 | 256 | on | on |
| CA-06 | 512 | on | on |

目标：

- 观察 cache size 对 P99 的边际收益；
- 判断 path_cache 是否真的有效；
- 确认 memory_budget_pass 是否稳定；
- 找到内存预算内的最佳 cache 大小。

---

### 8.5 第五阶段：I/O 策略扫描

固定最优图、搜索、缓存参数，扫描：

| 实验编号 | O_DIRECT | io_uring | io_depth | prefetch_depth |
|---|---|---|---:|---:|
| IO-01 | off | off | 1 | 0 |
| IO-02 | on | off | 1 | 0 |
| IO-03 | on | on | 4 | 0 |
| IO-04 | on | on | 8 | 0 |
| IO-05 | on | on | 16 | 0 |
| IO-06 | on | on | 8 | 1 |
| IO-07 | on | on | 16 | 1 |
| IO-08 | on | on | 16 | 2 |

目标：

- 判断 O_DIRECT 是否降低系统 page cache 干扰；
- 判断 io_uring 是否真正降低 P99；
- 判断 prefetch 是否提高 cache hit 或减少等待；
- 若 io_uring 变慢，需要重点检查是否仍然同步等待、是否 batch submit 不足。

---

## 9. SIFT1M 正式测试流程

### 9.1 数据准备

建议目录：

```bash
~/data/sift/sift_base.fvecs
~/data/sift/sift_query.fvecs
~/data/sift/sift_groundtruth.ivecs
```

检查数据：

```bash
ls -lh ~/data/sift
```

需要确认：

```text
base 向量数量 = 1,000,000
query 数量 = 10,000
维度 = 128
ground truth topk >= 10
```

---

### 9.2 构建索引

示例命令，需根据项目实际 CLI 名称调整：

```bash
./build/agentmem_flow \
  --mode build \
  --dataset sift \
  --base ~/data/sift/sift_base.fvecs \
  --index ~/data/sift_index/sift1m_r64_l128_a12 \
  --R 64 \
  --L_build 128 \
  --alpha 1.2 \
  --page_size 4096
```

构建阶段记录：

```text
build_time_sec
index_size_gb
avg_degree
max_degree
page_count
nodes_per_page
```

---

### 9.3 搜索测试

示例命令：

```bash
./build/agentmem_flow \
  --mode search \
  --dataset sift \
  --query ~/data/sift/sift_query.fvecs \
  --groundtruth ~/data/sift/sift_groundtruth.ivecs \
  --index ~/data/sift_index/sift1m_r64_l128_a12 \
  --topk 10 \
  --search_width 128 \
  --beam_width 16 \
  --rerank_k 100 \
  --page_cache_size_mb 256 \
  --use_odirect 1 \
  --use_io_uring 0 \
  --output_json ~/logs/sift_bench/sift1m_baseline.json
```

正式测试至少跑三轮：

```bash
run_1
run_2
run_3
```

最终结果取：

```text
Recall@10: 平均值
P50/P95/P99: 每轮单独列出，同时给平均值
QPS: 平均值
memory_budget_pass: 必须全部为 1
```

---

## 10. 测试结果记录模板

### 10.1 参数记录表

| run_id | dataset | R | L_build | alpha | search_width | beam_width | rerank_k | cache_mb | odirect | io_uring | io_depth | prefetch |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|---:|---:|
| SW-01 | SIFT1M | 64 | 128 | 1.2 | 32 | 16 | 100 | 256 | on | off | 1 | 0 |

### 10.2 结果记录表

| run_id | Recall@1 | Recall@10 | P50 ms | P95 ms | P99 ms | QPS | reads/q | bytes/q | visited/q | peak RSS MB | memory_pass |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| SW-01 |  |  |  |  |  |  |  |  |  |  |  |

### 10.3 结论记录表

| 结论编号 | 发现 | 证据 | 下一步 |
|---|---|---|---|
| C-01 | search_width 从 128 到 256 Recall 提升明显，但 P99 增大 | SW-04 vs SW-06 | 检查图质量或 rerank_k |
| C-02 | io_uring 开启后变慢 | IO-02 vs IO-04 | 检查 batch submit 与 pending depth |

---

## 11. 验收标准

### 11.1 基础验收

项目进入下一阶段前，必须满足：

```text
SIFT1M Recall@10 >= 0.95
memory_budget_pass = 1
程序无崩溃、无死锁、无明显内存泄漏
结果可重复，三轮 Recall 波动 <= 0.005
```

### 11.2 性能验收

建议目标：

```text
P99 明显低于 search_width=1024 暴力扩展方案
search_width 不依赖 1024 才能达标
disk reads/query 随 cache 和 prefetch 优化下降
page cache hit rate 随 cache 增大合理上升
```

### 11.3 工程验收

需要形成以下文件：

```text
docs/experiments/param-tuning-and-sift-scale-test.md
docs/experiments/sift1m-mixed-rw-immutable-view.md
docs/changelog.md
scripts/linux/run_sift1m_once.sh
logs/sift_bench/*.json
build/sift1m_*.json
```

---

## 12. 推荐执行顺序

### Step 1：建立 SIFT100K 快速基线

目标：

```text
确认程序流程正确
确认 Recall 计算正确
确认日志字段完整
```

输出：

```text
sift100k_baseline.json
```

---

### Step 2：建立 SIFT1M baseline

推荐 baseline 参数：

```text
R = 64
L_build = 128
alpha = 1.2
search_width = 128
beam_width = 16
rerank_k = 100
cache_mb = 256
O_DIRECT = on
io_uring = off
prefetch_depth = 0
```

输出：

```text
sift1m_baseline.json
```

---

### Step 3：扫描 search_width

从 32 到 512 扫描，找到最小达标 search_width。

关键判断：

```text
如果 search_width <= 256 达到 Recall@10 >= 0.95：进入延迟优化
如果 search_width >= 512 才达到：优先优化图构建质量
如果 search_width = 512 仍不达标：检查 Vamana / RobustPrune / 磁盘页解码正确性
```

---

### Step 4：优化图构建参数

优先调整：

```text
R
L_build
alpha
```

不要先调 io_uring。因为如果图质量不够，I/O 优化只能加速错误路径，不能从根本上提升 Recall。

---

### Step 5：优化缓存和 I/O

顺序：

```text
先 page_cache_size
再 hot page / pinned entry pages
再 path cache
再 O_DIRECT
最后 io_uring + prefetch
```

如果 io_uring 开启后变慢，应检查：

```text
是否一读一等
是否 batch submit 太小
是否 io_depth 实际为 1
是否预取读放大
是否 O_DIRECT buffer 未对齐
```

---

### Step 6：三轮复现实验

最终参数确定后，至少跑三轮：

```text
final_run_1
final_run_2
final_run_3
```

输出最终表格：

```text
平均 Recall@10
P50/P95/P99
QPS
peak RSS
memory_budget_pass
```

---

## 13. 风险与应对

| 风险 | 表现 | 应对 |
|---|---|---|
| Recall 无法到 0.95 | search_width 很高仍不达标 | 检查 Vamana 构建、RobustPrune、ground truth、距离函数 |
| P99 极高 | 少数 query 随机读非常多 | 统计 worst query，输出 visited pages 和 reads |
| io_uring 更慢 | 开启后 P99 上升 | 检查 batch、pending depth、同步等待 |
| O_DIRECT 报错 | read 返回 EINVAL | 检查 buffer、offset、size 是否 4KB 对齐 |
| 内存预算失败 | RSS 超预算 | 分拆 cache、visited、pinned、rerank buffer 内存账本 |
| `/mnt/d` 性能异常 | P99 离谱 | 迁移到 WSL ext4 或原生 Linux |

---

## 14. 最终交付物

完成本计划后，应提交以下内容：

```text
1. 参数调优计划文档
2. SIFT1M baseline 测试结果
3. search_width 扫描结果
4. 图构建参数扫描结果
5. cache / I/O 参数扫描结果
6. 最终推荐参数表
7. 三轮复现实验结果
8. 性能瓶颈分析
9. 下一阶段优化建议
```

最终推荐参数表格式：

| 参数 | 推荐值 | 原因 |
|---|---:|---|
| R |  |  |
| L_build |  |  |
| alpha |  |  |
| search_width |  |  |
| beam_width |  |  |
| rerank_k |  |  |
| cache_mb |  |  |
| O_DIRECT |  |  |
| io_uring |  |  |
| io_depth |  |  |
| prefetch_depth |  |  |

---

## 15. 结论

本阶段的核心目标不是盲目追求复杂优化，而是通过 SIFT1M 建立可信的参数-性能曲线。只有当 Recall、P99、I/O 放大、缓存命中率、内存预算全部可解释时，后续继续做 io_uring、预取、PQ、页布局、图压缩才有意义。

优先级如下：

```text
第一优先级：SIFT1M Recall@10 >= 0.95
第二优先级：search_width 不依赖 1024
第三优先级：memory_budget_pass = 1
第四优先级：P99 稳定下降
第五优先级：io_uring / prefetch 真正带来收益
```
