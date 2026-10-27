# Phase 0：测量地基与首次可信测量

> 日期：2026-10-26
> 环境：VMware Ubuntu 26.04（内核 7.0）/ GCC 15.2 / 8 vCPU / 12 GB
> 数据盘：`/dev/sdb`（`8:16`，ext4，196 GB）/ 仓库 `/data/agent-aware`
> 构建：`cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS='-O3 -march=native -Wall -Wextra'`
> 前序：[`retrospective-2026-10-25.md`](retrospective-2026-10-25.md) §1.3「用调用次数冒充 I/O」

---

## 0. 这一阶段要回答什么

复盘 §1.3 的结论是：仓库里所有 I/O 数字都是 `pread` 调用计数，而数据集完全驻留在页缓存里，
所以那些数字测的是内存延迟。在修任何算法之前，必须先让"测出来的数是不是真的"这个问题有答案。

Phase 0 不优化任何东西，只建立三条硬前提：

| 前提 | 实现 | 验证 |
| --- | --- | --- |
| 物理 I/O 可测 | `/proc/diskstats` 按 `major:minor` 精确匹配 | `platform_io` 测试：O_DIRECT 读 32 MiB → 设备计数 32 MiB |
| 缓存状态可控可测 | `mincore` 驻留比例 + `POSIX_FADV_DONTNEED` / `vmtouch -e` | `platform_io` 测试：冷 0.000 → 热 1.000 |
| 内存预算是硬约束 | `VmHWM` 超预算 → 非零退出 | 预算不可满足时单独报错（退出码 3） |

输出 JSON 新增字段：`physical_read_bytes_per_query`、`logical_to_physical_read_ratio`、
`index_cache_residency_startup/before/after`、`index_fully_cached_during_run`、`query_details`
（单查询 recall 与 top-k，最多 256 条）。

---

## 1. I/O 计量基线

1M 向量（128 维，gmm-v1 合成数据，1000 簇），索引 704,771,256 B（672 MiB），
degree 32，每页 6 节点，172,064 个 4 KiB 页。每查询 500 条，top-10。

| 模式 | 页驻留（启动后） | 物理读/查询 | 逻辑→物理比 | 延迟 |
| --- | --- | --- | --- | --- |
| `io_uring`（O_DIRECT） | 1.000 | 464,486 B | **1.000** | 3.1–3.7 ms |
| `odirect` | 1.000 | 658,047 B（20k 索引） | 1.000 | 7.8 ms |
| `pread` | 1.000 | **0** | **0** | 0.48–0.51 ms |

结论：

- `io_uring` 与 `odirect` 路径**确实每次都落到设备**，物理读量等于逻辑读量，不受页缓存影响。
  这是仓库第一次能证明"这次实验测的是磁盘"。
- `pread` 路径在本次配置下**完全不走 I/O**：`physical_read_per_query = 0`，延迟 0.48 ms。
  此前 README 报的 1.14 ms 一类数字正属于这种口径。
  **（复核补充，见 §4.5）** 这里的"0"不是 pread 的特性，而是页缓存已被
  `ensure_node_in_degrees()` 填满的结果。真冷条件下（`--protect-hot-pages 0`）
  `pread` 的物理读是 2.23 MB/查询、延迟 6.69 ms，`logical_to_physical_read_ratio = 0.39`。
  所以 `pread` 不是"不走 I/O"，而是"大部分命中页缓存"——这正是它在真冷时
  比 `io_uring` 快的原因（它自己把缓存读热了）。

## 2. 冷/热对照

用 `vmtouch -e` 逐出全部 172,064 页、再 `vmtouch` 复核驻留为 0，然后计时。

| 场景 | 启动驻留 | 计时前驻留 | 物理读/查询 | 平均延迟 |
| --- | --- | --- | --- | --- |
| 冷（3 次） | 0.000 | 1.000 | 464,486 B | 3.08 / 3.33 / 3.66 ms |
| 热（2 次，无逐出） | 1.000 | 1.000 | 464,486 B | 3.01 / 3.66 ms |

**这是 Phase 0 最重要的发现**：索引构造之后、搜索开始之前，672 MiB 索引就已经全部驻留
—— `startup=0 → before=1`。但 `io_uring` 用 `O_DIRECT` 打开，**读路径完全绕过页缓存**，
所以冷/热对照对延迟几乎没有影响，`index_fully_cached_during_run = true` 也不代表"读来自内存"。

也就是说：

- 对于 `pread`/`odirect` 之外的路径，`index_cache_residency_before` 不是延迟的解释变量。
  **判断延迟来源应该看 `logical_to_physical_read_ratio`，而不是驻留比例。**
- ~~构造函数把整个索引扫一遍是不必要的 I/O（约 672 MiB），且会污染页缓存。~~
  **（复核修正，见 §4.5）** 构造函数只读 PQ 段（18 MB）和目录（775 KB），
  **没有**扫整个索引。真正把 672 MiB 读进页缓存的是
  `configure_cache` → `ensure_node_in_degrees()` 对全部 166,667 页的顺序读，
  且 `protect_hot_pages` 默认 true 使得 `--cache-policy none` 也会触发。
  这一项已列为待修项。

## 3. 1M 数据集暴露出召回故障

同一 1M 索引，`io_uring`/`pread` 两种模式、单线程/多线程、宽度 64/256/512、
top-10/top-1，**Recall@k 恒定在 0.0002**。20k 索引则报 0.877（benchmark 口径）。

排查过程与结论：

1. **数据与真值正确**：新增 `brute_force_check`，用 `search_memory_fast` 暴力算 top-10，
   对 `.ivecs` 真值 `recall = 1.000000`。所以 0.0002 不是数据问题。
2. **不是 ADC/PQ 近似**：`graph_probe` 关掉 ADC（`adc_enable=false`）结果与开启时**逐位相同**。
   当时的解释是"不存在按全精度打分的路径"，**这个解释是错的**：`search_one` 在
   `adc_enable=true` 且 `pq_model` 为空时会回落到索引自带的 PQ 模型，
   而探针原来把 `pq_model` 置空来"关 ADC"，两条分支其实走的是同一段代码。
   修正后（探针始终传索引的 PQ 模型）两组的差别仍然很小，真正的原因是
   下面第 4 条 —— 从入口可达的区域里根本不存在近邻。
3. **不是入口选择**：`graph_probe` 分别从索引入口 148523、真值 top-1、真值 top-3 出发，
   mean recall 分别是 0.00 / 0.09 / 0.18。**即使直接站在正确答案上出发也召不回。**
4. **距离是错的**：从真值点出发时，graph_top1 的 id 有时**就是正确答案**（q0–q3 全对），
   但报告的 `distance` 是 9.86 / 6.67 / 6.22 / 8.36，而暴力基线对同一点算出 2.86 / 2.54 / 2.70 / 2.78。
   同一个 id、同一份向量、同为平方 L2，差距 3 倍 —— **搜索结果携带的距离与其 id 不对应**。
5. **benchmark 的 recall 与自身数据一致**：新增 `query_details`（逐查询 top-k），
   用 `.ivecs` 重算 10 条查询全部 0.0，与报告的 0.0002 吻合。所以是引擎返回了错误的邻居，
   不是 recall 计算错了。
6. **索引本身没有损坏**：新增 `index_integrity`，通过引擎自己的 `get_node` 取回 90 个节点，
   id 与 128 维向量**逐位等于**底库原始向量（`worst|diff| = 0.000000`），
   邻居距离落在全库第 0.2 百分位（即确实是近邻）。问题既不在写入也不在读取层。
7. **索引是断开的**：同一个工具从入口点做 BFS，20k 索引可达 99.91%，
   1M 索引**只能到达 942 / 1,000,000（0.09%）**。Vamana 贪心搜索只能沿边前进，
   到不了的区域无论搜索宽度多大都召不回。这是决定性证据。
8. **即使从真值点出发也只召回 0.09**：说明 1M 的索引几乎由孤立的"簇"构成。

### 根因与修复

| 缺陷 | 位置 | 修复 |
| --- | --- | --- |
| 构造时只从 medoid 单点贪心，大规模下走不出局部区域，图碎成互不连通的小簇 | `vamana_builder.cpp` `search_for_construction` / `insert` | 构造搜索从 64 个均匀切片起点同时出发；每个节点额外把 32 个远点（beacon）放进候选池 |
| 结果集 `best` 直接收 ADC 近似距离，且永不重排 | `disk_graph_index.cpp` `expand_candidate_batch` | 结果集只接受全精度距离（节点打开时算，向量就在页里，不产生额外 I/O） |
| 候选堆按键值而非全精度距离排序；ADC 估错的点被永久丢弃或重复计入批次 | 同上 | 邻居在页里时就地算全精度距离；纠正近似值时先 pop 再重新入堆 |
| `beam` 名额用完时仍把堆顶弹出，最近候选被丢弃 | `search_one` | 先判断再 pop |
| 收敛条件按"整批"判断，beam 一宽就退化成"必须凑满一批才能停" | 同上 | 只对"最前面的候选"判断（Vamana relaxed 条件） |
| 一次撒 32 个起始锚点，前几轮批次被远处的锚点占满 | `initialize_search_state` | 只保留最近的 4 个 |
| 每个节点都重算一次 medoid（O(n·dim)），1M 构建从 5 分钟涨到小时级 | `vamana_builder.cpp` `beacon_candidates` / `search_for_construction` | `build()` 算一次并缓存 |
| 每个节点、每条反向边、每次剪枝都重建 `deleted_mask`（1M 下 1 MB 清零） | 同上 | 仅在删除列表非空时构造 |

### 修复后的实测（20k，60 条查询）

| 搜索宽度 | 修复前 | 修复后 |
| --- | --- | --- |
| 64 | 0.8883 | **0.9033** |
| 128 / 156 / 256 | 0.9833 | 0.9833 |

benchmark 口径（`search_width=156`、`beam_width=16`）修复后召回 1.000、平均延迟 2.51 ms；
同一口径下 `graph_probe` 从入口出发 0.9833，从真值起点出发 1.0000。
构建器的修复在 20k 上与旧构建**结构持平**（可达率 99.91%、邻居局部性 0.2% 百分位都相同），
它的价值只在 1M 及以上规模显现。

**注意**：修复前 benchmark 报的 0.877/0.998 是靠"采样了 500 条查询 + bench 默认参数"
恰好掩盖了部分缺陷；修复前 `graph_probe` 与 benchmark 的数字对不上，
根因是探针以前没传 `beam_width`（默认 0 → 等于搜索宽度），口径不一致。

### 仍然遗留

- `adc_enable` 仍被 `search_one` 硬编码为 `true`；关掉 ADC 的对照分支实际不存在，
  参数名存实亡（已记录，未改，因为它牵涉"不训练 PQ 时如何搜索"的设计取舍）。
- `compute_distance_direct` / `vector_data` 一层多绕了一次 `get_node`，
  虽然结果正确，但在热路径上重复查表，待清理。

## 3.1 重建 1M 索引后召回仍然只有 0.0002

按 §3 的修复重建 1M 索引（11m40s，704,771,256 B，入口 148523）后，
`index_integrity` 的结果与修复前**完全一致**：

| 指标 | 修复前 | 重建后 |
| --- | --- | --- |
| 邻居距离百分位（0% = 全近邻，100% = 随机） | 27.4% | 11.3% |
| 从入口可达 | 942 / 1,000,000 (0.09%) | 942 / 1,000,000 (0.09%) |

可达数一字不差，说明"修构造搜索"没有触及真正的断裂点。逐层排查如下。

### 1. 图没有断，是入口点孤岛

新增 `entry_audit`（入度分布 + 任意节点出边）与 `u_reach`（有向可达规模）：

```
entry 0       reachable=997122 (99.71%)
entry 148523  reachable=942    (0.09%)
```

**图本身是连通的**，但构建时选中的入口（medoid）恰好落在一个 942 点的小簇里。
1M 图上 medoid 的入度 487,233、全图最大入度 517,149 —— 这不是"中心"，而是
robust prune 的 alpha 规则在 hub 上失效后堆出来的伪 hub，出边又被写成了别的点。

### 2. 邻居局部性与召回解耦

`entry_audit` 看真值点 635985 的 32 条出边：**0 条是近邻**（最近 73.7，
而它自己的最近邻是 3.04）。这些出边指向的是均匀切片点（0, 31250, 62500 …）
和一撮 hub。构造阶段给每个节点注入的远点/切片锚点，作为 robust_prune 的候选
时与真实近邻争抢有限的出边名额，并且会因为 alpha 更宽松而胜出。

### 3. 一次对照实验几乎判断错方向

20k 上开关 beacon/起点锚点，四种组合的可达率、局部性、召回**完全相同**
（99.91% / 0.2% / 1.00）——因为 20k 上远点太远，压根进不了候选池。
在大规模图上的结论不能从 20k 外推。

### 4. "第二遍构建"反而制造了新的断裂，并暴露一个序列化契约

把构造改成 Vamana 论文的两遍（第二遍在完整图上重新搜索+裁剪）后，局部性
从 3.1% 降到 **0.0%**（出边全是近邻），但可达率掉到 **0.10%**。
原因是第二遍补反向边时直接 `push_back`，**没有做度数约束**：

```
in-memory max_degree=119 nodes_over_limit=105472
```

而 V3 页格式只按 `metadata.degree`（32）编码每条邻接表的前 32 条，
多出来的边在序列化时被**静默丢弃**。于是"内存里连通、落盘后断裂"。

> **契约缺陷**：`DiskPageCodec::encode_packed_page` 与
> `decode_packed_page` 遇到超长度邻接表都不报错，只是截断/跳过。
> 这是一类会静默产生坏索引的错误，应当改为拒绝编码。

修复后（反向边走 `add_reverse_edge`，受度数约束）：内存与磁盘一致，
无缓存与 64 页 LRU 下可达规模也一致（99.712%）。

### 5. 入口选择：PageRank 也不够

固定为"最大连通分量里 PageRank 最高的点"后仍然只能到达 122 个节点：
PageRank 前 32 名全部落在同一个局部稠密簇里。补上均匀采样候选、按**实测有向
可达规模**挑选后，入口落到 0 号节点，可达 119,195 / 120,000（99.33%，12 万子集）。

区间实验（关闭 beacon 与切片锚点、两遍构建、可达规模选入口）：

| 规模 | 邻居局部性 | 从入口可达 |
| --- | --- | --- |
| 200 | — | 199/200 |
| 1,000 | — | 997/1000 |
| 5,000 | — | 4999/5000 |
| 20,000 | 0.1% | 19,968/20,000 |
| 40,000 | — | 39,350/40,000 |
| 80,000 | — | 77,390/80,000 |
| 100,000 | — | 284/100,000（**转折点**） |
| 120,000 | 2.9% | 119,195/120,000 |

### 6. 搜索侧另有两个缺陷

- **候选堆可以为空**：`spread_start_count=0` 时 `accepted_entries` 里的种子
  会被锚点截断逻辑吞掉，`candidates=0`、搜索零扩展、返回空结果，而调用方
  只看到一个召回 0 的查询。已加"候选堆不得为空"的兜底。
- **近似值改正不计入预算**：`front.approximate` 分支 pop 后重新入堆，既不计入
  `expanded` 也不计入 `emitted`，存在自循环的理论空间。已加固定上界并留痕。

### 7. 找到真正的根因：alpha 没有随构建进度调度

第 5、6 条修完后（关 beacon/切片锚点、两遍构建、按可达规模选入口），
1M 新索引的可达率是 99.99%、从入口出发的召回 0.0917，**结构指标全对，召回仍然很低**。

这时引入一个不受查询影响的直接指标 `neighbor_recall@10`：对采样节点暴力找出
它自己的最近 10 个点，数出边里有几个命中。它把"构图问题"和"搜索问题"分开：

| 索引 | 邻居召回@10 |
| --- | --- |
| 20k（任意构建变体） | 0.981 |
| 1M 旧图 | 0.089 |
| 1M 新图（两遍 + 无边锚点） | 0.051 |

并按规模扫了一遍，退化是单调的：

| 规模 | 邻居召回@10 |
| --- | --- |
| 20k | 0.975 |
| 40k | 0.963 |
| 80k | 0.703 |
| 160k | 0.472 |

**根因**：`robust_prune` 的 alpha 一直是固定的 1.2。Vamana 要求它**随构建进度
从 1.0 线性升到目标值**。固定 alpha 时，第 i 个插入的节点只能看见 i-1 个点，
候选池里几乎全是"已插入的远处点"，宽松的 alpha 把它们全部保留成出边；
规模越大，早期节点的邻居池越贫瘠，图里"每个点的出边都不在本地点附近"。

改成 alpha 调度后，同样的规模序列：

| 规模 | 修复前 | 修复后 |
| --- | --- | --- |
| 20k | 0.975 | 0.975 |
| 80k | 0.703 | **0.973** |
| 160k | 0.472 | **0.935** |
| 320k | — | **0.935** |

> 教训：前 6 条里的每一个修复都是对的（可达率、度数约束、入口选择、编解码契约），
> 但没有一个能影响召回，因为它们都在修"搜索能不能走到"，而问题在"要走的边
> 本身不存在"。**是邻居召回这个指标把它一次定位出来的**——在此之前的排查
> 一直在查询层面打转。这条指标应当成为构图改动的前置检查项。

### 8. 1M 重建结果与起点覆盖

alpha 调度后在 1M 上重建（`idx1m_v3`，入口 437651）：

| 指标 | 旧图 | 修复后 |
| --- | --- | --- |
| 邻居召回@10 | 0.089 | **0.862** |
| 邻居距离百分位 | 27.4% | **1.4%** |
| 入口可达 | 0.09% | **99.28%** |
| `graph_probe` 从真值起点 | 0.14 | **0.98** |
| `graph_probe` 从入口起点 | 0.00 | 0.23 |

图本身和搜索都对了，**剩下的唯一瓶颈是从入口出发的搜索起点**：单入口的贪心
搜索只沿更近的点走，起点落在错误区域就永远出不来。

对照实验（1M，degree 32，io_uring，100 条查询）：

> **复核注（§4.5）**：以下所有"冷读/冷缓存"标注均不成立——默认配置下
> `ensure_node_in_degrees()` 已把索引全部读入页缓存，`io_uring` 靠 O_DIRECT
> 绕过它。物理读字节数与召回仍然真实，但延迟不是纯冷盘延迟。

| 起点覆盖 | recall@10 | 延迟 | 物理读/查询 |
| --- | --- | --- | --- |
| 单入口 | 0.035 | — | — |
| 均匀撒 32 点 | 0.295 | 48.6 ms | 2.06 MB |
| 均匀撒 2048 点（每个都读整页） | 0.947 | 221 ms | 9.41 MB |
| 撒 2048 点 + ADC 预筛（只读 4 页） | 0.946 | 25.3 ms | 1.07 MB |
| 撒 4096 点 + ADC 预筛 | **0.987** | **19.9 ms** | **0.86 MB** |

关键发现：**起点越密，读的页越少**。撒 32 个点时贪心在错误区域里反复游走
（50,290 页），撒 4096 个点时前几批扩展就在答案附近（21,036 页）。
"撒点多 = I/O 多"在这个规模上不成立，真正决定 I/O 的是起点离答案有多远。

因此两处默认值改了：

- `spread_start_count` 32 → **4096**；
- 撒开的起点先按 PQ 码的 ADC 近似值排序，只对最靠前的 4 个读整页
  （`kMaxSeedAnchors × kExactAnchorsPerSlot`），把"筛多少候选"和
  "读多少页"解耦。

### 9. 正式结果（2000 条查询，2000 万条基线）

| 指标 | 结果 | 赛题要求 |
| --- | --- | --- |
| Recall@10（1M） | **0.988** | ≥ 0.85 |
| 平均延迟 / p95 | 20.7 ms / 28.1 ms | — |
| 物理读/查询 | 874 KB | — |
| 逻辑→物理比 | 1.000（O_DIRECT，确实走盘） | — |
| 峰值 RSS / 数据集 | **7.6%** | 10%–20% |
| 20k 回归 | 0.960（邻居召回 0.981 不变） | — |

峰值 RSS 7.6% 低于 10% 下限，这是好事（内存更省），但预算口径需要按赛题
定义再核对，不能只看"不超过 20%"。

改完默认值（`cache-policy none`、撒点固定 4096）后，用**完全默认的参数**复跑
2000 条查询：exit 0、Recall 0.988、20.95 ms、873,742 B/q、峰值 RSS 39.06 MB
（7.62%）、`memory_budget_pass = true`。ctest 7/7 通过。

### 10. 撒点数与页缓存争同一份预算，撒点数全面占优

`spread_start_count` 和页 cache 都要吃内存。把两者交叉跑一遍（1M，io_uring，
每次 `vmtouch -q -e`，200 条查询）：

> **复核注（§4.5）**：`vmtouch -e` 逐出后，`ensure_node_in_degrees()` 会立刻把
> 索引全部读回页缓存，所以这些"冷缓存"对照实际上都是在索引全驻留状态下跑的。
> 物理读与召回数字仍然有效，延迟对比受此影响。

| 撒点数 | 页 cache | Recall@10 | 延迟 (ms) | 物理读 (MB/q) | 峰值 RSS |
| --- | --- | --- | --- | --- | --- |
| 512 | none | 0.628 | 39.7 | 1.63 | 7.7% |
| 512 | 12000 | 0.628 | 196.6 | 1.32 | 20.1% |
| 1024 | none | 0.785 | 31.7 | 1.38 | 7.7% |
| 1024 | 12000 | 0.785 | 168.2 | 1.20 | 20.0% |
| 2048 | none | 0.928 | 24.8 | 1.07 | 7.6% |
| 2048 | 12000 | 0.928 | 130.8 | 0.99 | 20.0% |
| 4096 | none | **0.986** | **21.2** | **0.87** | **7.4%** |
| 4096 | 12000 | 0.986 | 100.7 | 0.83 | 20.0% |

两个结论：

1. **撒点数同时降延迟、降读量、升召回**。从 512 到 4096，召回 0.628 → 0.986，
   延迟 39.7 → 21.2 ms，物理读反而从 1.63 MB 降到 0.87 MB。原因见 §3.1.8：
   起点越准，贪心越早收敛，探的页就更少。这三点并不存在取舍。
2. **页 cache 是纯负收益**。在任何撒点数下，开 12000 页都把延迟放大 4–5 倍，
   物理读只降 3–5%。cache 命中率高（65%）但大多命中的是同一页里的其它节点，
   真正的磁盘读本来就少，省下的部分抵不过维护 cache 的开销。

因此两个默认值改了：

- `--cache-policy` 默认 `graph-aware-2q` → **`none`**；
- `spread_start_count` 固定 **4096**（移除了 `AGENT_AWARE_SPREAD_STARTS`
  实验开关，它在 `DiskGraphSearchConfig::spread_start_count` 之外又藏了一条
  不受参数系统管的配置路径）。

注意 O_DIRECT 本身绕过了页缓存，这也是 cache 收益小的物理原因之一。

### 11. 已知负收益机制清单

这一节把"投了资源反而更差"的机制集中列出，每条都附实测数字与根因。
判据统一为：**投入的资源（内存 / CPU / 延迟）换不回对应的收益**。

#### A. 页缓存（任何策略）

| 配置 | 延迟 | 物理读 | 峰值 RSS |
| --- | --- | --- | --- |
| none | 21.1 ms | 0.87 MB/q | 7.4% |
| lru 12000 页 | 100.7 ms | 0.83 MB/q | 20.0% |
| graph-aware-2q 12000 页 | 118.0 ms | 0.83 MB/q | 20.0% |

**根因（三层，缺一不可解释）**：

1. **物理层**：索引以 O_DIRECT 打开，本来就绕过页缓存。缓存命中省下的是
   "一次 `pread` 系统调用"，不是"一次磁盘寻道"——省下的量级完全不同。
2. **数据结构层**：`DecodedPage` 的真实常驻代价 = 页字节 4 KiB + 每个节点一份
   `std::vector` 邻接（6 节点/页 × 32 邻居 × 4 B ≈ 800 B）。所以"缓存 12000 页"
   实际占用远超 12000 × 4 KiB，`resident_page_bytes` 就是为此引入的。
3. **访问模式层**：实测 `same_page_node_reuse = 380,637` 而
   `unique_pages_touched = 106,579`，即每个打开页平均被复用 3.6 次，但复用
   **发生在同一条查询内部**。缓存的价值在跨查询复用，而跨查询复用有限，
   且省下的是系统调用。维护 LRU/2Q 的链表操作与互斥锁（`cache_mutex_` 在每次
   页查找/插入都加锁）成本超过了省下的那点 I/O。

**注意策略之间没有差异**：lru 与 graph-aware-2q 的命中率是 0.660916 对
0.660963，几乎完全相同。这说明瓶颈不在淘汰策略，而在"缓存这件事本身"。

**（复核修正，见 §4.5）** 上面引用的 0.661 是 `cache_hits / page_requests`，
分母含重复请求，口径错误。正确口径下 `cache_hits / page_requests_after_dedup`
= **3.79**（超过 100%，不可能），真实"未走 I/O 即满足"的比例是
`(pinned_hits + cache_hits) / page_requests` = **0.819**。数字更高，结论不变：
命中率高但收益小。

#### B. 拓扑预取

| 配置 | 召回 | 延迟 | 物理读 | `prefetch_submitted` |
| --- | --- | --- | --- | --- |
| none | 0.988 | 21.1 ms | 0.87 MB/q | 0 |
| topology | 0.988 | 21.3 ms | 0.87 MB/q | **0** |

**根因：预取被自己的前置判据完全短路了。** 预取试图预测"搜索接下来会打开
哪些页"，但预测依据是候选堆里的节点，而这些节点**几乎全部已经在当前会话里
物化过**（`is_node_materialized` 为真），所以 `prefetch_candidate_pages = 0`，
`submit_topology_prefetch` 收到空列表，`prefetch_submitted = 0`。

这不是"预取太弱"，而是**撒点 4096 已经把预测问题解决了**：起点足够密，
搜索一上来就打开了正确的页，剩下的候选自然都在已加载页里。预取要解决的问题
（"下一步该读哪页"）在撒点足够密时不存在。

代价仍在付：planner 遍历了 135,008 个候选节点、烧了 25.5 ms CPU
（`prefetch_planner_cpu_ratio = 0.0024`），换来的 `prefetch_saved_wait_us = 0`。
所以是**纯 CPU 负收益**，延迟那 0.2 ms 的差就来自这里。

#### C. 多线程（每 worker 一份索引）

| 线程 | 单查询延迟 | QPS | 峰值 RSS | 内存倍数 |
| --- | --- | --- | --- | --- |
| 1 | 20.9 ms | 47.9 | 7.5% | 1.0× |
| 2 | 23.3 ms | 82.9 | 18.9% | 2.5× |
| 4 | 29.3 ms | 128.6 | 30.9% | **4.1×** |
| 8 | 44.3 ms | 162.5 | 54.2% | **7.2×** |

**根因：并行化的收益被内存线性放大吃掉，而且单查询延迟反而恶化。**

1. **内存**：每个 worker 打开自己的 `PackedDiskGraphIndex`（页 cache、io_uring
   环、PQ 码、目录表都按 worker 数复制）。8 线程时 7.2 倍内存，54.2% 远超 20%
   预算——**4 线程就已经 30.9% 出局**。
2. **延迟**：QPS 从 47.9 涨到 162.5 只有 3.4 倍，而线程数是 8 倍；单查询延迟
   从 20.9 涨到 44.3 ms（**2.1 倍恶化**）。原因是 8 份索引同时从同一块盘冷读，
   I/O 队列深度饱和，每个查询的等待变长。这是"吞吐换延迟"，而且吞吐也没换到
   线性。（注：这组数据在 `protect_hot_pages = true` 默认配置下测的，见 §4.5；
   多线程的核心问题是内存放大，该结论不受缓存状态影响。）
3. **根本矛盾**：赛题约束是内存 ≤ 数据集 10%–20%。在 7.5% 的单线程配置下
   还有空间，但每加一个 worker 就复制一份索引，**2 线程就 18.9%、4 线程出局**。
   所以"用多线程提升 QPS"在这条内存约束下最多只能用到 2 线程，收益 1.7 倍。

#### D. 已修复的负收益（留作回归参照）

这些曾经是负收益，现已修复，列出来是为了防止回退：

| 机制 | 曾经的表现 | 根因 | 现状 |
| --- | --- | --- | --- |
| `robust_prune` 的 alpha 固定 1.2 | 邻居召回随规模崩塌：20k 0.975 → 1M 0.05 | Vamana 要求 alpha 从 1.0 线性升到目标值；一开始就 1.2 会立刻剪掉近邻 | 两遍构建 + 进度调度，1M 0.862 |
| 第二遍逆边裸 `push_back` | 入口可达率 99.33% → 0.10% | 内存邻接表可到 119 度，V3 页只编 32 条，编码静默截断 | 统一走 `add_reverse_edge`；编码器对超长邻接表抛错 |
| 入口选几何 medoid | 入口落进 122 点孤岛，可达 0.09% | medoid 是几何中心，不保证可达性 | PageRank 候选 + 实测 BFS 可达规模 |
| 单一入口搜索 | 1M recall 0.035 | 剪枝图上 beam search 困在局部 | 撒点 4096 + ADC 预筛，0.988 |
| 每查询容器不释放 | 峰值 RSS 随查询数上涨：50 条 91 MB、100 条 120 MB | `finish_query` 不释放本查询的解码结果 | `swap` 释放，稳定 7.6% |

#### E. `search_width` 超过收敛点后是纯浪费

| 配置 | 召回 | 延迟 | 物理读 | 每查询扩展数 |
| --- | --- | --- | --- | --- |
| width 64 | 0.969 | 18.9 ms | 0.80 MB/q | 68 |
| width 156（默认） | 0.988 | 20.8 ms | 0.87 MB/q | 123 |
| width 256 | 0.992 | 20.5 ms | 0.88 MB/q | **124** |
| width 512 | 0.992 | 20.6 ms | 0.88 MB/q | **124** |
| width 1024 | 0.992 | 20.6 ms | 0.88 MB/q | **124** |

**根因：width 是上限，不是目标。** relaxed 收敛条件在第 124 个节点处就满足了，
所以 256 / 512 / 1024 的实际扩展数完全相同。继续调大 width 不会多读一页，
也不会多算一次距离——它只是把上限设得更高，而搜索永远碰不到。

这也解释了复盘里"三处文档推荐不同的 search_width（128 / 156 / 350）"为什么
没有实测差异：三者都在饱和区。**真正需要调的是 156 与 124 之间那一段**，
那里才是收敛点所在。width 64 是唯一真正降级的配置（召回 0.969）。

#### F. 待确认（有负收益迹象，但未做对照实验）

| 机制 | 现象 | 需要什么实验才能定论 |
| --- | --- | --- |
| delta 层线性扫描 | 新写入数据只能靠 delta 线性扫描，磁盘图里没有 | delta 规模 × 查询延迟 |
| `--protect-hot-pages` | 默认 `true`，但 `cache-policy none` 下 `page_hot_score` 根本不会被调用 | 只在开 cache 时有效，随 A 一起作废，可从默认值里删掉 |
| 2Q 的 protected 队列 | 任何页第二次被访问即晋升（`lookup_cached_page` 无条件 `protected_queue = true`），受保护队列实际等于"所有被命中过的页"，形式上已退化为 FIFO | 需构造跨查询热点负载；但在 A 成立前没有意义 |

### 12. 搜索的主要成本是什么

用 `/usr/bin/time -v` 拆 user/sys CPU 与墙钟（1M，500 条查询）：

| 项 | 总量 | 每查询 | 占墙钟 |
| --- | --- | --- | --- |
| 墙钟 | 12.47 s | 24.94 ms | 100% |
| **user CPU** | 1.75 s | **3.50 ms** | 14% |
| **sys CPU** | 3.85 s | **7.70 ms** | 31% |
| 等待（I/O + 调度） | — | **13.74 ms** | **55%** |

> **（复核修正，见 §4.5）** 这张表是在 `protect_hot_pages = true` 的默认配置下测的，
> 而该配置会让 `ensure_node_in_degrees()` 顺序读全部 166,667 页（672 MB）。
> 这部分开销摊在进程总时间里，使 sys CPU 偏高。真冷重测
> （`--protect-hot-pages 0`，300 条，`io_uring`）：
> wall 6.03 s、user 0.71 s、**sys 1.64 s**、CPU 39%，即
> **user 2.37 ms/条、sys 5.47 ms/条、等待 12.12 ms/条**。
> 比例结构不变（等待 > sys > user），但 sys 的绝对值下降了约 29%。

CPU 占用率 39%–44%，即**一半以上时间在等 I/O**。每查询规模：

| 量 | 值 | 说明 |
| --- | --- | --- |
| 扩展节点 | 123 | 收敛点（§11.E） |
| 读页 | 213 | 每扩展 1.73 页 |
| 页请求 | 1,154 | 含 3.9k 次重复请求，被去重掉 |
| 物理读 | 873 KB | 213 × 4 KiB，逻辑→物理比 1.000 |
| 精确距离计算 | 851 次 | 每次 128 维 AVX2 |
| ADC 表构建 | 0.11 ms | 16 子空间 × 256 质心 |

**成本排序：I/O 等待（55%）> sys CPU（31%）> user CPU（14%）。**

#### 两个被指标误导的陷阱

**1. `demand_read_wait_us` 不是 I/O 等待。** 它报 21.39 ms/查询，但延迟只有
23.82 ms 且 CPU 只占 11.2 ms——三个数不可能同时成立。查代码：该计时区间的
`wait_start` 到累加之间**夹着 `decode_page` / `store_cached_page` /
`materialize_page` 的 CPU 时间**（`query_page_session.cpp:232-272`）。所以它
是"I/O 批次处理总耗时"，不是纯等待。**引用这个字段会得出"I/O 占 90%"的错误
结论。**

**2. sys CPU 高得反常：每页一次系统调用。** `io_submit_syscalls = 106579`，
与 `page_reads` 精确相等。批处理配置（`--io-depth` / `--io-batch`）对
syscall 数**毫无影响**：

| depth | batch | page_reads | syscalls | sys/页 |
| --- | --- | --- | --- | --- |
| 32 | 16 | 42719 | 42719 | 1.00 |
| 32 | 1 | 42719 | 42719 | 1.00 |
| 64 | 32 | 42719 | 42719 | 1.00 |
| 128 | 64 | 42719 | 42719 | 1.00 |

根因：`expand_candidate_batch`（`disk_graph_index.cpp:822`）对 batch 里的每个
候选**逐个** `state.load_node(current.id)`，而 `load_node` 落到
`get_or_load_page` → `read_demand_page`，后者用 `batch_submit({单个请求})`
提交**恰好一页**（`query_page_session.cpp:549`）。批处理路径
`ensure_pages_loaded_batch` 虽然存在，但只有 `ensure_pages_loaded_batch`
自己的入口会走到，扩展路径不经过它。

所以 `demand_batch_limit()`、`io_depth`、`io_batch` 三个参数在这条热路径上
全部无效——批大小实际恒为 1。这就是 sys 占大头的直接原因：**213 次
`io_uring_submit` + 213 次 `io_uring_wait_cqe` 系统调用，每次只搬 4 KiB。**

> **复核确认（§4.5）**：该结论在真冷条件下依然成立——`io_submits == page_reads`
> 在 `io_uring` / `odirect` / `pread` 三种模式下都精确相等。注意 `pread` 模式
> 的 `io_submit_syscalls` 也是每页 1 次，但那只对应一次 `pread` 系统调用；
> `io_uring` 每页还要额外付一次 `wait_cqe`。

#### 可动的空间

| 方向 | 当前 | 上限估计 | 依据 |
| --- | --- | --- | --- |
| 批处理 I/O | 1 页/次 | 16 页/次 | 已在代码里，只是扩展路径没走 |
| 减少页数 | 213 页 | ~123 页 | 每扩展 1.73 页，理想是 1.0 |
| 减少 sys CPU | 7.70 ms | — | 批处理生效后 syscall 降 16 倍 |
| 减少 user CPU | 3.50 ms | — | 851 次 128 维距离，已是 AVX2 |

**I/O 等待（55%）是真正的天花板**，但其中相当一部分来自"每页一次往返"的
串行化：单页提交 → 等 CQE → 解码 → 再提交下一页。批处理生效后，这 213 次
串行往返可以压成约 14 次，等待时间有实质性下降空间。

#### 结论

当前搜索成本的三分之二（55% 等待 + 31% sys）都来自**页级 I/O 及其系统调用
开销**，而 user CPU（真正的算法计算）只占 14%。这与 §11 的判断一致：这个
系统的问题是"I/O 粒度太细"，而不是"算法太慢"或"缓存不够"。



## 4. 内存预算

预算 = 数据集字节数 × `--memory-budget-ratio`（默认 0.20），1M 即 512 MB × 20%
= 102.4 MB。默认配置（`cache-policy none`、撒点 4096）实测峰值 RSS **39.06 MB
= 7.63%**，`memory_budget_pass = true`，退出码 0。

硬失败链路：`--memory-budget-enforce`（默认 1）下，实测峰值 RSS 超预算时进程以
**退出码 2** 结束；预算低于进程自身固定开销时以**退出码 3** 结束并说明"任何配置
都无法满足"。20k 场景就属于后者：预算 13 KB 而进程固定占用 5.5 MB。

之前的 `graph-aware-2q` + 自动页数会算到"刚好不剩"，实际峰值 23.95%，默认配置
直接撞上退出码 2 —— 这也是把默认值改成 `none` 的原因之一：与其在预算边缘自动
分配一个负收益的 cache，不如干脆不分配。

---

## 4.5 全量复核（2026-10-26 第二遍）

§1–§12 的结论写完后做了一次系统复核，**推翻了其中几条**。复核方法与结论如下。

### 复核方法

1. **字段级**：用脚本枚举 `DiskGraphSearchStats` 的 60 个字段，检查每个字段在
   生产者代码（`disk_graph_index.cpp` / `query_page_session.cpp` /
   `async_page_reader.cpp` / `prefetch_adaptive_controller.cpp`）里是否有写点，
   排除 `merge_aggregate` 的汇总假象。
2. **恒等式**：对每个配置检查 `io_submits == page_reads == physical_B/page_size`、
   `hits + misses <= page_requests`、`residency ∈ [0,1]` 等应当成立的关系。
3. **外部对照**：用 `vmtouch`（独立进程）与程序内的 `mincore` 互相比对驻留率，
   避免"自己测自己"。
4. **量级估算**：把实测字节数与索引结构的大小对比，判断某个操作"物理上能否
   解释这个量"。

### 结论一：冷缓存从未成立（推翻 §1、§2、§11、§12 的冷读前提）

程序内报 `index_cache_residency_before = 1`，而同一时刻 `vmtouch` 报 0%。
两者都是 `mincore`，不该矛盾。逐段缩小范围后定位到：

| 步骤 | 驻留率 |
| --- | --- |
| `vmtouch -e` 之后 | 0/172064 = **0%** |
| 构造索引对象之后 | 0/172064 = **0%** |
| **跑完 1 条查询之后** | **172064/172064 = 100%** |

一条查询只读 195 页（0.8 MB），却让 672 MB 全部驻留。唯一的解释是有代码顺序
读了整个文件——即 `configure_cache` 里的 `ensure_node_in_degrees()`：

```cpp
if (policy == "graph-aware-2q" || policy == "agent" || protect_hot_pages) {
  ensure_node_in_degrees();   // 顺序读全部 page_count 页
}
```

`protect_hot_pages` **默认为 true**，所以 `--cache-policy none` 也会触发。实测对照：

| `--protect-hot-pages` | residency_before | residency_after |
| --- | --- | --- |
| 1（默认） | **1.000** | 1.000 |
| 0 | **0.044** | 0.044 |

**影响**：§1–§12 里所有标注"冷读 / 冷缓存"的数字，实际测的是
"索引全驻留 + O_DIRECT 绕过页缓存"的组合。物理读字节数仍然真实（O_DIRECT
确实走盘），但**延迟不是纯冷盘延迟**，且页缓存的 672 MB 占用完全没被计入
内存预算。

**修正**：原先 §5 待办 #3 写"构造函数全量读 638 MiB PQ 码区"，是错的——
PQ 码区只有 18 MB（占索引 2.6%）。真正的元凶是入度统计的全文件顺序读。

### 结论二：三个统计字段恒为 0 却照常输出

| 字段 | 状态 |
| --- | --- |
| `pq_filter_reject_count` | **从不赋值**，恒 0，但写进 JSON |
| `prefetch_saved_wait_us` | **从不赋值**，恒 0，但写进 JSON |
| `page_cache_evictions` | 正常（经 `stats->` 指针写入，脚本误报） |

前两个字段永远输出 0，读起来像"没有发生"，实际是"没有测量"。

### 结论三：`cache_hit_rate` 分母错误（推翻 §11.A 的 65% 命中率）

```cpp
cache_hit_rate = cache_hits / page_requests
```

`page_requests` 含**重复请求**（同一页被多次请求），而 `cache_hits` 只在去重后
路径累加。实测：`cache_hits = 153243`、`page_requests = 231848`、
`page_requests_after_dedup = 40405`。

| 口径 | 值 | 说明 |
| --- | --- | --- |
| 报告值 | 0.661 | 分母含重复请求 |
| `hits / after_dedup` | **3.79** | **超过 100%，数学上不可能** |
| `(pinned + hits) / page_requests` | 0.819 | 真实"未走 I/O 即满足"比例 |

§11.A 用 0.661 论证"命中率高但收益小"，这个论证仍然成立（真实比例 0.819
更高，结论只会更强），但**引用的数字是错的**。

### 结论四：`demand_read_wait_us` 不是 I/O 等待（已在 §12 修正）

计时区间里夹着 `decode_page` / `store_cached_page` / `materialize_page` 的 CPU
时间。它报 21.39 ms/查询而实际延迟只有 23.82 ms、CPU 仅 11.2 ms，三个数不可能
同时成立。

### 结论五：`io_uring` 与 `pread` 不可比（推翻 §12 的 CPU 分解）

真冷（`--protect-hot-pages 0`）条件下的对照：

| 模式 | 延迟 | wall | user | sys | CPU% | 物理读/q | residency after |
| --- | --- | --- | --- | --- | --- | --- | --- |
| io_uring | 19.96 ms | 6.03 s | 0.71 s | 1.64 s | 39% | 876 KB | **0.044（保持冷）** |
| pread | **6.69 ms** | 2.05 s | 0.44 s | 0.90 s | 65% | **2.23 MB** | **0.991（自己读热了）** |

`pread` 快 3 倍，但它**把自己读热了**——`logical_to_physical_read_ratio = 0.39`，
即 61% 的逻辑读由页缓存满足。而 `io_uring` 走 O_DIRECT，始终保持 0.044 的冷
状态。**两者的延迟不可直接比较**：一个是"冷盘读 876 KB"，另一个是
"冷盘读 2.23 MB + 热缓存读 876 KB"。

§12 原先的 CPU 分解（user 3.50 / sys 7.70 / 等待 13.74 ms）是在污染条件下测的，
`sys 7.70 ms` 里含 `ensure_node_in_degrees` 的 672 MB 顺序读开销。真冷下
`io_uring` 的 sys 是 1.64 s / 300 条 = **5.47 ms/条**，等待仍是主要成本。

### 结论六：`entry_strategy` 等字段与实际不符

JSON 报 `entry_strategy = "single-medoid"`、`entry_count = 1`，而实际使用的是
PageRank + BFS 可达性选出的入口（§3.1.5），并撒 4096 个起点。**没有任何字段
报告撒点数量**，`entry_count` 恒为 1 是硬编码。

### 复核后仍然成立的结论

| 结论 | 为什么不受影响 |
| --- | --- |
| Recall 0.988 | 真冷重测 0.985，一致 |
| alpha 调度修复（§3.1.7） | 图质量指标，与缓存无关 |
| 入口选择修复（§3.1.5） | 同上 |
| 页缓存负收益（§11.A） | 真冷下差距只会更大 |
| 多线程内存线性放大（§11.C） | 内存测量独立于缓存状态 |
| `search_width` 饱和（§11.E） | 扩展数统计，与缓存无关 |
| 批处理未生效（§12） | `io_submits == page_reads` 在真冷下依然成立 |

### 复核后的正式结果（以此为准）

真冷（`--protect-hot-pages 0 --require-cold-cache 1`）、1M、2000 条查询：

| 指标 | 复核后 | 原报告（污染条件） | 赛题要求 |
| --- | --- | --- | --- |
| Recall@10 | **0.988** | 0.988 | ≥ 0.85 |
| 平均延迟 / p95 | **19.76 / 27.07 ms** | 20.95 / — | — |
| 物理读/查询 | 873,742 B | 873,742 B | — |
| 逻辑→物理比 | **1.000** | 1.000 | — |
| 峰值 RSS / 数据集 | **6.84%** | 7.62% | 10%–20% |
| `index_fully_cached_during_run` | **False** ✅ | True ❌ | 冷读实验须为 False |
| `cache_hit_rate`（新口径） | **0.808** | 0.661 ❌ | — |
| `cross_query_cache_hit_rate` | **0.000** | — | — |
| `memory_budget_pass` | True | True | 必须 True |

三点值得注意：

1. **召回与物理读完全不变**。复核没有推翻任何关于图质量或 I/O 量的结论。
2. **延迟略降**（20.95 → 19.76 ms）。因为 `ensure_node_in_degrees()` 的
   672 MB 顺序读不再发生，启动阶段省掉了这部分开销。
3. **`cross_query_cache_hit_rate = 0.000`**。这条彻底证实 §11.A 的论断：
   在 `cache-policy none` 下**没有任何跨查询页复用**，命中率 0.808 全部来自
   同一条查询内部的 `pinned_hits`。缓存的价值在跨查询，而这里跨查询复用是 0。

### 复核发现的方法论问题

`index_fully_cached_during_run` 这个字段**本来就是为了标记"本实验无效"**而加的
（注释写得很清楚），默认配置下它报 `True`，但没有任何地方据此中止或拒绝结论。
`scripts/bench_cold.sh` 会打印警告，可基准本身照常输出漂亮的数字。

**教训**：一个"用于自我否定"的指标，如果不接硬失败，就等于不存在。

### 复核遗留项已修复（2026-10-26 收尾）

复核提出的两个"测量能力"层面的缺陷已经修掉，默认配置现在开箱即是真冷。

**① `protect_hot_pages` 默认值 true → false**

它只服务于 hub-aware 缓存策略的 `page_hot_score`，而 `cache_policy` 默认已经是
`none`，那张入度表根本不会被读。默认 `true` 的代价是每次启动顺序读全部
166,667 页：1M 上 **+0.48 s 启动**、**672 MB 页缓存污染**、**4 MB 常驻内存**。

验证（默认配置，不带任何参数）：

| 项 | 修复后 |
| --- | --- |
| `index_cache_residency_before/after` | 0.0435 / 0.0435（真冷） |
| `index_fully_cached_during_run` | **False** |
| `in_degree_bytes` | **0** |
| 启动耗时 | 2.49 s → 2.01 s |
| 峰值 RSS | 6.59% |
| `--require-cold-cache 1` | **exit 0 通过** |

显式 `--protect-hot-pages 1` 仍然可用（`residency=1`、`in_degree_bytes=4 MB`），
需要对照实验时不受影响。

**② `effective_config.adc_enable = true` 硬编码 → 移除**

那一行会静默覆盖调用方的设置：传 `adc_enable = false` 进去会被改回 `true`，
于是"关闭 ADC"的对照实验实际仍走 ADC，而报告里的开关看起来是关的。
现在不再改写，关闭 ADC 时 `initialize_search_state` 明确报错
（非 PQ 候选生成路径已被移除，所以确实无法关闭）。`--enable-pq 0` 在
benchmark 层就以 exit 1 拒绝。

顺带删掉 `rank_neighbors_by_distance`：它用 ADC 近似值给邻居排序，但
`expand_candidate_batch` 早已改成用 `exact_distance` 排序（页刚读过，算距离
不产生额外 I/O），这个函数已无调用者。留着会让人误以为 ADC 还在承担邻居
排序的职责。

**②的实质结论**：ADC 目前承担三件事——起点预筛、`topk` 后的 rerank、
（历史上）邻居排序占位。第三件已消失，前两件与 I/O 优化强耦合，所以
"完全关闭 ADC"不是一个可用的对照组，而是一个应当明确拒绝的配置。
文档 §5 待办 #2 的原始设想（"恢复关掉 ADC 作为真实对照组"）在现有架构下
不成立，已改为"明确拒绝"。

---


## 5. 待办（按优先级）

1. 1M 重建索引后复测入口召回（`index_integrity` 的 `reachable_from_entry` 是首要指标：
   修复前 0.09%，修复后应接近 100%），再决定 20k（0.98）与 1M 的差距来自图结构还是入口点。
2. ~~去掉 `adc_enable = true` 的硬编码，恢复"关掉 ADC"作为真实对照组。~~
   **已完成（§4.5 收尾）**：硬编码已移除。但结论是"关掉 ADC 不是可用的对照组"
   —— 非 PQ 候选生成路径已被移除，所以现在明确报错拒绝，而不是静默改回 true。
3. ~~`ensure_node_in_degrees()` 顺序读全部 166,667 页~~ **已完成（§4.5 收尾）**：
   `protect_hot_pages` 默认改为 `false`，默认配置不再做入度扫描
   （`in_degree_bytes = 0`，启动省 0.48 s，不再污染页缓存）。
   原先"构造函数全量读 PQ 码"的判断也是错的（PQ 码区只有 18 MB，占索引 2.6%）。
4. 把 `graph_probe` / `index_integrity` / `brute_force_check` 纳入固定流程：
   任何召回结论必须附暴力基线与可达性数字。
5. `--hot_degree_threshold` 已确认正确传递（`sift_search_benchmark.cpp:813` →
   `configure_cache`），此项作废。
6. **（新）修 `expand_candidate_batch` 绕过批处理的问题**：它对每个候选逐个
   `load_node`，导致 `--io-depth` / `--io-batch` 在热路径上无效，每页一次
   `io_uring_submit` + 一次 `wait_cqe`。这是 §12 定位的最大优化空间，属 Phase 1。
   **已完成（Phase 1，2026-10-27）**：见
   [`phase1-demand-io-batching-2026-10-27.md`](phase1-demand-io-batching-2026-10-27.md)。
   复核修正了本条的归因：单页读不在"逐个候选 `load_node`"，而全部在
   `neighbor_rank`（逐个邻居算精确距离），扩展阶段读 0 页。
   修复后 syscall 426,632 → 37,581、每批 11.35 页、延迟 30.1 → 13.5 ms，
   召回 0.988 与读页数 873,742 B/查询**不变**。§12"213 次往返压成 14 次"中，
   批次上限 16 页使实际为 18.8 次/查询。

## 5.1 召回问题的诊断顺序（按这个顺序查，不要跳到查询层）

召回低时，指标要**从构图往下查**，每一步都能排除一整类原因：

1. `neighbor_recall@10`：出边里有没有该点自己的最近邻？
   低 → 构图问题，后面 2–4 条都不用看了，直接查构建参数。
2. `entry_audit`：入口的入度/度数、任意节点的出边距离分布。
3. `u_reach`：从入口出发的有向可达规模。低 → 入口落在孤岛里。
4. `index_integrity`：向量逐位一致 + 入口可达率。
5. 最后才是 `graph_probe`（入口 / 真值起点 / 多起点三组召回）和
   `agent-aware-benchmark`，用来判断搜索本身。

## 6. 复现命令

```bash
# 物理 I/O 计量自检
cd build && ctest -R platform_io --output-on-failure

# 图质量第一指标：出边里有没有该点自己的最近邻（先查这个）
./build/neighbor_recall /data/work/idx1m_v3.idx /data/work/ds1m/sift_base.fvecs 100 10
./build/entry_audit /data/work/idx1m_v3.idx 0        # 入度分布 + 连通分量
./build/u_reach /data/work/idx1m_v3.idx 0           # 入口可达规模

# 暴力基线（真值是否正确）
./build/brute_force_check /data/work/ds1m/sift_base.fvecs \
    /data/work/ds1m/sift_query.fvecs /data/work/ds1m/sift_groundtruth.ivecs 200

# 索引完整性与连通性（向量逐位比对 + 入口可达率 + 邻居局部性）
./build/index_integrity /data/work/idx1m/sift1m.idx /data/work/ds1m/sift_base.fvecs 100 148523

# 图召回诊断：入口 / 真值起点 / 多起点 三组对照。
# beam 必须与 benchmark 一致（默认 16），否则口径不同。
GRAPH_PROBE_BEAM=16 ./build/graph_probe /data/work/idx1m/sift1m.idx /data/work/ds1m/sift_base.fvecs \
    /data/work/ds1m/sift_query.fvecs /data/work/ds1m/sift_groundtruth.ivecs 148523 60 156

# 冷缓存基准。protect_hot_pages 默认已是 false，无需再显式关闭；
# 但 --require-cold-cache 1 仍应带上，它会在驻留率 >0.9 时以 exit 4 失败。
vmtouch -q -e /data/work/idx1m_v3.idx
./build/agent-aware-search --sift-dir /data/work/ds1m --index-path /data/work/idx1m_v3.idx \
    --io-mode io_uring --cache-policy none --require-cold-cache 1 \
    --query-limit 2000 --output-json /data/work/cold.json

# 验证冷缓存是否真的成立：residency_before 必须 < 0.9
python3 /data/work/readjson.py /data/work/cold.json \
    index_cache_residency_before,index_cache_residency_after,index_fully_cached_during_run

# 需要 hub-aware 缓存策略时才打开入度统计（它会顺序读整个索引）
./build/agent-aware-search --sift-dir /data/work/ds1m \
    --index-path /data/work/idx1m_v3.idx --cache-policy graph-aware-2q \
    --protect-hot-pages 1 --cache-pages 12000 --query-limit 200 \
    --memory-budget-enforce 0 --output-json /data/work/hubaware.json

# 撒点数 × 页缓存对照（§3.1.10）。撒点数现已固定为 4096，
# 要重跑网格需临时改 DiskGraphSearchConfig::spread_start_count 后重建。
# protect_hot_pages 默认已是 false，这里不用再带。
for pages in 0 2000 12000; do
  vmtouch -q -e /data/work/idx1m_v3.idx
  extra=""; [ "$pages" -gt 0 ] && extra="--cache-policy lru --cache-pages $pages"
  ./build/agent-aware-search --sift-dir /data/work/ds1m \
      --index-path /data/work/idx1m_v3.idx --io-mode io_uring $extra \
      --query-limit 200 --memory-budget-enforce 0 --output-json /data/work/sc_$pages.json
done

# CPU 成本分解（真冷）：确认 user/sys/等待三者比例
for mode in io_uring pread; do
  vmtouch -q -e /data/work/idx1m_v3.idx
  /usr/bin/time -v ./build/agent-aware-search --sift-dir /data/work/ds1m \
      --index-path /data/work/idx1m_v3.idx --io-mode $mode \
      --cache-policy none --protect-hot-pages 0 --query-limit 300 \
      --memory-budget-enforce 0 --output-json /data/work/cpu_$mode.json \
      >/dev/null 2>/data/work/cpu_$mode.time
  grep -E "User time|System time|Percent of CPU|Elapsed" /data/work/cpu_$mode.time
done
```
