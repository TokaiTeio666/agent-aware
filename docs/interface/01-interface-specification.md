# MemHarmony 接口规范

> 文档版本：1.0.0  
> 目标平台：HarmonyOS NEXT，API 12+，ARM64  
> 接口语言：ArkTS、Node-API（N-API）、C++17  
> 状态说明：本文是鸿蒙适配的实现合同；仓库中已有的 C++ 能力会标记为“已有”，待开发接口标记为“待实现”。

## 1. 目的与范围

本文定义 MemHarmony 个人知识记忆系统的模块边界、数据类型、方法签名、参数校验、错误码、并发语义、恢复流程和版本兼容规则，作为 ArkTS、端侧嵌入、N-API 与 C++ 引擎之间的联调依据。

接口分为四层：

```text
ArkUI
  |
AgentMemoryService（公开业务接口）
  |-- LocalEmbeddingProvider（文本 -> 512 维向量）
  |-- MetadataRepository（RDB 元数据与事务状态）
  `-- NativeVectorStore（ArkTS 包装）
         |
         `-- agentAwareNative（N-API 向量接口）
                |
                `-- PackedGraphEngine / DynamicWriteManager（C++）
```

边界约束：

- `AgentMemoryService` 是 UI 和 Agent 调用的唯一公开入口。
- `MetadataRepository` 保存文本、来源、标签和状态；C++ 索引不保存业务文本。
- `LocalEmbeddingProvider` 对外保持统一接口，默认实现使用 MindSpore Lite；模型闸门触发时可替换为 NCNN。
- `agentAwareNative` 只接收定长向量、数值 ID 和引擎参数，不接收业务文本或 RDB 对象。
- 所有模型推理、索引访问、WAL、flush 和 benchmark 操作必须异步执行，不得阻塞 ArkUI 线程。
- 本期不包含云端接口、多设备同步、系统特权接口和用户隐私数据采集。

## 2. 全局约定

### 2.1 基础约束

| 项目 | 约定 |
|---|---|
| ID 类型 | `uint32`，ArkTS 使用 `number` 表示，范围 `0..4294967295` |
| 预置数据 ID | `0..4999`，只读，不允许更新或删除 |
| 用户数据 ID | `>= 5000`，由 RDB 持久化序列分配 |
| 默认向量维度 | `512` |
| 向量数据类型 | L2 归一化后的 `Float32Array` |
| 距离度量 | squared L2，值越小越相似 |
| 文本编码 | UTF-8 |
| 最大文本长度 | 8,192 个 UTF-16 code units；模型只处理前 256 tokens |
| `topK` 范围 | `1..100`，默认 `10` |
| 时间戳 | Unix epoch milliseconds，ArkTS `number` |
| 标签数量 | 最多 32 个，每个标签 1..64 个字符 |
| 超时 | 仅限制调用等待时间；是否取消底层工作由方法说明决定 |
| Promise | 成功时 resolve，失败时 reject `MemHarmonyError` |

### 2.2 生命周期状态

```ts
export type ServiceState =
  | 'NEW'
  | 'INITIALIZING'
  | 'READY'
  | 'SHUTTING_DOWN'
  | 'CLOSED'
  | 'FAILED';
```

状态规则：

- `initialize()` 仅允许在 `NEW`、`CLOSED` 或可恢复的 `FAILED` 状态调用。
- CRUD、search、flush、stats 和 benchmark 仅允许在 `READY` 状态调用。
- `shutdown()` 可以重复调用；第二次及后续调用直接成功。
- 初始化与关闭期间的新业务请求以 `E_BUSY` 拒绝。
- 应用切入后台时不强制关闭引擎；仅在 Ability 销毁、显式退出或不可恢复错误时调用 `shutdown()`。

### 2.3 错误对象

```ts
export type MemHarmonyErrorCode =
  | 'E_NOT_INITIALIZED'
  | 'E_ALREADY_INITIALIZED'
  | 'E_BUSY'
  | 'E_INVALID_ARGUMENT'
  | 'E_EMPTY_TEXT'
  | 'E_TEXT_TOO_LONG'
  | 'E_ID_OUT_OF_RANGE'
  | 'E_RESERVED_ID'
  | 'E_NOT_FOUND'
  | 'E_CONFLICT'
  | 'E_DIMENSION_MISMATCH'
  | 'E_MODEL_LOAD_FAILED'
  | 'E_MODEL_INFERENCE_FAILED'
  | 'E_INDEX_NOT_FOUND'
  | 'E_INDEX_INCOMPATIBLE'
  | 'E_INDEX_CORRUPTED'
  | 'E_CHECKSUM_MISMATCH'
  | 'E_IO'
  | 'E_DATABASE'
  | 'E_WAL'
  | 'E_TIMEOUT'
  | 'E_CANCELLED'
  | 'E_RESOURCE_EXHAUSTED'
  | 'E_UNSUPPORTED'
  | 'E_INTERNAL';

export interface MemHarmonyError extends Error {
  name: 'MemHarmonyError';
  code: MemHarmonyErrorCode;
  message: string;
  operation: string;
  retryable: boolean;
  nativeCode?: number;
  details?: Record<string, string | number | boolean>;
}
```

错误规则：

- 参数错误、维度错误、保留 ID、模型/索引不兼容不可直接重试。
- `E_BUSY`、部分 `E_IO`、`E_TIMEOUT` 可由上层退避后重试。
- 错误消息不得包含完整用户文本、向量内容或应用沙箱绝对路径。
- N-API 必须将所有 C++ 异常转换为 Promise rejection，禁止异常跨越 ABI 边界。

## 3. ArkTS 公开业务接口

### 3.1 数据类型

```ts
export type MemorySource =
  | 'USER'
  | 'CAMPUS_DATASET'
  | 'IMPORT'
  | 'AGENT';

export type MemoryStatus =
  | 'PENDING'
  | 'ACTIVE'
  | 'DELETING'
  | 'FAILED';

export type ResourceProfile =
  | 'NORMAL'
  | 'CONSTRAINED'
  | 'CRITICAL';

export interface MemoryInput {
  /** 可选；不传时由服务分配，传入时必须 >= 5000。 */
  id?: number;
  text: string;
  source?: MemorySource;
  occurredAt?: number;
  tags?: string[];
  attributes?: Record<string, string>;
}

export interface MemoryPatch {
  text?: string;
  source?: MemorySource;
  occurredAt?: number;
  tags?: string[];
  attributes?: Record<string, string>;
}

export interface MemoryRecord {
  id: number;
  text: string;
  source: MemorySource;
  occurredAt: number;
  tags: string[];
  attributes: Record<string, string>;
  status: MemoryStatus;
  createdAt: number;
  updatedAt: number;
  revision: number;
}

export interface SearchFilter {
  sources?: MemorySource[];
  tagsAny?: string[];
  tagsAll?: string[];
  occurredFrom?: number;
  occurredTo?: number;
  includePreset?: boolean;
  includeUser?: boolean;
}

export interface SearchRequest {
  text: string;
  topK?: number;
  filter?: SearchFilter;
  timeoutMs?: number;
}

export interface SearchItem {
  id: number;
  text: string;
  source: MemorySource;
  occurredAt: number;
  tags: string[];
  attributes: Record<string, string>;
  distance: number;
  score: number;
  rank: number;
  revision: number;
}

export interface SearchTiming {
  embeddingMs: number;
  nativeSearchMs: number;
  metadataMs: number;
  totalMs: number;
}

export interface SearchResponse {
  requestId: string;
  items: SearchItem[];
  timing: SearchTiming;
  profile: ResourceProfile;
  partial: boolean;
  candidateCount: number;
}
```

`score` 仅用于 UI 展示，按 `1 / (1 + distance)` 计算，不作为跨模型可比较的概率。排序以 `distance` 升序为准；距离相同时按 ID 升序稳定排序。

### 3.2 初始化类型

```ts
export interface AgentMemoryOptions {
  baseIndexPath: string;
  pqModelPath?: string;
  embeddingModelPath: string;
  tokenizerPath: string;
  dynamicDir: string;
  databaseName?: string;
  expectedDimension?: number;
  initialProfile?: ResourceProfile;
  enableRecovery?: boolean;
  validateChecksum?: boolean;
}

export interface InitResult {
  serviceVersion: string;
  nativeVersion: string;
  modelBackend: 'MINDSPORE_LITE' | 'NCNN';
  modelId: string;
  modelVersion: string;
  dimension: number;
  baseVectorCount: number;
  dynamicVectorCount: number;
  recoveredOperationCount: number;
  profile: ResourceProfile;
  ioMode: string;
  warnings: string[];
}

export interface ResetOptions {
  /** Release 构建由确认弹窗生成；测试构建可省略。 */
  confirmationToken?: string;
  flushAfterReset?: boolean;
}

export interface ResetResult {
  deletedRecordCount: number;
  lastDynamicSequence: number;
  flushed: boolean;
}
```

### 3.3 `AgentMemoryService`

```ts
export interface AgentMemoryService {
  initialize(options: AgentMemoryOptions): Promise<InitResult>;
  putMemory(input: MemoryInput): Promise<MemoryRecord>;
  updateMemory(id: number, patch: MemoryPatch): Promise<MemoryRecord>;
  getMemory(id: number): Promise<MemoryRecord | null>;
  searchMemory(request: SearchRequest): Promise<SearchResponse>;
  deleteMemory(id: number): Promise<void>;
  setResourceProfile(profile: ResourceProfile): Promise<void>;
  flush(): Promise<void>;
  getStats(): Promise<EngineStats>;
  runBenchmark(config: BenchmarkConfig): Promise<BenchmarkReport>;
  resetDemoData(options?: ResetOptions): Promise<ResetResult>;
  shutdown(): Promise<void>;
}
```

#### 3.3.1 `initialize`

处理流程：

1. 校验资源文件存在且位于应用可访问目录。
2. 打开 RDB 并执行 schema migration。
3. 加载嵌入模型与 tokenizer，取得模型维度和版本。
4. 打开 base 索引、PQ 模型和动态 WAL/SSTable。
5. 校验模型、PQ 和索引维度一致，默认均为 512。
6. 校验 magic、格式版本和校验和。
7. 恢复 `PENDING`、`DELETING` 和 Native WAL 状态。
8. 应用初始资源档位，完成后切换为 `READY`。

失败时必须关闭已打开资源。索引损坏、维度不一致或模型加载失败时进入 `FAILED`，不得以空索引静默启动。

#### 3.3.2 `putMemory`

参数规则：

- `text.trim()` 不能为空。
- 手动 ID 必须 `>= 5000` 且不存在；重复 ID 返回 `E_CONFLICT`。
- `attributes` 最多 32 项；key 最长 64 字符，value 最长 512 字符。

事务语义：

1. RDB 分配 ID，插入 `PENDING` 记录。
2. 对规范化文本生成向量。
3. 调用 Native `upsert(id, vector, 'INSERT')`，追加 WAL 并发布动态读视图。
4. RDB 将状态改为 `ACTIVE`，`revision=1`。
5. 第 3 步失败时删除或标记 RDB 记录为 `FAILED`；启动恢复不得返回该记录。

Promise 只有在 RDB 已标记 `ACTIVE` 后才 resolve。

#### 3.3.3 `updateMemory`

- `id < 5000` 返回 `E_RESERVED_ID`。
- 不存在或非 `ACTIVE` 返回 `E_NOT_FOUND`。
- patch 不得为空。
- 文本变化时必须重新嵌入并更新 Native 向量；只更新元数据时不得触发向量写入。
- 使用乐观版本更新，成功后 `revision += 1`。
- 文本更新使用 RDB 操作日志：先写入 `PENDING_UPDATE` 操作，再生成新向量。
- Native 向量提交与 RDB 新 revision 提交期间持有服务级更新屏障；新搜索等待该短临界区，避免返回“新向量 + 旧文本”。
- Native 更新成功后，RDB 在同一事务中应用 patch、递增 revision 并删除操作日志。
- Native 更新失败时删除操作日志并保留旧版本；RDB 提交失败时保留操作日志，启动恢复按相同 ID 幂等重放 Native update 后再提交元数据。
- Promise 只有在新 revision 已提交后才 resolve。

#### 3.3.4 `getMemory`

只返回 `ACTIVE` 记录；不存在、已删除、`PENDING` 或 `FAILED` 均返回 `null`。该方法不访问向量索引。

#### 3.3.5 `searchMemory`

处理流程：

1. 校验并规范化查询文本。
2. 生成 512 维 L2 归一化查询向量。
3. 为过滤和 tombstone 留出候选余量，Native 请求量为 `min(1000, max(topK * 4, topK))`。
4. 调用 Native `searchVector`。
5. RDB 批量读取命中 ID，仅保留 `ACTIVE` 且满足过滤条件的记录。
6. 按 Native 距离稳定排序，最多返回 `topK` 条。

超时语义：

- `timeoutMs` 省略或为 `0` 表示不设置业务超时。
- 超时后返回 `E_TIMEOUT`；底层查询如果已经进入不可中断的 `pread`，允许在后台完成，但结果必须丢弃。
- `partial` 当前固定为 `false`。只有未来显式支持部分结果时才能设为 `true`。

#### 3.3.6 `deleteMemory`

事务语义：

1. RDB 将 `ACTIVE` 原子改为 `DELETING`。
2. Native `deleteVector(id)` 写入 tombstone WAL 并发布动态读视图。
3. RDB 删除记录或保留审计 tombstone，但不得再作为 `ACTIVE` 返回。
4. 第 2 步失败时保留 `DELETING`，启动恢复继续执行删除。

重复删除按幂等处理：用户区不存在的 ID 直接成功；预置 ID 返回 `E_RESERVED_ID`。

#### 3.3.7 `setResourceProfile`

档位仅在查询边界切换，不中断进行中的查询：

| 档位 | 缓存预算 | I/O 深度 | 每轮预取页 | 预取 |
|---|---:|---:|---:|---|
| `NORMAL` | 原始向量数据大小的 20% | 8 | 4 | 开启 |
| `CONSTRAINED` | 10% | 4 | 2 | 开启 |
| `CRITICAL` | 4% | 1 | 0 | 关闭 |

同档位重复调用直接成功。档位变更不能清除 WAL、重建索引或修改 Recall 关键参数。

#### 3.3.8 `flush`

将动态 MemTable 持久化为 SSTable，并提交 manifest。成功返回表示调用前已确认写入均可在进程重启后恢复。允许与搜索并发；同一时间只允许一个 flush/compaction，冲突时排队而不是并行执行。

#### 3.3.9 `runBenchmark`

仅供诊断页和测试构建使用。Release 版本可通过构建开关禁用，禁用时返回 `E_UNSUPPORTED`。benchmark 数据必须来自预置查询集，不得读取用户真实记忆文本。

#### 3.3.10 `resetDemoData`

仅删除 `source='USER' | 'IMPORT' | 'AGENT'` 的动态记录，保留预置 5,000 条 base 数据。执行期间服务拒绝 CRUD 和 search，并在删除后重新打开动态层。Release 版本应要求二次确认令牌。

## 4. ArkTS 内部接口

### 4.1 `LocalEmbeddingProvider`

```ts
export interface EmbeddingModelConfig {
  modelPath: string;
  tokenizerPath: string;
  expectedDimension: number;
  maxTokens: number;
  backendPreference: 'MINDSPORE_LITE' | 'NCNN';
}

export interface EmbeddingModelInfo {
  backend: 'MINDSPORE_LITE' | 'NCNN';
  modelId: string;
  version: string;
  dimension: number;
  maxTokens: number;
  quantization: 'INT8';
  pooling: 'CLS';
  normalized: true;
}

export interface LocalEmbeddingProvider {
  initialize(config: EmbeddingModelConfig): Promise<EmbeddingModelInfo>;
  embed(text: string): Promise<Float32Array>;
  shutdown(): Promise<void>;
}
```

实现要求：

- tokenizer 与模型版本必须配套，禁止仅凭维度相同混用。
- 输出必须是 512 维、有限数值、L2 norm 在 `[0.999, 1.001]` 内。
- 空文本由业务层拒绝；Native 仍需做防御性校验。
- 输入超过 256 tokens 时按模型 tokenizer 截断，并在诊断指标中累计 `embeddingTruncatedCount`。
- 模型推理最多并发 1 个任务；后续任务进入有界队列，队列满返回 `E_RESOURCE_EXHAUSTED`。

推荐将模型推理封装为独立 Native 模块 `localEmbeddingNative`，从而不把模型依赖耦合进向量索引 ABI：

```ts
export interface LocalEmbeddingNativeModule {
  openModel(config: EmbeddingModelConfig): Promise<EmbeddingModelInfo>;
  embed(text: string): Promise<Float32Array>;
  closeModel(): Promise<void>;
}
```

### 4.2 `MetadataRepository`

```ts
export interface MetadataRepository {
  open(databaseName: string): Promise<void>;
  allocateUserId(): Promise<number>;
  insertPending(record: MemoryRecord): Promise<void>;
  activate(id: number, expectedRevision: number): Promise<MemoryRecord>;
  updateMetadata(id: number, patch: MemoryPatch): Promise<MemoryRecord>;
  markDeleting(id: number): Promise<void>;
  markFailed(id: number, reason: string): Promise<void>;
  createPendingUpdate(
    id: number,
    baseRevision: number,
    patch: MemoryPatch
  ): Promise<string>;
  commitPendingUpdate(operationId: string): Promise<MemoryRecord>;
  deletePendingOperation(operationId: string): Promise<void>;
  getActive(id: number): Promise<MemoryRecord | null>;
  getActiveBatch(ids: number[]): Promise<Map<number, MemoryRecord>>;
  listRecoverableRecords(): Promise<MemoryRecord[]>;
  listPendingOperations(): Promise<PendingMemoryOperation[]>;
  finalizeDelete(id: number): Promise<void>;
  close(): Promise<void>;
}

export interface PendingMemoryOperation {
  operationId: string;
  type: 'PENDING_UPDATE';
  memoryId: number;
  baseRevision: number;
  patch: MemoryPatch;
  createdAt: number;
}
```

RDB 表建议：

```sql
CREATE TABLE memory_record (
  id INTEGER PRIMARY KEY,
  text TEXT NOT NULL,
  source TEXT NOT NULL,
  occurred_at INTEGER NOT NULL,
  tags_json TEXT NOT NULL,
  attributes_json TEXT NOT NULL,
  status TEXT NOT NULL,
  revision INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  failure_reason TEXT
);

CREATE INDEX idx_memory_status ON memory_record(status);
CREATE INDEX idx_memory_source_time
  ON memory_record(source, occurred_at DESC);

CREATE TABLE id_allocator (
  name TEXT PRIMARY KEY,
  next_id INTEGER NOT NULL
);

CREATE TABLE memory_operation (
  operation_id TEXT PRIMARY KEY,
  operation_type TEXT NOT NULL,
  memory_id INTEGER NOT NULL,
  base_revision INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE INDEX idx_memory_operation_memory
  ON memory_operation(memory_id);
```

`allocateUserId()` 必须在 RDB 事务内递增，初值为 5000；ID 永不回收，避免旧 WAL 与新记录发生身份冲突。

同一 `memory_id` 同时最多存在一个 `memory_operation`。`commitPendingUpdate()` 必须校验当前 revision 等于 `base_revision`，否则返回 `E_CONFLICT`，不能覆盖更新后的记录。

### 4.3 `NativeVectorStore`

```ts
export interface VectorStoreOpenConfig {
  baseIndexPath: string;
  pqModelPath?: string;
  dynamicDir: string;
  expectedDimension: number;
  cachePolicy: 'none' | 'lru' | '2q' | 'graph-aware-2q';
  cachePages: number;
  ioMode: 'thread_pool_pread' | 'pread';
  ioBatchSize: number;
  ioDepth: number;
  searchWidth: number;
  beamWidth: number;
  entryCount: number;
  rerankTopK: number;
  prefetchPolicy: 'none' | 'pq';
  prefetchSubmitTopK: number;
  prefetchMaxInflight: number;
}

export interface NativeSearchHit {
  id: number;
  distance: number;
}

export interface NativeSearchResponse {
  hits: NativeSearchHit[];
  stats: NativeSearchStats;
}

export interface NativeVectorStore {
  open(config: VectorStoreOpenConfig): Promise<NativeEngineInfo>;
  searchVector(vector: Float32Array, topK: number): Promise<NativeSearchResponse>;
  upsertVector(
    id: number,
    vector: Float32Array,
    mode: 'INSERT' | 'UPDATE'
  ): Promise<void>;
  deleteVector(id: number): Promise<void>;
  setResourceProfile(profile: ResourceProfile): Promise<void>;
  flush(): Promise<void>;
  getMetrics(): Promise<NativeEngineMetrics>;
  close(): Promise<void>;
}
```

## 5. N-API 模块规范

### 5.1 模块名与导出

模块名固定为 `agentAwareNative`：

```ts
declare const agentAwareNative: {
  open(config: VectorStoreOpenConfig): Promise<NativeEngineInfo>;
  searchVector(
    vector: Float32Array,
    topK: number
  ): Promise<NativeSearchResponse>;
  upsertVector(
    id: number,
    vector: Float32Array,
    mode: 'INSERT' | 'UPDATE'
  ): Promise<void>;
  deleteVector(id: number): Promise<void>;
  setResourceProfile(profile: ResourceProfile): Promise<void>;
  flush(): Promise<void>;
  getMetrics(): Promise<NativeEngineMetrics>;
  close(): Promise<void>;
};
```

为兼容早期原型，`upsert`、`search`、`erase`、`stats` 可在开发阶段保留为别名；1.0 正式接口只使用上面的完整名称，别名不得出现在业务层。

### 5.2 打开结果

```ts
export interface NativeEngineInfo {
  apiVersion: '1.0';
  engineVersion: string;
  dimension: number;
  baseVectorCount: number;
  basePageCount: number;
  dynamicSequence: number;
  cachePolicy: string;
  cachePages: number;
  requestedIoMode: string;
  effectiveIoMode: string;
  ioFallbackReason?: string;
  pqEnabled: boolean;
  pqSubspaces: number;
  indexFormatVersion: number;
}
```

`open()` 在以下情况必须失败：

- base index、PQ 文件或 dynamic 目录不可访问；
- `expectedDimension` 与索引/PQ 不一致；
- PQ magic、版本、offset、codes 数量或校验和错误；
- base index 元数据不完整；
- 已存在打开中的单例实例。

### 5.3 N-API 参数与内存规则

- 异步任务创建时立即复制 `Float32Array` 到 Native 自有 `std::vector<float>`。
- 禁止在工作线程中持有 `napi_env`、ArkTS 对象地址或 JS ArrayBuffer 裸指针。
- `Float32Array.length` 必须精确等于引擎维度；NaN/Infinity 返回 `E_INVALID_ARGUMENT`。
- `topK` 必须是整数且范围 `1..1000`；业务层公开范围仍为 `1..100`。
- ID 必须是安全整数且位于 `uint32` 范围。
- 所有 resolve/reject 必须回到 JS 线程执行。
- N-API 环境 teardown 时取消未开始任务，等待已经进入 WAL 提交阶段的写任务完成。

### 5.4 并发与排队

| 操作 | 并发策略 |
|---|---|
| `searchVector` | 允许排队；C++ 当前搜索路径存在互斥区，实际并行度以引擎能力为准 |
| `upsertVector` / `deleteVector` | 串行，保持 WAL sequence 单调 |
| `flush` | 与写入串行；可与已取得动态快照的搜索并发 |
| `setResourceProfile` | 在下一次查询开始前生效 |
| `open` / `close` | 全局独占 |

队列优先级从高到低：用户查询、用户写入、flush、预取、benchmark。查询不得长期饿死写入；连续处理 16 个查询后应允许一个已等待写任务运行。

## 6. 指标接口

### 6.1 `EngineStats`

```ts
export interface LatencySummary {
  count: number;
  p50Ms: number;
  p95Ms: number;
  p99Ms: number;
  maxMs: number;
}

export interface EngineStats {
  collectedAt: number;
  state: ServiceState;
  profile: ResourceProfile;
  dimension: number;
  baseVectorCount: number;
  dynamicVectorCount: number;
  dynamicSequence: number;
  searchLatency: LatencySummary;
  embeddingLatency: LatencySummary;
  native: NativeEngineMetrics;
  pendingMetadataCount: number;
  deletingMetadataCount: number;
  embeddingTruncatedCount: number;
}
```

### 6.2 `NativeEngineMetrics`

```ts
export interface NativeSearchStats {
  usedGraphPath: boolean;
  dynamicReadSequence: number;
  nodeReads: number;
  expanded: number;
  visited: number;
  pageRequests: number;
  cacheHits: number;
  demandReads: number;
  prefetchReads: number;
  prefetchUsefulPages: number;
  prefetchWastedPages: number;
  prefetchReadyHits: number;
  prefetchPendingHits: number;
  ioSubmits: number;
  ioCompletions: number;
  demandReadWaitUs: number;
  dynamicSnapshotMs: number;
  baseSearchMs: number;
  deltaSearchMs: number;
  mergeMs: number;
}

export interface NativeEngineMetrics {
  requestedIoMode: string;
  effectiveIoMode: string;
  ioFallbackReason?: string;
  ioDepth: number;
  ioBatchSize: number;
  cachePolicy: string;
  cacheCapacityPages: number;
  cacheResidentPages: number;
  memoryBudgetBytes: number;
  modelResidentBytes: number;
  indexResidentBytes: number;
  totalSearches: number;
  totalWrites: number;
  totalDeletes: number;
  totalFlushes: number;
  totalIoErrors: number;
  aggregate: NativeSearchStats;
}
```

比例由 ArkTS 展示层基于原始计数计算，避免 Native 重复定义分母：

- `cacheHitRate = cacheHits / pageRequests`
- `prefetchUsefulRate = prefetchUsefulPages / prefetchReads`
- `readsPerQuery = demandReads / totalSearches`

分母为 0 时显示 `0`，不得产生 NaN。

## 7. Benchmark 接口

```ts
export interface BenchmarkConfig {
  queryAssetPath: string;
  groundTruthAssetPath?: string;
  queryLimit?: number;
  warmupCount?: number;
  rounds?: number;
  topK?: number;
  concurrency?: 1 | 4 | 8;
  coldCache?: boolean;
  outputDir: string;
}

export interface BenchmarkEnvironment {
  deviceModel: string;
  osVersion: string;
  sdkVersion: string;
  appVersion: string;
  batteryPercent: number;
  thermalState: string;
  freeStorageBytes: number;
  profile: ResourceProfile;
}

export interface BenchmarkReport {
  reportVersion: '1.0';
  runId: string;
  startedAt: number;
  environment: BenchmarkEnvironment;
  config: BenchmarkConfig;
  queryCount: number;
  recallAt10?: number;
  top3HitRate?: number;
  qps: number;
  latency: LatencySummary;
  readsPerQuery: number;
  cacheHitRate: number;
  prefetchUsefulRate: number;
  peakRssBytes: number;
  energyMah?: number;
  rawResultPath: string;
  warnings: string[];
}
```

测试约束：

- 默认 `warmupCount=20`、`rounds=5`、`queryLimit=200`、`topK=10`。
- 报告保存每轮原始数据并计算中位数，不得只保留最好一轮。
- HarmonyOS 真机指标必须填写真实设备环境；Linux/WSL 历史结果只能作为独立基线，不得写入真机字段。

## 8. C++ 引擎接口映射

### 8.1 仓库已有接口

仓库已有公开 C++ 接口：

```cpp
class StorageEngine {
 public:
  virtual ~StorageEngine() = default;
  virtual EngineSearchResult search_one(const float* query,
                                         std::size_t top_k) = 0;
  virtual void insert(std::uint32_t id, const float* vector) = 0;
  virtual void update(std::uint32_t id, const float* vector) = 0;
  virtual void erase(std::uint32_t id) = 0;
};
```

N-API 映射：

| N-API | C++ 已有调用 | 备注 |
|---|---|---|
| `searchVector` | `PackedGraphEngine::search_one` | 返回 base/delta merge 后 Top-K 和查询统计 |
| `upsertVector(..., 'INSERT')` | `PackedGraphEngine::insert` | 依赖 `DynamicWriteManager` |
| `upsertVector(..., 'UPDATE')` | `PackedGraphEngine::update` | 依赖 `DynamicWriteManager` |
| `deleteVector` | `PackedGraphEngine::erase` | 写入动态 tombstone |
| `flush` | `DynamicWriteManager::flush` | 当前不在 `StorageEngine` 抽象中，N-API 持有 manager 调用 |
| `getMetrics` | `EngineSearchStats`、`DiskGraphSearchStats`、`io_status()` 聚合 | 需新增累计器 |
| `setResourceProfile` | cache/I/O/prefetch 配置 | 需新增查询边界重配置能力 |
| `close` | `DynamicWriteManager::close` + 释放 engine | 需 N-API 生命周期封装 |

现有 `PackedGraphEngineConfig` 中的 Linux `io_uring` 和 `odirect` 不作为鸿蒙接口公开值。鸿蒙默认使用待实现的 `thread_pool_pread`；若不可用，明确回退到同步 `pread` 并填写 `ioFallbackReason`。

### 8.2 待新增的稳定 C++ facade

为了避免 N-API 直接依赖多个具体类，鸿蒙适配建议新增：

```cpp
enum class ErrorCode : std::uint32_t {
  kOk = 0,
  kNotInitialized,
  kBusy,
  kInvalidArgument,
  kDimensionMismatch,
  kIndexIncompatible,
  kIndexCorrupted,
  kModelFailure,
  kIoError,
  kWalError,
  kCancelled,
  kInternal,
};

enum class WriteMode : std::uint8_t {
  kInsert = 0,
  kUpdate = 1,
};

enum class ResourceProfile : std::uint8_t {
  kNormal = 0,
  kConstrained = 1,
  kCritical = 2,
};

struct HarmonyEngineConfig {
  std::string base_index_path;
  std::string pq_model_path;
  std::string dynamic_dir;
  std::uint32_t expected_dimension = 512;
  std::string cache_policy = "graph-aware-2q";
  std::size_t cache_pages = 0;
  std::string io_mode = "thread_pool_pread";
  std::size_t io_batch_size = 1;
  std::size_t io_depth = 8;
  std::size_t search_width = 64;
  std::size_t beam_width = 0;
  std::size_t entry_count = 32;
  std::size_t rerank_top_k = 0;
  std::string prefetch_policy = "pq";
  std::size_t prefetch_submit_top_k = 4;
  std::size_t prefetch_max_inflight = 8;
};

struct Status {
  ErrorCode code = ErrorCode::kOk;
  std::string message;
  bool retryable = false;

  bool ok() const { return code == ErrorCode::kOk; }
};

template <typename T>
struct StatusOr {
  Status status;
  T value;
};

struct EngineMetricsSnapshot {
  DiskGraphIoStatus io;
  DiskGraphSearchStats aggregate_search;
  std::uint64_t dynamic_sequence = 0;
  std::uint64_t total_searches = 0;
  std::uint64_t total_writes = 0;
  std::uint64_t total_deletes = 0;
  std::uint64_t total_flushes = 0;
  std::size_t cache_capacity_pages = 0;
  std::size_t cache_resident_pages = 0;
  std::size_t memory_budget_bytes = 0;
};

class HarmonyEngineFacade {
 public:
  Status open(const HarmonyEngineConfig& config);
  StatusOr<EngineSearchResult> search(const float* query,
                                      std::uint32_t dim,
                                      std::size_t top_k);
  Status upsert(std::uint32_t id, const float* vector,
                std::uint32_t dim, WriteMode mode);
  Status erase(std::uint32_t id);
  Status set_resource_profile(ResourceProfile profile);
  Status flush();
  StatusOr<EngineMetricsSnapshot> metrics() const;
  Status close();
};
```

facade 必须在调用现有 `StorageEngine` 前校验维度，因为当前 `insert/update/search_one` 的指针接口本身不携带维度。

## 9. Platform I/O 接口

### 9.1 待实现 `PageIoBackend`

```cpp
using IoToken = std::uint64_t;

enum class IoPriority : std::uint8_t {
  kDemandRead = 0,
  kWrite = 1,
  kPrefetch = 2,
};

struct PageReadRequest {
  std::uint32_t page_id = 0;
  std::uint64_t offset = 0;
  std::size_t bytes = 0;
  IoPriority priority = IoPriority::kDemandRead;
};

struct PageReadResult {
  IoToken token = 0;
  std::uint32_t page_id = 0;
  Status status;
  std::vector<char> data;
};

struct PageIoStatus {
  std::string requested_mode;
  std::string effective_mode;
  std::string fallback_reason;
  std::size_t queue_capacity = 0;
  std::size_t queue_depth = 0;
  std::size_t pending = 0;
};

class PageIoBackend {
 public:
  virtual ~PageIoBackend() = default;
  virtual Status open(const std::string& path,
                      std::size_t page_size,
                      std::size_t queue_depth) = 0;
  virtual StatusOr<std::vector<char>> read(
      std::uint64_t offset, std::size_t bytes) = 0;
  virtual StatusOr<std::vector<IoToken>> submit(
      const std::vector<PageReadRequest>& requests) = 0;
  virtual StatusOr<PageReadResult> wait(IoToken token,
                                        std::uint32_t timeout_ms) = 0;
  virtual Status cancel(IoToken token) = 0;
  virtual Status flush() = 0;
  virtual PageIoStatus status() const = 0;
  virtual Status close() = 0;
};
```

HarmonyOS 实现要求：

- 使用有界线程池和 `pread`，默认 queue depth 为 8。
- demand read 优先于 prefetch；队列满时优先丢弃未开始的 prefetch。
- 同一页的并发请求应合并，完成后唤醒所有等待者。
- `cancel()` 只保证取消尚未开始的任务；正在执行的 `pread` 可正常完成并丢弃结果。
- 不依赖 `io_uring`、`O_DIRECT` 或系统权限修改。
- `read()` 和 `wait()` 的短读、EOF、EINTR、文件关闭必须转换为明确 `Status`。

## 10. 索引与 PQ 文件兼容合同

PQ 持久化头至少包含：

```cpp
struct PqFileHeader {
  char magic[8];              // {'M','E','M','H','P','Q','0','1'}
  std::uint32_t format_version;
  std::uint32_t header_bytes;
  std::uint32_t dimension;
  std::uint32_t subspaces;
  std::uint32_t centroids;
  std::uint64_t vector_count;
  std::uint64_t codebook_bytes;
  std::uint64_t codes_bytes;
  std::uint64_t payload_checksum;
};
```

打开时必须满足：

- `dimension == baseIndex.dimension == embeddingModel.dimension`；
- `vector_count == baseIndex.vectorCount`；
- `codes_bytes == vector_count * subspaces`；
- payload 实际长度与 header 声明一致；
- checksum 校验通过；
- 主版本不兼容时返回 `E_INDEX_INCOMPATIBLE`，内容损坏时返回 `E_INDEX_CORRUPTED` 或 `E_CHECKSUM_MISMATCH`。

不同维度、不同模型版本或不同 tokenizer 生成的数据必须使用独立索引目录，不允许混合写入。

## 11. 恢复状态机

### 11.1 新增恢复

```text
RDB PENDING
  |-- Native 已有该 ID 的最新非删除记录 --> RDB ACTIVE
  |-- Native 不存在该 ID ----------------> 重做 embed + upsert
  `-- 重做失败 --------------------------> RDB FAILED
```

### 11.2 删除恢复

```text
RDB DELETING
  |-- Native 最新记录为 tombstone --> finalizeDelete
  `-- Native 仍存在有效向量 ------> 重做 deleteVector -> finalizeDelete
```

### 11.3 更新恢复

```text
RDB memory_operation=PENDING_UPDATE
  |-- 当前 revision != base_revision --> 标记冲突并停止自动覆盖
  `-- 当前 revision == base_revision
        |-- 重建目标文本向量
        |-- 幂等调用 Native update
        `-- commitPendingUpdate
```

Native update 即使在崩溃前已经成功也允许按同一 ID 重放；动态层以更高 sequence 的相同向量覆盖旧版本，不影响可见结果。恢复完成前，原 `memory_record` 仍保持旧的 `ACTIVE` revision。

### 11.4 冲突规则

- Native 动态层 sequence 是向量可见性的最终顺序依据。
- RDB `revision` 是业务元数据版本依据。
- 搜索只返回 Native 命中且 RDB 状态为 `ACTIVE` 的交集。
- 出现“RDB ACTIVE、Native 无向量”时，初始化返回 warning 并将记录转为 `PENDING` 重建；不得静默永久丢失。
- 出现“Native 有用户 ID、RDB 无记录”时视为孤儿向量，搜索过滤掉；后台维护可写 tombstone 清理。

## 12. 调用示例

```ts
const service = new DefaultAgentMemoryService();

await service.initialize({
  baseIndexPath: `${filesDir}/index/base.idx`,
  pqModelPath: `${filesDir}/index/base.pq`,
  embeddingModelPath: `${filesDir}/model/bge-small-zh-v1.5-int8.ms`,
  tokenizerPath: `${filesDir}/model/tokenizer.json`,
  dynamicDir: `${filesDir}/dynamic`,
  expectedDimension: 512,
  initialProfile: 'NORMAL',
  enableRecovery: true,
  validateChecksum: true,
});

const record = await service.putMemory({
  text: '图书馆周五晚上十点闭馆。',
  source: 'USER',
  tags: ['图书馆', '开放时间'],
});

const result = await service.searchMemory({
  text: '周五图书馆几点关门？',
  topK: 3,
  filter: { tagsAny: ['图书馆'] },
  timeoutMs: 800,
});

await service.updateMemory(record.id, {
  text: '图书馆周五晚上十点半闭馆。',
});

await service.deleteMemory(record.id);
await service.flush();
await service.shutdown();
```

错误处理示例：

```ts
try {
  await service.searchMemory({ text: '', topK: 10 });
} catch (error) {
  const e = error as MemHarmonyError;
  if (e.code === 'E_EMPTY_TEXT') {
    // UI 显示本地参数提示，不触发自动重试。
  }
}
```

## 13. 接口验收清单

实现完成后至少通过以下合约测试：

1. 所有耗时接口返回 Promise，ArkUI 线程无同步模型或磁盘访问。
2. 初始化成功、重复初始化、初始化失败清理和重复关闭行为符合状态机。
3. 空文本、超长文本、非法 ID、重复 ID、非法 topK 和错误维度返回规定错误码。
4. N-API 异步调用不持有失效的 `Float32Array` 内存。
5. 新增、纯元数据更新、文本更新、删除和重复删除符合事务语义。
6. 杀进程后可恢复 `PENDING`、`DELETING`、WAL 和 SSTable。
7. 搜索不返回 tombstone、孤儿向量或非 `ACTIVE` 元数据。
8. `CRITICAL` 档位关闭预取，I/O depth 为 1，仍可完成查询。
9. 模型、PQ、base index 维度或版本不匹配时初始化失败。
10. 连续 500 次查询、100 次混合 CRUD 和 10 次强制重启无 Native 崩溃、无已确认记录丢失。

## 14. 版本管理

- ArkTS 公开接口、N-API ABI、索引格式和 benchmark report 分别独立版本化。
- 增加可选字段属于兼容变更；删除字段、改变语义或改变默认排序属于破坏性变更。
- N-API `apiVersion` 采用 `major.minor`。major 不匹配时 ArkTS 必须拒绝启动。
- 索引/PQ 文件采用整数 `format_version`；读取器只支持明确列出的版本，不做猜测性解析。
- 1.0 冻结后，任何接口变更必须同时更新本文、ArkTS `.d.ts` 声明和合约测试。
