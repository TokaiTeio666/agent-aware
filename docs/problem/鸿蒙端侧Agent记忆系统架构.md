# 鸿蒙端侧 Agent 记忆系统参赛文档包

## 总结

以现有 Agent-aware C++ 向量检索引擎为基础，设计“鸿蒙端侧 Agent 长时记忆智能存储与检索服务”。原型运行于 HarmonyOS NEXT 真机，采用普通 HAP、ArkTS/ArkUI、N-API 和原生 C++，不依赖系统签名。

文档将清楚区分：

- 已验证能力：Vamana、PQ、Graph-Aware 2Q、预取、WAL/MemTable/SSTable、Recall 与内存预算。
- 鸿蒙适配设计：N-API、应用沙箱、端侧嵌入模型、平台 I/O 后端。
- 待真机验证指标：所有 HarmonyOS 延迟、QPS、能耗和内存数据，禁止用 WSL 数据代替。

## 文档交付

在 `docs/` 下建立完整中文文档包，并从根 `README.md` 添加入口：

- `README.md`：项目定位、赛道要求映射、文档导航、能力状态矩阵。
- `01-technical-solution.md`：需求、总体架构、数据流、核心创新、安全隐私、故障恢复和边界。
- `02-interface-specification.md`：ArkTS、N-API、C++ 引擎及平台 I/O 接口规范。
- `03-deployment-and-demo.md`：DevEco/HAP 构建结构、资源部署、真机安装、五分钟演示流程。
- `04-test-plan-and-report.md`：功能、性能、稳定性、能耗、消融实验及报告模板。

总体架构固定为：

```text
ArkUI 演示与性能仪表盘
        |
AgentMemoryService（ArkTS）
├── LocalEmbeddingProvider
├── MetadataRepository（鸿蒙 RDB）
└── NativeVectorStore
        |
N-API 异步桥接
        |
Agent-aware C++ Engine
├── Vamana + PQ/ADC
├── Graph-Aware 2Q
├── 拓扑/学习型预取
├── WAL + MemTable + SSTable
└── Telemetry
        |
Platform I/O Adapter
├── Harmony POSIX/线程池后端
└── Linux io_uring 后端
        |
应用沙箱中的索引、WAL、SSTable、模型与元数据
```

演示文本使用可替换的端侧轻量嵌入模型，默认规划为 MindSpore Lite 加载量化版 `bge-small-zh-v1.5`；SIFT/预计算向量继续作为可复现性能基准。不同维度使用独立索引实例，不混合写入。

## 接口设计

ArkTS 对应用暴露 `AgentMemoryService`：

- `initialize(options): Promise<InitResult>`
- `putMemory(input): Promise<MemoryRecord>`
- `updateMemory(id, patch): Promise<MemoryRecord>`
- `searchMemory(request): Promise<SearchResponse>`
- `deleteMemory(id): Promise<void>`
- `flush(): Promise<void>`
- `getStats(): Promise<EngineStats>`
- `runBenchmark(config): Promise<BenchmarkReport>`
- `shutdown(): Promise<void>`

核心类型包括：

- `MemoryInput`：文本、来源、时间、标签和可选 `uint32` ID。
- `SearchRequest`：查询文本、`topK`、过滤条件和可选超时。
- `SearchItem`：ID、文本、距离、元数据和排序位置。
- `EngineStats`：P95/P99、页读取、缓存命中、预取命中、I/O 模式、内存预算、动态序列号。
- `BenchmarkReport`：环境、参数、Recall、QPS、延迟、读放大、能耗和原始结果路径。

N-API 模块 `agentAwareNative` 只处理向量和引擎生命周期：

- `open(config)`
- `upsert(id, Float32Array)`
- `search(Float32Array, topK)`
- `erase(id)`
- `flush()`
- `stats()`
- `close()`

所有模型推理与磁盘操作均返回 `Promise`，在原生工作线程执行。跨 N-API 边界时复制输入到原生自有缓冲区，避免异步任务持有失效的 ArkTS 内存。

C++ 层增加文档化的平台抽象 `PageIoBackend`，定义单页读取、批量提交、等待、刷新、取消和状态查询；鸿蒙默认使用线程池并行 `pread`，Linux 保留 `io_uring`。接口统一返回明确错误码，包括未初始化、参数错误、维度不匹配、索引不兼容、模型失败、I/O 错误、繁忙和取消。

元数据与向量采用恢复友好的两阶段状态：

1. RDB 创建 `pending` 记录并分配 ID。
2. 生成嵌入并写入原生 WAL/增量索引。
3. RDB 标记为 `active`。
4. 重启时清理或重放 `pending/deleting` 状态；检索只返回 `active` 元数据。

## 测试与验收

- 功能：初始化、文本写入、语义搜索、更新、删除、flush、重启恢复、模型失败、索引损坏、维度错误和应用生命周期切换。
- 接口：ArkTS 参数校验、N-API Promise 行为、并发调用、错误码、超时及重复关闭。
- 性能：SIFT1M 上 Recall@10 `>= 0.95`、常驻内存比例 `<= 20%`，记录 QPS、P50/P95/P99、reads/query、缓存命中和预取有效率。
- 对比：无缓存/LRU/Graph-Aware 2Q，无预取/拓扑预取/学习型预取，同步 `pread`/线程池异步读取。
- 混合负载：100/0、95/5、80/20 读写比例，并发度 1/4/8，记录读写吞吐、尾延迟、compaction 和恢复时间。
- 优化验收：在 Recall 不下降且内存达标时，相对鸿蒙同步基线达到 P95 降低 `>= 15%` 或 QPS 提升 `>= 20%`。
- 语义质量：自建 100 条中文记忆测试集，Top-5 目标命中率 `>= 90%`。
- 真机测试：记录机型、系统版本、SDK、温度、电量、存储余量和性能模式；至少三轮取中位数。

现有 `ctest` 合约测试已通过。README 中 Recall `0.9917/0.9940`、resident ratio `0.199` 等数据仅作为 Linux/WSL 历史基线，并附来源；HarmonyOS 表格在真机实验前统一标记“待验证”。

## 假设与边界

- 目标设备为 HarmonyOS NEXT 商用手机，使用普通应用签名和应用沙箱。
- 本次交付只编写架构、接口、部署演示和测试文档，不实现 ArkTS、N-API 或 HAP 代码。
- 不将方案描述为 System Ability、内核模块或系统特权服务。
- 不依赖云端嵌入、云端检索或跨设备同步，所有记忆和模型数据默认留在本机。
- 设备、SDK、MindSpore Lite 和模型的实际版本由真机环境确认后填入部署文档，不虚构兼容性结论。