# AgentMem-Flow M1 分析报告

## 版本信息

- 大版本：M1 - 受限内存读优化层
- 合并旧版本：V3、V4、V7
- 对应计划：用户态缓存、路径复用、查询签名策略、内存预算下的读取优化
- 状态：已完成

## 合并范围

M1 合并原始报告中的读路径优化阶段：

- V3：Agent-Aware Page Cache，在用户态实现可控页面缓存。
- V4：Query Path Cache，复用相似查询的历史图遍历路径。
- V7：Query Signature Policy Compare，对 routed、simhash、pq-prefix、simhash-pq 等签名策略进行对比。

这一层的目标是在严格内存限制下减少重复读，避免依赖 OS Page Cache 的不可控行为。

## 核心能力

### 用户态页面缓存

V3 引入 `--cache-policy none|lru|agent` 和 `--cache-pages`。缓存从系统 Page Cache 转移到程序可统计、可淘汰、可约束的用户态页面池，并输出 hit/miss、读取页数、缓存命中率等指标。

Agent 策略利用访问模式和图遍历局部性，优先保留更可能被后续查询复用的页面。相比完全无缓存或朴素 LRU，它更适合 agent memory 中会话式、主题连续的访问负载。

### 查询路径复用

V4 引入 path cache，将相似查询的历史入口、候选路径或中间节点作为下一次搜索的初始线索。该策略降低了图搜索从入口点开始重复探索的成本，尤其适用于 session 内连续相关查询。

### 查询签名策略

V7 将路径复用的 key 从单一策略扩展为多种签名：

- routed：基于路由或入口区域的粗粒度签名。
- simhash：基于向量方向的相似性签名。
- pq-prefix：基于量化前缀的桶级签名。
- simhash-pq：组合签名，在命中率和误复用风险之间折中。

签名策略的意义是控制 path cache 的复用粒度：过粗会污染搜索路径，过细会降低命中率。

## 指标摘要

| 阶段 | 关键实验 | 结果 | 结论 |
| --- | --- | --- | --- |
| V3 | Agent-Aware Page Cache | warm run Avg SSD Reads/Query 约 16.0800 | 用户态缓存可进一步降低布局后的读放大 |
| V4 | Query Path Cache | path hit rate 约 0.5533 | 会话式相似查询存在可复用搜索路径 |
| V7 | 签名策略对比 | 多签名策略可比较 hit、reads、latency | path cache 需要负载感知的签名选择 |

## 内存限制讨论

M1 的优化必须服从 10%-20% 数据集大小的内存预算。缓存页数、路径缓存容量、签名表大小和可能的 PQ/ADC rerank 辅助结构都应进入统一内存账本，不能默认将原始向量全量保留在内存中。

对 PQ/ADC rerank 的定位是“预算内的读优化辅助项”：它可以减少候选向量回读或降低 rerank 成本，但如果压缩误差导致 Recall@10 低于目标，则不能替代原始向量或精确重排。

## 计划问题映射

- P4：OS Page Cache 不可控，已由用户态 page cache 解决。
- P5：相似查询重复遍历图，已由 path cache 和 signature policy 解决。
- P10：内存约束下的缓存淘汰策略，在 M1 中形成基础机制，后续继续由 M2/M4 的真实 I/O 实验验证。

## 阶段结论

M1 证明读路径存在明显的 agent-aware 复用空间。页面缓存负责复用物理页，路径缓存负责复用图搜索过程，签名策略负责决定什么时候复用。

这一层的关键边界是：所有缓存收益都必须在显式内存预算内成立。后续 M2 的 O_DIRECT/io_uring 会进一步去掉 OS Page Cache 干扰，从而检验 M1 优化是否真实来自项目自身。

