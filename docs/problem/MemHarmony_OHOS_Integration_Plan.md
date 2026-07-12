# MemHarmony 原生向量引擎嵌入 HarmonyOS NEXT 实施计划

## 1. 文档目标

本计划用于指导现有 Agent-Aware C++ 向量检索与动态存储能力迁移到 HarmonyOS NEXT Agent 项目中，并完成从“桥接层可迁移”到“真机可运行、可恢复、可被 AgentMemoryService 调用”的完整接入。

当前阶段的目标不是直接宣布生产可用，而是按以下顺序逐步验收：

1. 原生动态库能够由 OHOS ARM64 工具链成功构建。
2. ArkTS 能够加载 N-API 模块并调用基础接口。
3. V3 索引、PQ 文件和动态层目录能够在应用沙箱内正确访问。
4. 真机完成向量搜索、写入、删除、刷新和关闭的最小闭环。
5. 在最小闭环稳定后，再接入 AgentMemoryService、文本元数据和 Embedding 流程。
6. 最终完成异常恢复、性能、内存和资源释放测试。

---

## 2. 当前能力边界

### 2.1 已具备的迁移基础

当前原生桥接层已具备以下迁移条件：

- 已存在 C++ 向量检索和动态存储核心。
- 已存在或已规划 N-API 模块注册入口。
- ArkTS 侧可通过 `NativeVectorStore` 封装原生接口。
- 原生接口能够覆盖基础生命周期：
  - `open`
  - `searchVector`
  - `upsertVector`
  - `deleteVector`
  - `flush`
  - `close`
- 支持数值向量和数值 ID 的基础读写。
- 支持 V3 索引文件、PQ companion 文件以及 WAL/MemTable/SSTable 动态层。

### 2.2 当前暂不纳入第一阶段的能力

第一阶段不直接实现以下功能：

- 文本到向量的 Embedding 推理。
- RDB 文本元数据管理。
- metadata filter。
- 完整 Agent 记忆编排。
- 多设备同步。
- 云端向量化。
- 系统级权限或系统签名能力。
- 正式生产级性能结论。

因此，第一阶段迁移完成后，系统只能被描述为：

> 已完成 HarmonyOS NEXT 真机上的原生向量存取与检索链路验证。

不能直接描述为：

> 已完成完整的鸿蒙 Agent 长时记忆服务。

---

## 3. 总体接入架构

```text
ArkUI 页面 / Agent
        |
        v
AgentMemoryService
        |
        +--------------------+
        |                    |
        v                    v
EmbeddingService       MemoryMetadataRepository
        |                    |
        |                    v
        |               HarmonyOS RDB
        |
        v
NativeVectorStore
        |
        v
agentAwareNative N-API Module
        |
        v
C++ StorageEngine
        |
        +---------------------------+
        |             |             |
        v             v             v
V3 Index + PQ     WAL/MemTable    SSTable/Delete
```

### 分层约束

- Agent 不直接调用 `agentAwareNative`。
- Agent 不直接持有 C++ 句柄。
- ArkTS 只通过 `NativeVectorStore` 管理 Native 生命周期。
- `AgentMemoryService` 负责业务编排。
- RDB 只保存文本、标签、时间、来源等元数据。
- Native 层保存向量索引、动态写入状态和持久化文件。
- 数值 ID 是 RDB 与 Native 层之间的关联键。

---

## 4. 建议工程目录

建议在 HarmonyOS 项目中使用如下结构：

```text
MemHarmony/
├── AppScope/
├── entry/
│   └── src/main/
│       ├── ets/
│       │   ├── pages/
│       │   │   └── NativeSmokePage.ets
│       │   ├── services/
│       │   │   ├── AgentMemoryService.ets
│       │   │   ├── EmbeddingService.ets
│       │   │   └── NativeVectorStore.ets
│       │   ├── repositories/
│       │   │   └── MemoryMetadataRepository.ets
│       │   ├── models/
│       │   │   ├── MemoryRecord.ets
│       │   │   ├── NativeSearchResult.ets
│       │   │   └── NativeStoreConfig.ets
│       │   └── common/
│       │       ├── AppPaths.ets
│       │       ├── NativeError.ets
│       │       └── Logger.ets
│       ├── cpp/
│       │   ├── CMakeLists.txt
│       │   ├── napi/
│       │   │   ├── native_module.cpp
│       │   │   ├── native_vector_store_napi.cpp
│       │   │   └── native_vector_store_napi.h
│       │   ├── core/
│       │   │   └── agent-aware/
│       │   ├── adapter/
│       │   │   ├── ohos_file_system.cpp
│       │   │   ├── ohos_file_system.h
│       │   │   ├── ohos_log.cpp
│       │   │   └── ohos_log.h
│       │   └── types/
│       │       ├── native_config.h
│       │       └── native_result.h
│       └── resources/
│           └── rawfile/
│               ├── base.idx
│               └── base.pq
├── oh-package.json5
└── build-profile.json5
```

说明：

- 原生核心代码可先以源码子目录方式接入。
- 后续稳定后再考虑改为静态库或独立 HAR/HSP 组件。
- 索引资源可放在 `rawfile` 中，但运行时需要复制到应用沙箱目录。
- 不应直接在包内资源路径上执行动态写入。

---

## 5. 分阶段实施计划

## 阶段 0：冻结当前 Native 接口和基线

### 目标

在修改 HarmonyOS 工程前，先固定原生接口、输入输出和文件依赖，避免迁移过程中同时重构算法核心。

### 工作项

1. 确认原生模块名统一为：

```text
agentAwareNative
```

2. 确认第一阶段暴露的 N-API 接口：

```text
open(config)
searchVector(vector, topK)
upsertVector(id, vector, operation)
deleteVector(id)
flush()
close()
getStats()
```

3. 明确配置对象字段：

```ts
interface NativeStoreConfig {
  indexPath: string
  pqPath: string
  dynamicDir: string
  dimension: number
  memoryBudgetMB: number
  searchBeam: number
  enablePrefetch: boolean
}
```

4. 固定搜索返回格式：

```ts
interface NativeSearchResult {
  id: number
  distance: number
}
```

5. 记录桌面或 WSL 环境下的基线数据：

- 索引维度。
- 索引文件大小。
- PQ 文件大小。
- 搜索参数。
- 10 次固定查询结果。
- Recall@K 基线。
- 单次搜索耗时。
- open 和 close 行为。
- flush 后生成的文件列表。

### 验收标准

- 接口清单和字段不再随意变化。
- 准备至少 10 个固定测试向量及预期 Top-K 结果。
- 桌面环境可以重复执行 open、search、close。
- 输出一份 `native_baseline.json`。

---

## 阶段 1：创建 HarmonyOS Native 模块骨架

### 目标

让 HarmonyOS 工程能够识别并打包 `agentAwareNative` 动态库。

### 工作项

1. 在 `entry/src/main/cpp` 下建立 CMake 工程。
2. 加入 N-API 模块注册文件。
3. 将原生核心代码以子目录形式加入构建。
4. 配置 OHOS ARM64 构建目标。
5. 将输出库名固定为：

```text
libagentAwareNative.so
```

6. 确保模块注册名与 ArkTS import 名一致。

### CMake 接入原则

```cmake
cmake_minimum_required(VERSION 3.5.0)
project(agentAwareNative)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(agentAwareNative SHARED
    napi/native_module.cpp
    napi/native_vector_store_napi.cpp
    adapter/ohos_file_system.cpp
    adapter/ohos_log.cpp
)

add_subdirectory(core/agent-aware)

target_include_directories(agentAwareNative PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/napi
    ${CMAKE_CURRENT_SOURCE_DIR}/core/agent-aware
)

target_link_libraries(agentAwareNative PUBLIC
    libace_napi.z.so
    hilog_ndk.z
    agent_aware_core
)
```

> 实际链接库名称应以当前 OHOS SDK 和工程模板生成结果为准。第一阶段重点是确认 N-API、日志和原生核心能够共同链接。

### 验收标准

- DevEco Studio 中 Native 编译通过。
- ARM64 `.so` 被正确生成。
- HAP 中包含 `libagentAwareNative.so`。
- 无未解析符号。
- 模块注册函数在运行时可被执行。

---

## 阶段 2：实现 N-API 适配层

### 目标

完成 ArkTS 类型与 C++ 类型之间的安全转换，并统一生命周期管理。

### 工作项

1. 实现 `open`：
   - 解析配置对象。
   - 检查路径是否为空。
   - 检查维度是否有效。
   - 创建唯一 Native 实例。
   - 防止重复 open。

2. 实现 `searchVector`：
   - 接收 `Float32Array`。
   - 检查长度等于 dimension。
   - 将数据传入 C++ 搜索。
   - 返回 `{ id, distance }[]`。

3. 实现 `upsertVector`：
   - 校验 ID。
   - 校验向量维度。
   - 支持 `insert`、`update`、`upsert`。
   - 将原生错误转换为 ArkTS 可识别异常。

4. 实现 `deleteVector`、`flush`、`close`。

5. 实现 `getStats`，至少返回：

```ts
interface NativeStoreStats {
  opened: boolean
  searchCount: number
  upsertCount: number
  deleteCount: number
  cacheHitRate: number
  memoryUsageBytes: number
  walBytes: number
  lastErrorCode: number
}
```

6. 建立统一错误码：

| 错误码 | 含义 |
|---|---|
| 1001 | Store 未打开 |
| 1002 | Store 已打开 |
| 1003 | 向量维度错误 |
| 1004 | 索引文件不存在 |
| 1005 | PQ 文件不存在 |
| 1006 | 动态目录不可写 |
| 1007 | Native 内存分配失败 |
| 1008 | 搜索失败 |
| 1009 | WAL 写入失败 |
| 1010 | flush 失败 |
| 1011 | close 失败 |

### 生命周期要求

- Native 实例只能由 `NativeVectorStore` 管理。
- 页面销毁不等于立即销毁 Native Store。
- 应用退出、显式停用或测试结束时调用 `close`。
- `close` 必须具备幂等性。
- `open` 失败后不得留下半初始化实例。

### 验收标准

- 所有接口均能被 ArkTS 调用。
- 错误输入不会导致应用崩溃。
- 重复 close 不崩溃。
- 未 open 时调用 search 能返回明确错误。
- 维度不匹配能返回明确错误。

---

## 阶段 3：实现 ArkTS NativeVectorStore

### 目标

在 ArkTS 中建立唯一、安全、可测试的原生访问入口。

### 建议接口

```ts
export class NativeVectorStore {
  private opened: boolean = false

  async open(config: NativeStoreConfig): Promise<void>

  async searchVector(
    vector: Float32Array,
    topK: number
  ): Promise<NativeSearchResult[]>

  async upsertVector(
    id: number,
    vector: Float32Array,
    operation: 'insert' | 'update' | 'upsert'
  ): Promise<void>

  async deleteVector(id: number): Promise<void>

  async flush(): Promise<void>

  async getStats(): Promise<NativeStoreStats>

  async close(): Promise<void>
}
```

### 实现要求

- 封装所有模块 import。
- 不允许业务层直接 import `agentAwareNative`。
- 在 ArkTS 层再次做参数检查。
- 统一记录调用耗时。
- 将 Native 错误转换为业务错误。
- 记录 open、flush、close 的状态变化。
- 支持测试环境注入 Mock 实现。

### 验收标准

- 页面和 Agent 层无法绕过 `NativeVectorStore` 直接访问 Native 模块。
- open、search、upsert、delete、flush、close 能串联执行。
- 错误日志中包含阶段、接口和错误码。
- 生命周期状态不会出现重复打开或关闭后继续访问。

---

## 阶段 4：准备应用沙箱内的数据目录

### 目标

解决 HarmonyOS 包内只读资源与运行时可写目录之间的差异。

### 目录规划

```text
<Application Sandbox>/
├── files/
│   └── memharmony/
│       ├── index/
│       │   ├── base.idx
│       │   └── base.pq
│       ├── dynamic/
│       │   ├── wal/
│       │   ├── memtable/
│       │   ├── sstable/
│       │   └── tombstone/
│       ├── temp/
│       └── logs/
```

### 启动流程

```text
应用首次启动
  |
  v
检查 files/memharmony/index/base.idx
  |
  +-- 存在 --> 校验版本与大小
  |
  +-- 不存在 --> 从 rawfile 复制到沙箱
                    |
                    v
               校验复制结果
                    |
                    v
            创建 dynamic 子目录
                    |
                    v
             调用 Native open
```

### 工作项

1. 建立 `AppPaths.ets`。
2. 获取应用沙箱路径。
3. 首次启动时复制 `base.idx` 和 `base.pq`。
4. 写入临时文件后再原子重命名，避免中途退出产生半文件。
5. 复制完成后校验：
   - 文件存在。
   - 文件大小正确。
   - 可读。
   - 索引版本匹配。
6. 创建动态层目录。
7. 启动前进行可写测试。
8. 不允许 Native 代码自行猜测路径。
9. 所有绝对路径必须由 ArkTS 配置传入。

### 验收标准

- 第一次安装可自动准备索引文件。
- 第二次启动不重复复制。
- 删除索引后重新启动可自动恢复。
- 动态目录可以创建、写入、重命名和删除测试文件。
- 包内资源与运行目录严格分离。

---

## 阶段 5：完成最小 Native Smoke Test

### 目标

在真机上验证向量存取和检索的最小闭环，不先接完整 Agent。

### Smoke 页面

新增：

```text
NativeSmokePage.ets
```

页面提供以下按钮：

- 准备索引。
- Open。
- Search Zero Vector。
- Search Fixed Vector。
- Upsert。
- Delete。
- Flush。
- Get Stats。
- Close。
- Run Full Smoke。

### 最小测试流程

```ts
await store.open(config)

const query = new Float32Array(512)
const before = await store.searchVector(query, 5)

await store.upsertVector(9000001, query, 'upsert')
const afterInsert = await store.searchVector(query, 5)

await store.flush()
await store.close()

await store.open(config)
const afterRestart = await store.searchVector(query, 5)

await store.deleteVector(9000001)
await store.flush()
await store.close()
```

### 必测场景

1. 正常 open。
2. 重复 open。
3. 正常 search。
4. 错误维度 search。
5. topK 为 0、负数和过大值。
6. 新增向量。
7. 更新已有向量。
8. 删除向量。
9. flush 后重启。
10. close 后再次 close。
11. close 后 search。
12. 索引文件缺失。
13. PQ 文件缺失。
14. 动态目录只读或不可创建。
15. 应用切到后台再恢复。
16. 连续执行 100 次搜索。
17. 连续执行 100 次混合 CRUD。

### 验收标准

- 真机完成完整 smoke 流程。
- 应用不崩溃。
- 插入数据在重启后可恢复。
- 删除状态在重启后仍有效。
- 连续搜索无明显内存持续增长。
- close 后所有后台任务退出。
- 输出 `native_smoke_report.json`。

---

## 阶段 6：接入 AgentMemoryService

### 目标

将 NativeVectorStore 从测试页面依赖升级为业务服务内部依赖。

### 建议接口

```ts
export interface MemoryRecord {
  id: number
  text: string
  source: string
  tags: string[]
  createdAt: number
  updatedAt: number
}

export interface MemorySearchItem {
  id: number
  text: string
  score: number
  source: string
  tags: string[]
}

export class AgentMemoryService {
  async initialize(): Promise<void>

  async addMemory(record: MemoryRecord): Promise<void>

  async updateMemory(record: MemoryRecord): Promise<void>

  async deleteMemory(id: number): Promise<void>

  async searchMemory(
    text: string,
    topK: number
  ): Promise<MemorySearchItem[]>

  async flush(): Promise<void>

  async shutdown(): Promise<void>
}
```

### 写入流程

```text
用户提交文本
  |
  v
EmbeddingService 生成向量
  |
  v
RDB 开启事务
  |
  +--> 写入文本元数据
  |
  +--> NativeVectorStore.upsertVector
  |
  +--> 写入状态标记
  |
  v
提交事务 / 失败补偿
```

### 搜索流程

```text
查询文本
  |
  v
EmbeddingService
  |
  v
NativeVectorStore.searchVector
  |
  v
得到 ID + distance
  |
  v
RDB 批量查询文本元数据
  |
  v
重排并返回给 Agent
```

### 一致性策略

第一版建议采用“RDB 主记录 + Native 状态字段”：

```text
PENDING_VECTOR
READY
PENDING_DELETE
FAILED
```

#### 新增

1. RDB 写入 `PENDING_VECTOR`。
2. Native upsert。
3. 成功后更新为 `READY`。
4. 失败则保留记录，供恢复任务重试。

#### 删除

1. RDB 标记 `PENDING_DELETE`。
2. Native delete。
3. 成功后删除 RDB 记录或标记已删除。
4. 失败则启动恢复任务。

### 验收标准

- Agent 业务层只依赖 `AgentMemoryService`。
- 文本新增后能被检索。
- 更新后旧向量不可继续命中。
- 删除后不再返回。
- Native 失败时 RDB 有明确状态。
- 应用重启后可以恢复未完成任务。

---

## 阶段 7：实现恢复状态机

### 目标

处理应用异常退出、强制终止、写入中断和 flush 未完成等问题。

### 启动恢复顺序

```text
1. 检查索引和 PQ 文件
2. 检查动态目录
3. 打开 Native Store
4. Native 回放 WAL
5. 读取 RDB 中的非 READY 状态
6. 重试 PENDING_VECTOR
7. 重试 PENDING_DELETE
8. 校验 RDB 与 Native 计数
9. 恢复完成后开放业务接口
```

### 故障注入测试

在以下位置强制终止应用：

- RDB 写入后、Native upsert 前。
- Native upsert 后、RDB 状态更新前。
- delete 标记后、Native delete 前。
- Native delete 后、RDB 删除前。
- flush 执行中。
- close 执行中。
- 索引复制中。
- WAL 写入后但未落盘完成。

### 验收标准

- 所有故障点重启后均可恢复到可解释状态。
- 不出现同一 ID 多版本不可控并存。
- 不出现 RDB 显示存在但 Native 永久缺失且无法修复。
- 不出现删除记录重启后重新出现。
- 输出恢复测试报告。

---

## 阶段 8：真机性能与资源验收

### 目标

在功能稳定后评估是否满足比赛演示和端侧运行要求。

### 测试指标

#### 启动

- 首次索引复制耗时。
- 非首次启动耗时。
- Native open 耗时。
- WAL 回放耗时。
- AgentMemoryService 可用时间。

#### 搜索

- Avg latency。
- P50。
- P95。
- P99。
- Recall@10。
- QPS。
- 缓存命中率。
- 读取页数。
- demand read wait。
- prefetch ready/pending/unused。

#### 写入

- 单条新增耗时。
- 单条更新耗时。
- 单条删除耗时。
- flush 耗时。
- WAL 增长速度。
- SSTable 合并耗时。

#### 资源

- Native 堆内存。
- ArkTS 堆内存。
- 总 PSS。
- 峰值内存。
- 文件占用。
- CPU 占用。
- 温度和能耗。
- 后台恢复后的资源占用。

### 建议资源档位

| 档位 | 内存预算 | Beam | Prefetch | 场景 |
|---|---:|---:|---|---|
| Low | 64 MB | 8 | 关闭或低预算 | 低内存设备 |
| Balanced | 128 MB | 16 | 自适应 | 默认演示 |
| Performance | 256 MB | 32 | 开启 | 性能测试 |

### 验收标准

- Recall@10 不低于项目目标。
- 搜索过程中无 OOM。
- 连续运行 30 分钟无明显内存泄漏。
- 资源档位切换后行为可解释。
- Prefetch 必须通过真机 A/B 数据证明收益后才能默认开启。
- WSL 数据不能替代真机数据。

---

## 6. 三人任务分工

### 成员 A：Native 与 CMake

负责：

- OHOS CMake 接入。
- N-API 模块注册。
- Native 类型转换。
- 文件系统适配。
- WAL、flush、close 验证。
- 原生错误码和日志。
- Native 性能指标。

### 成员 B：ArkTS 与应用沙箱

负责：

- `NativeVectorStore`。
- `AppPaths`。
- rawfile 复制。
- Native Smoke 页面。
- 生命周期处理。
- RDB Repository。
- AgentMemoryService。

### 成员 C：测试、数据与文档

负责：

- 固定测试向量。
- 桌面基线。
- 真机 smoke。
- 故障注入。
- 性能测试脚本。
- 测试报告。
- 演示脚本。
- 风险和问题清单。

### 协作约束

- Native 接口由三人共同确认后冻结。
- 每天合并前必须跑 smoke。
- 不允许在同一提交中同时大改核心算法和 N-API 接口。
- 每个阶段单独验收后再进入下一阶段。

---

## 7. 最小验收清单

### 构建

- [ ] OHOS ARM64 编译成功。
- [ ] `libagentAwareNative.so` 被打包进 HAP。
- [ ] 模块注册名为 `agentAwareNative`。
- [ ] ArkTS 能成功 import。

### 文件

- [ ] `base.idx` 能从 rawfile 复制到沙箱。
- [ ] `base.pq` 能从 rawfile 复制到沙箱。
- [ ] 动态层目录可读写。
- [ ] 不存在桌面 Linux 绝对路径。
- [ ] 索引版本和维度校验通过。

### Native

- [ ] open 成功。
- [ ] search 成功。
- [ ] upsert 成功。
- [ ] delete 成功。
- [ ] flush 成功。
- [ ] close 成功。
- [ ] 重复 close 不崩溃。
- [ ] 参数错误不会导致崩溃。

### 恢复

- [ ] flush 后重启数据仍存在。
- [ ] delete 后重启记录不再出现。
- [ ] 强制终止后 WAL 能恢复。
- [ ] RDB 非 READY 状态能自动修复。
- [ ] 索引复制中断能够重新执行。

### 性能

- [ ] 连续 100 次搜索稳定。
- [ ] 连续 100 次混合 CRUD 稳定。
- [ ] 连续运行 30 分钟无明显内存增长。
- [ ] Recall@10 达到项目要求。
- [ ] P95/P99 有真机记录。
- [ ] Prefetch 有开关对照数据。

---

### 8. 阶段完成定义

### M1：Native 可加载

完成条件：

- `.so` 成功打包。
- ArkTS 能调用 open/search/close。

### M2：向量链路可运行

完成条件：

- 真机完成 search、upsert、delete、flush。
- 动态数据重启后可恢复。

### M3：AgentMemoryService 可用

完成条件：

- 文本、向量和元数据完成业务闭环。
- Agent 不直接调用 Native。

### M4：真机稳定性通过

完成条件：

- 连续搜索、混合 CRUD、故障恢复、close/teardown 通过。
- 无明显崩溃和内存泄漏。

### M5：比赛演示版本

完成条件：

- 固化 HAP。
- 固化索引和演示数据。
- 完成性能表、测试报告和演示脚本。
- 所有性能结论均来自真机。

---

## 9. 下一步立即执行事项

建议当前仓库立即完成以下五项：

1. 核对实际文件名：
   - N-API 注册文件。
   - ArkTS Native 封装文件。
   - CMakeLists。
   - StorageEngine 入口。
   - V3 索引和 PQ 文件名。

2. 新建分支：

```bash
git checkout -b harmony/native-integration
```

3. 将 Native 核心放入：

```text
entry/src/main/cpp/core/agent-aware/
```

4. 先只暴露：

```text
open
searchVector
close
```

5. 创建 `NativeSmokePage.ets`，在真机完成第一轮：

```text
准备索引 -> open -> search -> close
```

第一轮通过后，再继续加入 upsert、delete、flush 和 AgentMemoryService，避免一次性接入过多模块导致问题难以定位。
