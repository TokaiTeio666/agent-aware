赛题项目
命名日期	大赛名称
2026-03-20	2026年全国大学生计算机系统能力大赛-操作系统设计赛(全国)-OS功能挑战赛道
赛题中文名称	赛题英文名称
面向 Agent记忆的向量检索系统I/O 优化	I/O Optimization of Vector Retrieval Systems for Agent Memory
赛题难度等级	赛题类型
B	学术型
信息维护
赛题维护单位名称：南开大学、赛题维护人姓名：崔立骁、邮箱：cuilx@nankai.edu.cn
赛题背景
在大模型 Agent 的实际应用中，其“长时记忆”不仅规模庞大（远超物理内存），而且是高度动态的。Agent 会在交互过程中不断产生新的记忆（实时写入），并频繁回忆历史关联信息（高频读取）。

当这些高维向量被组织成图索引（如 HNSW）并存储于 SSD 等外存时，传统的操作系统文件 I/O 机制面临巨大挑战：图遍历检索导致的极度随机读使 OS 默认的顺序预取和 Page Cache 失效；而实时的高频随机写则会导致严重的写放大。

本赛题要求选手在内存受限的环境下，针对agent记忆场景，设计高性能的I/O优化的向量检索系统，打破操作系统的 I/O 性能墙，作为未来agent的底层存储引擎使用。

赛题任务
设计并实现一个针对动态向量检索的底层 I/O 存储引擎。系统需在严格受限的内存（如限制为数据集大小的 10%-20%）下运行，召回率（Recall@10）保证大于等于85%。同时处理高并发的 Top-K 检索请求和实时的向量插入/更新请求。参赛选手可以实现下面的任务，也可以自行提出I/O优化方案：

读优化：系统级 I/O 缓存设计
放弃面对向量检索低效的操作系统默认Page Cache机制。在用户态实现一个针对向量检索访问特征和agent memory负载的高效缓存池。要求设计合理的缓存淘汰策略，最大化减少I/O次数。

读优化：向量检索感知的 I/O 预取
突破传统 OS 的顺序预取局限。选手可结合向量检索索引的拓扑连通性，设计“下一跳”预取逻辑。要求使用先进的异步 I/O 框架（如 Linux io_uring），在 CPU 计算当前节点距离时，后台非阻塞地并行拉取相邻节点的数据，从而隐藏磁盘 I/O 延迟。

写优化：高并发下的实时写优化与合并
更新磁盘上的向量索引可能导致灾难性的随机写延迟。选手可借鉴 LSM-Tree（日志结构合并树）的思想，将实时的随机写操作转化为对磁盘/SSD的顺序追加写。需实现后台的异步合并与重整机制，确保在有大量写入负载的背景下，检索（读）延迟不发生严重抖动。

赛题特征
I/O优化策略： 读取优化（如缓存替换、异步预取策略）能根据内存容量约束和实际负载动态调整。更新优化可有效降低写入I/O开销。
透明集成： 作为底层存储提供标准 API 封装，无需修改上层主流 Agent 框架（如 LangChain）的逻辑。
性能评估： 以读写混合负载下的吞吐率（QPS）、查询延迟以及召回率（查询准确性）为核心评估指标。
硬件适配： 支持 Intel/AMD等多核 CPU 架构或者国产处理器，要求深度结合并利用高速外存设备的特性。
参考资料
[1] Subramanya, S. J., Devvrit, F., Simhadri, H. V., Krishnawamy, R., & Kadekodi, R. (2019). DiskANN: Fast accurate billion-point nearest neighbor search on a single node. Proceedings of the 33rd International Conference on Neural Information Processing Systems, 13766 – 13776.
[2] Wang, J., Xia, C., et al. (2021). Milvus: A purpose-built vector data management system. Proceedings of the 2021 International Conference on Management of Data (SIGMOD), 2614-2627.
[3] DiskANN, https://github.com/microsoft/DiskANN
[4] SIFT数据集, http://corpus-texmex.irisa.fr/

评审要点
性能指标 (25%)，核心考核高并发混合读写下的吞吐量（QPS）与延迟、以及查询准确性（召回率）
创新性 (25%)，相比现有向量检索方案在I/O优化上的新颖程度
代码质量(25%)
文档完整性(25%)