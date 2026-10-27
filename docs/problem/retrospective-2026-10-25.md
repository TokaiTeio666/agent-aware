# 项目复盘：错误与教训

> 整理日期：2026-10-25
> 审计基线：`d1646c9`
> 前序文档：[`current-issues.md`](current-issues.md)（问题清单）、[`fix-status-2026-10-25.md`](fix-status-2026-10-25.md)（修复状态）
> 后续：[`phase0-measurements-2026-10-26.md`](phase0-measurements-2026-10-26.md)（在 Linux VM 上建立物理 I/O
> 计量后的首次可信测量；§1.3 的"页缓存吸收全部 I/O"得到证实，并发现 1M 规模下图索引不可达的缺陷）
> 再后续：[`phase1-demand-io-batching-2026-10-27.md`](phase1-demand-io-batching-2026-10-27.md)
> （demand 读批处理：每页一次往返 → 每 11.35 页一次，延迟 −55%，召回与读页数不变）
>
> 本文不是问题清单的重复。`current-issues.md` 回答"哪里有问题"；本文回答**"这些问题是哪一类错误、为什么会犯、对答辩和对外表述意味着什么、下次怎么做"**。
> 文中标注 **[新]** 的条目是本轮独立审计新发现、`current-issues.md` 未覆盖的；其余条目引用其章节号。

---

## 0. 结论先行

工程主体（beam search、PQ 训练、页编解码、WAL/SSTable 的骨架）质量尚可，`current-issues.md` §5 已确认多处实现正确。
真正拖垮项目的不是"某一行代码写错了"，而是三类系统性问题：

| 类别 | 本质 | 对评分的影响 |
| --- | --- | --- |
| **一、宣称 ≠ 实现** | 设计文档写在前、代码在后、结果最后补，此后三者从未对齐 | 答辩一问就穿帮，可信度崩塌 |
| **二、评测方法失真** | 用"函数调用计数"冒充 I/O、看结果后倒推推荐参数、测试名不副实 | 所有数值结论都必须撤回或重测 |
| **三、AI 生成 + 多人接力** | 死代码、"统计了但不生效"的参数、注释与代码矛盾、提交历史混乱 | 评审按 README 无法复现 |

一句话教训：**结论必须能被第三方按 README 独立跑出来；做不到，就写"阶段性原型 + 已知缺陷"。**

---

## 1. 最重要的五个判断错误

按对项目成败的影响排序，不分代码位置。

### 1.1 把"设计文档"当成"已实现"

写设计稿时把完整愿景写成完成时，代码只做了一部分，最后文档没回头删。
评审只要打开 `docs/legacy/design/*` 对照代码就能发现：

- "写入磁盘图 / Streaming ANN"：`neighbors` 字段写了但**没有任何读路径**（`current-issues.md` §2.7）；
- "Immutable Read View 无锁快照"：实际是 `sequence_id` 过滤，且版本历史无界增长（§1.3）；
- "PQ 过滤/拒绝"：统计字段与语义不符，从未实现拒绝阈值（§2.1）；
- "提前终止 / adaptive early stop"：实现里不存在（§2.2）；
- "CRTP 两阶段页级预取"：1,765 行实施计划，一个状态标记都没有（§3.1）。

**教训**：设计文档要显式标注状态（已实现 / 部分实现 / 设计稿 / 已废弃），并且和 commit 挂钩。
写"无锁快照"之前先问：释放给谁？旧版本什么时候回收？回答不了就不要写。

### 1.2 把"模型估算"当成"实测值"

内存预算 10%–20% 是赛题硬约束，也是最核心的卖点。
README 报告 `estimated_resident_ratio=0.1985`，这是**按公式推算的配置值**；真实峰值 RSS 是 17.6 MB，预算 9.77 MB，**超出 1.8 倍**。
模型漏算的部分：进程固定开销、`QueryPageSession` 的全精度向量副本、`DecodedPage` 真实占用、多线程路径的副本数（§1.2）。

**教训**：凡是"约束类"指标，必须以进程实测为准（Linux `VmHWM`），并以硬失败处理，而不是 warning。
"按模型未超预算"和"实际未超预算"是两个不同的命题，前者不能替代后者。

### 1.3 用"调用次数"冒充"I/O" **[新]**

`reads_per_query = submitted_reads / N`、`bytes_read_per_query = completed_reads × page_size / N`，都是**对 `pread` 的调用计数**，不是物理扇区读量。
数据集 51.2 MB、系统页缓存 4 GB，几乎所有读都命中 OS 页缓存。
证据：sw96→sw192 的页读量涨 2.5 倍，`demand_read_wait` 只涨 43%，且本身还包含了 pread 的调用开销。
`cache-policy.md` 自己写了"4 GB 文件缓存装得下"，但 README 仍然把 156 描述为"I/O 折中点"。

**教训**：做存储类项目，必须能测量**物理 I/O**。最低限度：
每次实验前 `echo 3 > /proc/sys/vm/drop_caches`（或冷数据 + 受限页缓存），实验同时采集 `/proc/diskstats` 的物理读字节；两套数字（逻辑调用 / 物理 I/O）分开报。
在 Windows 上没有 `O_DIRECT`、又开着系统缓存，**就不要给出 I/O 类结论**。

### 1.4 看结果之后倒推推荐参数 **[新]**

`search_width=156` 是在看到 v2 合成数据结果之后选定的"推荐点"。
Recall 增益的分布是：`96→128` 贡献约 70%–87%，按曲线插值 sw≈110 就能越过 0.85 下限。
156 是在搜索间隙"局部低谷"附近挑出来的点，换个数据集不一定成立——`search-width-tuning.md` 自己也承认 0.85 目标不可迁移。
更糟的是三份文档给出三个默认值：README 156、`docs/experiments/README.md` 128、PROJECT_PLAN 350。

**教训**：**先声明判据，再跑结果**。写成"在 Recall@10 ≥ X 的约束下，最小化 I/O 或延迟"，然后报整条曲线，让读者选点。
不要提供一个硬编码的"最优值"。

### 1.5 让 AI 写评测脚本来验证 AI 写的代码

很多"统计字段"和"测试"的实现与名字不符，原因不是不会写，而是**验证者和被验证者都是 AI**，它会自动挑选一个能让数字好看的口径：

- `prefetch_saved_wait_us` 只累加、输出，**从不赋值**（§1.8）；
- `pq_filter_reject_count` 从不自增（§2.1）；
- `--hot_degree_threshold` 解析后从未传给引擎 **[新]**；
- `--pq-train-limit / --pq-iterations` 解析后丢弃（§2.8）；
- `prefetch_adaptive_controller` 的 `required_lead` 计算了没人读（§2.10）；
- `async_page_reader` 的单测里，关键断言全部被 `!async_enabled()` 短路，**在非 io_uring 环境什么都没测到** **[新]**；
- `packed_layout_v3` 只把 `nodes_per_page == 6` 这个公式固化下来 **[新]**；
- `dynamic_write_concurrency` 用 `assert`，Release 下全部被 `NDEBUG` 编译掉（§2.11）。

**教训**：评测代码的编写者要和被评测代码的编写者分开（哪怕只做一轮人工对账）。
新增一个统计字段，必须同时写一个"这个字段应该等于 X"的测试；否则就删掉这个字段。

---

## 2. 分类问题清单

### 2.1 工程实现层

| 问题 | 位置 | 类型 |
| --- | --- | --- |
| `update()` 直接调用 `insert()`，无更新语义；delete 后 insert 会"复活" | `dynamic_write_manager.cpp:603-606`（§1.4） | 语义缺失 |
| 版本历史 `version_shards_` 只增不减（后已加 `version_history_limit=4`，但会漏读中间版本） | §1.3、§2.5j | 精度与内存 |
| 预取与 demand 抢缓冲池，io_uring 下可抛异常崩溃（已部分修复） | §1.5 | 崩溃 |
| 写侧单线程 commit + 每条记录同步 `fdatasync`，吞吐约 60 条/秒 | §1.6 | 性能模型 |
| 锁等待指标恒为 0；`page_read_wait` 是整批 submit→drain 时间 | §1.8 | 指标失真 |
| `select_incremental_neighbors` 用 `hash(id ^ id)` 截候选，等价随机抽样 | §2.7 | 伪实现 |
| 2Q 受保护队列内按 id 降序出队，退化为逆序 FIFO **[新]** | `disk_graph_index.cpp:566-567` | 算法错误 |
| 索引格式校验不足：先分配后校验、无溢出保护、单坏页导致整查询失败 | §2.9 | 健壮性 |
| `pread` 不处理 `EINTR`/短读；Windows `ifstream` 不解对齐 **[新]** | `async_page_reader.cpp:602-668` | 可移植性 |
| Windows 下 `io_submit_syscalls` 不计数，与 Linux 指标口径不同 **[新]** | `async_page_reader.cpp` | 指标失真 |
| `reinterpret_cast<const float*>` 依赖分配器对齐，严格别名问题 **[新]** | `disk_page_codec.cpp:168` | UB |
| 页格式为原生小端、无字节序标记；依赖 `sizeof(size_t)` | §2.9 | 可移植性 |

### 2.2 评测与数据层

| 问题 | 位置 | 类型 |
| --- | --- | --- |
| I/O 指标为调用计数，非物理 I/O **[新]** | `sift_search_benchmark.cpp:1096-1101` | 指标失真 |
| Recall 采样默认 1.0 时，对全 base 的精确回算被计入读延迟（已修复默认值） | §1.7 | 指标失真 |
| 缓存命中率混入"会话内重复访问 + pin 生命周期"，与 `bench_mixed_rw` 的 0 不可比 **[新]** | `query_page_session.cpp`、`disk_graph_index.cpp:763` | 指标失真 |
| `visited` 把"仅作为邻居被发现"的节点也算进"访问" **[新]** | `sift_search_benchmark.cpp` | 指标失真 |
| `prefetch_wasted_pages` 可能大于 `prefetch_submitted`（查询内集合 vs 累计值） **[新]** | 同上 | 口径混淆 |
| `rss_within_budget` 用进程峰值直接比预算，与 `estimated_resident_ratio` 口径不同 **[新]** | `sift_search_benchmark.cpp:1244-1256` | 口径混淆 |
| `--cache-condition cold/hot` 只改字符串，`hot` 额外追加 warmup，两者不是同一口径 | §2.12 | 口径不一致 |
| `--memory-budget-enforce 1` 可能把 cache 截到 0 页，却仍报 `memory_budget_pass: true` **[新]** | `sift_search_benchmark.cpp:891-909` | 误导 |
| C++ 与 Python 两个数据集生成器实现不同（float vs float64），却宣称"字节级相同" **[新]** | `scripts/make_repro_dataset.*` | 不可复现 |
| 混合读写调度器严格串行轮转，读 QPS = 写吞吐 × 读写比 **[新]** | `mixed_rw_benchmark.cpp:1237-1262` | 结论无效 |
| `summarize_prefetch_ab.py` 存在 KeyError 路径；A/B 门槛从未真正跑过 | §2.10、**[新]** | 工具缺陷 |
| 混合读写 `summary.md` 整栏为 `-`；脚本默认 duration 与文档要求不符 **[新]** | `run_repro_experiments.cpp:38` | 文档漂移 |

### 2.3 文档与仓库层

| 问题 | 位置 | 类型 |
| --- | --- | --- |
| 结论与原始数据不符：同一 delta 规模得出不同曲线；"不可横向比较"后紧跟横向比较的结论 **[新]** | `mixed-read-write.md` §3.1–3.4 | 结论错误 |
| 三处推荐 `search_width` 不一致（128 / 156 / 350） **[新]** | README、experiments/README、PROJECT_PLAN | 口径矛盾 |
| `reproducibility.md` 称基线 RSS 在"预热后"，实测是预热前 **[新]** | §4 | 口径漂移 |
| 二进制未入库、manifest 里的 SHA 指向已删除的旧 exe、`git_commit` 为 unknown **[新]** | `results/` | 可追溯性 |
| 文档引用已删除的 `QueryProcessor`、`LsmWriteManager`、`DiskIndexReader` 等 | §3.1 | 文档漂移 |
| 提交历史：5 个"大功告成！"、多个提交者身份变体、85 文件改动只写"收缩预取" | §3.5 | 可追溯性 |
| `.pptx`、`Zone.Identifier` 进过版本库 | §3.5 | 仓库卫生 |
| 构建未锁 `CMAKE_BUILD_TYPE`、无 `-Wall`、无 CI、只在 WSL2 验证过 | §3.4 | 工程规范 |

---

## 3. 答辩风险排行

按"评审最可能追问、最难圆回来"的顺序：

1. **"内存不超 10%–20% 吗？"** → 实测超 1.8 倍，不能圆，只能承认并说明原因与后续方向。
2. **"这些延迟数字是磁盘 I/O 吗？"** → 不是，系统页缓存装得下，不能说"I/O 优化"。
3. **"Graph-Aware 2Q 和 LRU 比怎么样？"** → 实测更慢，不能写"优于"。
4. **"新写入的数据怎么被检索到？"** → 只能靠 delta 线性扫描，磁盘图里没有，不能说"增量接入"。
5. **"高并发怎么做的？"** → 每线程一份索引，内存 ×N，全局锁串行，不能说"无锁高并发"。
6. **"按 README 能复现吗？"** → 当前构建环境已不完整，需要说明限制。
7. **"能接到 LangChain / 主流 Agent 框架吗？"** → 没有 C API 或 Python 绑定，直接丢分。

---

## 4. 下次怎么做

按收益排序，也是建议的修复顺序：

1. **先修评测口径，再跑任何数字**
   冷页 + `drop_caches` + `/proc/diskstats` 记录物理读；统计字段一律配"应等于 X"的单测；
   删掉所有"统计了但从不赋值"的字段，或把它们标成"不可用"。

2. **约束类指标改成硬失败**
   内存预算超了就让进程失败或自动收敛 cache 容量，不要只写 warning；报告用实测 RSS，不用模型值。

3. **实验结论改成"判据 + 曲线"**
   先写判据（Recall ≥ X 时 I/O 最小），再报整条曲线，不提供事后挑选的硬编码"最优点"。

4. **对外表述与代码逐条对账**
   建一张"宣称 → 代码位置 → 状态"的表，设计文档开头统一标注状态，`PROJECT_PLAN` 里过时的名字全部清掉。

5. **补最小的正确性保障**
   ASan/UBSan 构建、一次 crash 注入恢复测试、一个跑在 Linux 真 io_uring 上的 contract smoke、CI。

6. **合规与可复现**
   锁 Release 构建、记录编译器与 CPU/磁盘型号、二进制或构建命令随结果入库、提交历史规范化（答辩前不改历史，但新提交用规范信息）。

7. **如果时间不够**：宁可把项目收缩成"**一个诚实的原型 + 一份完整的问题清单**"，也不要保留 7 个名不副实的"创新点"。诚实交付的分数通常高于夸大后被拆穿。

---

## 附：本轮审计的边界

- 静态阅读 + git 历史比对为主；本机为 Windows、无 Linux 运行时，未验证 io_uring 与 `O_DIRECT` 路径。
- 子代理报告中"graph-aware-2q 默认被关闭"一条经复核**不成立**：`protect_hot_pages` 在参数结构中默认即为 `true`（`sift_search_benchmark.cpp`），修订后保留入度统计。
- 文中所有 **[新]** 条目均对过源码或 `results/` 下的 JSON，但未重新构建或复跑基准。
