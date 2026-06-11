# AgentMem-Flow M2 分析报告

## 版本信息

- 大版本：M2 - 异步 I/O 与预取层
- 合并旧版本：V6，并吸收后续真实 O_DIRECT/io_uring 改造
- 对应计划：真实文件 I/O、O_DIRECT、io_uring、next-hop 预取、20% 内存限制验证
- 状态：已完成基础实现，仍需扩大 SIFT1M 参数扫描

## 合并范围

M2 以 V6 为基础，但不等同于 V6。原 V6 主要完成 AutoDL/Linux 部署、真实文件写入与 compaction I/O 统计；按照新计划，M2 进一步要求读路径也必须进入真实底层 I/O 模式：

- 显式支持 `O_DIRECT`，绕过 OS Page Cache。
- 显式支持 `io_uring`，用异步提交隐藏随机读延迟。
- 在搜索过程中进行 next-hop 预取。
- 在 20% 数据集大小以内的内存限制下运行并输出内存账本。

## 核心能力

### 真实文件 I/O

V6 将 compaction 从纯时间模拟推进到真实文件写入，增加了 compaction path、bytes/vector、`compaction_io_bytes` 等指标，并补齐了 Linux 部署脚本、cache 清理脚本和 AutoDL 运行路径。

这一阶段证明项目可以在 Linux 文件系统和真实 SSD 路径下执行，但原 V6 还不能证明搜索读路径已经绕过 Page Cache。

### O_DIRECT 读路径

M2 要求图页面读取使用 `O_DIRECT` 打开文件，并满足 direct I/O 的对齐要求。页面缓冲区需要按块大小对齐，读取 offset 和 size 也必须按页或块对齐。

该模式下，`io_direct_enabled=1` 且 `io_mode_effective=odirect` 才能视为真实 direct I/O。若系统不支持或打开失败，测试应明确失败或显式记录 fallback，不能静默退回 buffered I/O。

### io_uring 异步读路径

M2 增加 `io_uring` 模式，用 SQ/CQ 异步提交图页面读取。搜索扩展当前节点时，后台可以预取候选邻居页面，前台继续计算距离，从而隐藏部分 SSD 随机读延迟。

有效实验需要输出：

- `io_uring_enabled=1`
- `io_direct_enabled=1`
- `io_uring_submission_count`
- `io_uring_completion_count`
- pending depth 峰值
- 实际 batch 或 queue depth

### 20% 内存限制

M2 的内存约束不只看索引文件大小，还要统计查询工作区、I/O buffer、缓存页面、path cache、PQ 结构等常驻内存。原始 base 向量不能在搜索阶段完整留存于内存中。

验收指标包括：

- `memory_resident_ratio <= 0.20`
- `memory_budget_pass=1`
- `raw_base_released_before_search=1`
- query workspace 与 I/O buffer 字节数可见

## 当前验证摘要

| 场景 | 关键结果 | 结论 |
| --- | --- | --- |
| Synthetic O_DIRECT | `io_mode_effective=odirect`，`io_direct_enabled=1`，`memory_resident_ratio` 约 0.0802 | direct I/O 路径可运行且满足 20% 内存限制 |
| Synthetic io_uring | `io_mode_effective=io_uring`，`io_uring_enabled=1`，pending peak 约 4，`memory_resident_ratio` 约 0.1707 | 异步路径可运行，内存仍在预算内 |
| SIFT1M smoke | `io_uring_enabled=1`，`io_direct_enabled=1`，`raw_base_released_before_search=1`，`memory_resident_ratio` 约 0.0646 | SIFT1M 可在真实 I/O 与内存限制下跑通 |

当前 SIFT1M smoke 的 Recall@10 仍不是最终竞赛参数，只用于证明真实 I/O、内存释放和端到端可运行。最终 Recall/latency 需要在 M4 做更完整的参数扫描。

## 计划问题映射

- P11：文件 I/O 与真实部署，由 V6 解决。
- P12：异步 I/O 和 next-hop 预取，由 M2 的 O_DIRECT/io_uring 路径解决。
- P13：20% 内存预算，由 M2 的内存账本和 release-before-search 验证。

## 阶段结论

M2 是项目从“算法模拟”转向“真实存储系统”的关键层。只有在 O_DIRECT/io_uring 生效后，读放大、预取和缓存策略的收益才不会被 OS Page Cache 掩盖。

下一步 M4 需要把这一层接入更完整的 SIFT1M 评测，区分冷启动、warm cache、自有缓存和 direct I/O 场景下的真实性能。

