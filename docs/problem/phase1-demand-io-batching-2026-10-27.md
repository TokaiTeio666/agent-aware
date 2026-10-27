# Phase 1：demand 读批处理与分阶段 I/O 归因

> 日期：2026-10-27
> 环境：同 Phase 0（VMware Ubuntu 26.04 / 内核 7.0 / GCC 15.2 / 8 vCPU / 12 GB），
> 仓库副本 `/data/agent-aware-new`，数据盘 `/data/work`
> 基线提交：`783de3b`（"默认关闭入度扫描，移除 ADC 硬编码"）
> 前序：[`phase0-measurements-2026-10-26.md`](phase0-measurements-2026-10-26.md) §5 待办 #6、§12

---

## 0. 这一阶段要回答什么

Phase 0 §12 给出的待修项是：

> 修 `expand_candidate_batch` 绕过批处理的问题：它对每个候选逐个 `load_node`，
> 导致 `--io-depth` / `--io-batch` 在热路径上无效，每页一次 `io_uring_submit`
> + 一次 `wait_cqe`。这是 §12 定位的最大优化空间，属 Phase 1。

Phase 0 §12 还给出了定量预期："213 次串行往返可以压成约 14 次，等待时间有实质性
下降空间。"

Phase 1 要做三件事，并且每一件都必须有数字：

1. 让扩展路径的 demand 读真正成批提交；
2. 把 demand 读**按搜索阶段**归因（第几次查询才读的、读在哪个阶段、是页多还是
   往返多），因为 Phase 0 已经证明"总耗时"字段会误导（§12 陷阱 1）；
3. 证明批处理**不改变结果**（top-k、召回、读页数逐位一致），并让 syscall 数成为
   可回归的契约（新测试 `demand_io_batching`）。

---

## 1. 实测：修复前 vs 修复后

真冷（`vmtouch -e` + `--protect-hot-pages 0 --require-cold-cache 1`）、1M、2000 条查询、
`io_uring`/O_DIRECT、`cache-policy none`、单线程。新旧两个二进制交替跑 3 轮。

| 指标 | 修复前（`783de3b`） | 修复后 | 变化 |
| --- | --- | --- | --- |
| Recall@10 | 0.988 / 0.988 / 0.988 | 0.988 / 0.988 / 0.988 | **不变** |
| 平均延迟 | 30.10 / 29.84 / 37.98 ms | **13.50 / 13.60 / 13.37 ms** | −55% ~ −65% |
| p95 | 40.8 / 40.06 / 59.7 ms | **18.05 / 18.01 / 18.13 ms** | −55% |
| p99 | 56.2 / 55.6 / 83.8 ms | 24.2 / 25.0 / 24.6 ms | −56% |
| 物理读 / 逻辑读（每查询） | 873,742 B / 213.3 页 | **873,742 B / 213.3 页** | **不变** |
| 逻辑→物理比 | 1.000 | 1.000 | 不变 |
| `io_submit_syscalls` | 426,632（= 页数） | **37,581** | **−91%** |
| 每批页数（`pages_per_read_batch`） | 1.000 | **11.35** | |
| 扩展节点 / 节点读 | 123.2 / 213.3 | **123.2 / 213.3** | **不变** |
| 峰值 RSS / 数据集 | 6.83% | 6.85% | 不变 |
| CPU 占比（`time -v`） | 44%–53% | **97%** | 等待占比从 ~50% 降到 ~3% |

三个箭头都是同一个结论：**磁盘上读的字节、读的页、扩展的节点一个没变，变的是
这些读被组织成多少次往返**。批次从 1 页涨到 11.35 页，syscall 从 21 万次降到
1.9 万次，查询耗时减半，CPU 占满。

> **p99 的尾有噪声**。提交版本的追加复跑里出现过一次 `p99 = 58.8 ms`
> （`p95 = 19.27 ms`，CPU 85%），紧接着一轮回到 `23.34 ms`（CPU 97%）；
> 同一时段旧二进制的 p99 是 56.6 ms。磁盘偶发的长尾对新旧两边同样生效，
> 均值与 p95 在 6 轮里稳定，p99 单轮不可作结论。

> 与 Phase 0 的数字对照：Phase 0 §4.5 的正式结果是 19.76 / 27.07 ms。同一台 VM
> 上，修复前的二进制本次复现出 30 ms，说明 VM 当前更慢（Phase 0 当时的
> `/usr/bin/time -v` 原始记录已不在 `/data/work` 上，无法核对）。所以本文**只做
> 同机同日、交替顺序的新旧对照**；跨日数字（19.76 vs 13.5）不作结论。

### 1.1 为什么是 13.5 ms：把等待拆开看

同一个二进制新增的两个字段：`demand_io_wait_us`（阻塞在 `wait_cqe` 的窗口）与
`demand_post_cpu_us`（CQE 之后的 `decode_page` / `store_cached_page` /
`materialize_page`）。旧的 `demand_read_wait_us` 保留原口径（= 两者 + 提交 + 中转），
用于和 Phase 0 对齐。

| 项（每查询） | 修复前 | 修复后 |
| --- | --- | --- |
| 平均延迟 | 30.10 ms | 13.50 ms |
| `demand_read_wait_us`（旧口径） | 26.93 ms | 12.08 ms |
| `demand_io_wait_us` | —（无字段） | **8.66 ms** |
| `demand_post_cpu_us` | — | **1.35 ms** |
| 剩余（距离计算 / ADC / 扩展逻辑） | ~3.2 ms | ~3.5 ms |

修复前旧口径 26.9 ms 里，Phase 0 已判定"含解码 CPU"，所以那 26.9 ms 的真实构成
没法再拆；修复后能拆了：**8.66 ms 真在等盘，1.35 ms 在解码物化**。

### 1.2 阶段归因：读发生在哪一步

每查询：

| 阶段 | 页 | 批 | syscall | 阻塞等待 | 解码 CPU |
| --- | --- | --- | --- | --- | --- |
| `entry_anchor`（锚点探测） | 3.98 | 1.00 | 1.00 | 0.11 ms | 0.03 ms |
| `candidate_expand`（本批候选所在页） | **0** | 0 | 0 | 0 | 0 |
| `neighbor_rank`（邻居页） | **209.3** | 17.8 | 17.8 | 1.53 ms | 1.31 ms |
| `rerank`（top-k 重排） | **0** | 0 | 0 | 0 | 0 |

各阶段页数之和 = `page_reads` = `submitted_reads`，恒等式由 `demand_io_batching`
测试与 `contract_smoke` 双重断言。

**这张表推翻了一个假设**：Phase 0 §12 认为单页读来自"逐个候选 `load_node`"。
实测 `candidate_expand` 阶段读 0 页——候选所在页在进入扩展批之前已经被读进来
（前一轮作为"邻居"读过了）。真正的单页读全部发生在 `neighbor_rank`：每个邻居
逐个算全精度距离时，页不在会话里就落进 `read_demand_page`。修复点因此不是
"候选批次预读"，而是**邻居页预批**。

> `rerank` 为 0 是本配置的下游效应，不是这个阶段不工作：`rerank_topk = 10` 时
> 重排候选页在扩展阶段已全部物化。测试里用 600 点的极小索引（rerank 候选有相当
> 比例没被扩展过）确认了 `rerank` 分支本身有读、且归因正确。

---

## 2. 改了什么

### 2.1 邻居页预批（`disk_graph_index.cpp` `expand_candidate_batch`）

三阶段拆分，**节点处理顺序、`visited` 插入顺序、候选堆的插入顺序完全不变**：

1. 逐个候选打开节点（页已在手，不产生 I/O），同时记录每个新邻居"是否跨页"；
2. 把所有**跨页邻居**的页合成一个列表，一次 `ensure_pages_loaded_batch`
   （按 `demand_batch_limit()` = 16 页切块）。同页邻居本来就在手，不参与预读；
3. 再逐个邻居算全精度距离——此时页都在手，零 I/O。

第 3 步期间 `session.set_reading_neighbor_pages(true)`，只为归因：万一预读后
仍有漏网的按需读（预取竞争时缓冲池不足等），它应记在 `neighbor_rank` 而不是
`entry_anchor`。

### 2.2 锚点批探测（`initialize_search_state`）

撒点 4096 个、取 4 个最近锚点做精确距离；这 4 个点几乎不可能同页，原先各走
一次单页提交。现在先一次性批量载入它们的页，再逐个算距离。每查询省 3 次往返，
收益小（0.1 ms 级），但和邻居预批口径一致。

### 2.3 rerank 一次提交（`finalize_topk`）

原先按固定 2 页一块切，10 个候选要 5 趟往返；改为 32 页一块（`kRerankChunkPages`），
候选数不超过 32 时就是一趟。

### 2.4 分阶段归因与计时拆分（`query_page_session.cpp`）

- `DemandReadStage`：`entry_anchor` / `candidate_expand` / `neighbor_rank` / `rerank`；
  `DemandReadStageStats`：`pages_read`、`batches`、`syscalls`、`wait_calls`、
  `io_wait_us`、`post_cpu_us`。
- 批量路径：`io_wait_us` 只计"`reap_async_read(true)` 阻塞"的时间，`post_cpu_us`
  计每个 CQE 的解码/存储/物化时间。单页路径：`io_wait_us` 保持旧口径
  （提交 + 等待，单页提交本身不含 CPU 工作），解码单独计。
- 旧字段 `demand_read_wait_us` 语义**不变**（提交 + 回收 + 解码），
  Phase 0 的数字仍可对照，不再被当作 I/O 等待使用。

### 2.5 候选堆计数

`candidate_pushes` / `candidate_pops` / `candidate_corrections`。原实现里候选堆的
入堆/出堆完全不可观测，§12 里"邻居在页里就地精确化，不必弹出再塞回"这一设计
是否真的成立只能靠读代码判断。实测 1M：`pushes / pops = 6.09`、`corrections = 0`，
即**没有任何一次"近似值被弹出后改正再塞回"**，每个候选平均只被压入 6 次
（种子 + 6 轮邻居），设计按预期工作。

### 2.6 删掉的死代码

`select_candidate_batch` 及其写入的 `beam_unique_pages` / `beam_cached_pages`
从未被调用，恒为 0。留着会让人误读成"同页复用为 0"的实测结论。已删除，
`stats_schema_version` 升到 2，`contract_smoke` 断言这两个键不存在。

### 2.7 回归测试 `tests/demand_io_batching.cpp`

600 点合成聚类底库、PQ、degree 16、4096 字节页；同一索引在 `pread` 与 `io_uring`
两种模式下各用**新开**的 index 对象跑同样 5 条查询，断言：

1. 归因恒等式：各阶段页数之和 == `submitted_reads`，且 `neighbor_rank > 0`；
2. 结果不变：两种模式 top-k 的 id 与距离**逐位相同**，读页数相同；
3. 批量生效（仅 io_uring）：`io_submit_syscalls < page_reads`。

本机输出：`page_reads=131 io_submit_syscalls=20 read_batches=20
syscalls_per_page=0.153 stage_pages(entry/expand/neighbor/rerank)=20/0/111/0`。

---

## 3. 试过但没用的：CQE 批量 peek

批处理之后每批仍是 17.8 次 `wait_cqe`（37,581 次提交 → 42 万次回收）。按
"提交后请求在盘队列里并行，CQE 应成批到达"的直觉，在回收循环里先做非阻塞
`io_uring_peek_cqe`、命中一个属于本批的就直接处理，没有再阻塞等：

| 指标（3 轮） | 批处理 | 批处理 + peek |
| --- | --- | --- |
| 平均延迟 | 13.50 / 13.60 / 13.37 ms | 13.70 / 13.67 / 13.77 ms |
| `demand_io_wait_us` / 查询 | 8.66 / 8.76 / 8.45 ms | 9.02 / 8.99 / 9.05 ms |
| peek 命中数 | — | 421,865 / 421,725 / 421,926（≈ 每页 0.99 次） |
| syscall / 读页 / 召回 | 37581 / 426632 / 0.988 | 完全相同 |

peek 确实命中（几乎每页都命中），**但延迟没有变化**。原因：CQE 到达时间由磁盘
决定，peek 只是少了一次 `wait_cqe` 系统调用；真正支配时间的是"第一个请求落盘
到最后一个请求落盘"的间隔，而 peek 并不能让磁盘早一点回。

**结论：peek 不改等待。** CQE 到达时间由磁盘决定，peek 只是把"阻塞等下一个"
换成"先问一句有没有"，当没有就绪的 CQE 时仍要 `wait_cqe` 阻塞。它把一次
`wait_cqe` 摊成若干次 `peek_cqe` + 一次 `wait_cqe`，系统调用方向上是净增加。
按 Phase 0"负收益机制逐条记录"的惯例记录在此，**没有采纳**：测量代码与
`demand_cqe_peek_hits` 字段都已移除，上表数据来自当时的工作副本。

> 方法论备注：这类"看起来该有用"的改动，凡拿不出 ≥5% 的延迟差，就不进主路径。
> 延迟波动约 ±0.3 ms，而 peek 的影响 < 0.3 ms，属于噪声级。

---

## 4. 这条路径上还剩下什么

每查询 13.5 ms 中：

| 项 | 每查询 | 占比 | 能不能动 |
| --- | --- | --- | --- |
| 等盘（`demand_io_wait_us`） | 8.66 ms | 64% | 只与"读多少页 × 盘延迟"有关；页数 213.3 与扩展节点 123.2 是算法量 |
| 解码 + 物化（`demand_post_cpu_us`） | 1.35 ms | 10% | 1.3 ms / 213 页 = 6.3 µs/页，主要是把页解码成节点并物化；可以降到 4 µs/页级 |
| 其余 CPU（距离、ADC、扩展） | ~3.5 ms | 26% | 851 次 128 维距离 / 查询 |

**"213 次往返压成 14 次"的预期部分成立**：往返数是 17.8 + 1.0 = 18.8 次/查询
（不是 14，因为批次上限 16 页，每查询 213 页 ≈ 14 块，加上邻居预批只读跨页页、
有些块不满）。syscall 与 CPU 等待都已不再是瓶颈：`time -v` 报 CPU 97%，即
**99% 的可等待时间已经消除，剩下的 8.66 ms 是真实的盘延迟**。

要继续压低，只能减少"读多少页"或提高盘并发（多查询并行），而不是继续改批量
粒度。前者是 Phase 0 §12"减少页数：213 → ~123"那条线（每扩展 1.73 页 → 1.0），
属于搜索算法而非 I/O。

---

## 5. 复现

```bash
# 构建与回归（8/8，含新测试 demand_io_batching）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAGENT_AWARE_REQUIRE_LIBURING=ON
ninja -C build && cd build && ctest --output-on-failure

# 新旧交替冷读对照（旧二进制取自基线 783de3b 的构建目录）
for round in 1 2 3; do
  for v in old new; do
    [ $v = old ] && b=/data/agent-aware/build/agent-aware-search \
                 || b=/data/agent-aware-new/build/agent-aware-search
    vmtouch -q -e /data/work/idx1m_v3.idx
    /usr/bin/time -v $b --sift-dir /data/work/ds1m \
        --index-path /data/work/idx1m_v3.idx --io-mode io_uring \
        --cache-policy none --protect-hot-pages 0 --require-cold-cache 1 \
        --memory-budget-enforce 0 --query-limit 2000 \
        --output-json /data/work/p1_${v}_$round.json
  done
done
```

JSON 新增字段（`stats_schema_version = 2`，删除 `beam_unique_pages` / `beam_cached_pages`）：

- top-level：`pages_per_read_batch`、`demand_io_wait_us`、`demand_post_cpu_us`、
  `demand_read_batches`、`demand_cqe_peek_hits`、`candidate_pushes` / `pops` /
  `pushes_per_pop` / `corrections`（`demand_cqe_peek_hits` 属 §3 未采纳方案，最终版本不含）；
- `demand_read_by_stage`：四个阶段各一组 `pages` / `batches` / `syscalls` /
  `wait_calls` / `io_wait_us` / `post_cpu_us`。

---

## 6. 待办

1. ~~`expand_candidate_batch` 绕过批处理（Phase 0 §5 #6）~~ **已完成**，见 §1。
2. 解码/物化 1.35 ms/查询（6.3 µs/页）：`materialize_page` 每页对 6 个节点各建
   一份邻接 vector；可改为一页一份扁平数组 + 偏移。需先测量，不假设收益。
3. 每扩展 1.73 页：同页邻居没被当作候选（`visited` 按节点去重但每页 6 节点
   的局部性没被利用），这是减少读页数的主线，属搜索算法。
4. `candidate_pushes_per_pop = 6.09`：每轮扩展只"消费"压入候选中约 1/6，
   说明 beam 宽度 16 时大量候选压入后从未被弹出即收敛。是否可少压入（例如
   只压入邻居页里最近的 k 个）需要实验，不在此阶段动。
