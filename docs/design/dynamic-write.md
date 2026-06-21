# P5 动态写入计划文档

## 当前实现状态（2026-06-21）

P5 已完成最小可交付闭环：WAL、MemTable、SSTable、manifest、manual/background compaction、recovery、base/delta Top-K merge、动态 Recall 抽样和真实并发 mixed RW benchmark 均已落地。本轮进一步把 `DynamicWriteManager` 读侧改为 immutable read view 发布，避免 reader 查询 base 覆盖记录时和 writer/flush/compaction 抢同一把全局 mutex。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| MemTable/WAL/SSTable | 已完成 | `include/agent_aware/dynamic`、`src/agent_aware/dynamic` |
| Manifest/recovery | 已完成 | `Manifest` + manager `open()` 重放 |
| Compaction | 已完成可运行版 | `CompactionJob`、`DynamicWriteManager::compact_once` |
| mixed RW benchmark | 已完成 | `tools/benchmarks/mixed_rw_benchmark.cpp`，支持 JSON 和多线程 |
| immutable read view | 已完成 | `DynamicWriteManager::DynamicReadView` + atomic `shared_ptr` |
| 真正异步 flush queue | 未完成 | 当前仍沿用 manager flush 路径 |
| 周期性 rebuild packed graph | 未完成 | 下一阶段将 delta 批量吸收到 base |

## 1. 阶段定位

| 阶段 | 时间 | 核心目标 | 验收方式 |
|---|---|---|---|
| P5 动态写入 | 第 5–6 周 | 完成 MemTable、WAL、SSTable、Compaction 和增量插入 | 混合读写 benchmark |

本阶段目标是把系统从“静态构建 / 静态查询”推进到“支持在线增量写入和混合读写”的状态。  
完成后，系统应能够在已有索引或数据文件基础上继续插入新数据，并保证写入数据可恢复、可查询、可合并、可压缩。

---

## 2. 总体目标

P5 阶段需要实现一套类 LSM-Tree 的动态写入路径，核心包括：

1. **MemTable**：承接在线写入，提供内存态增量数据管理。
2. **WAL**：保证写入的崩溃恢复能力。
3. **SSTable**：将 MemTable 中的数据刷盘，形成不可变磁盘文件。
4. **Compaction**：合并多个 SSTable，减少读放大和空间放大。
5. **增量插入**：将新向量 / 新节点动态加入系统，并可被查询路径访问。
6. **混合读写 Benchmark**：验证写入、读取、恢复、Compaction 对延迟和吞吐的影响。

---

## 3. 模块设计

### 3.1 MemTable

#### 目标

MemTable 负责接收最新写入的数据，作为写入路径的第一层缓冲。

#### 建议职责

- 维护增量插入的向量、节点元信息和邻接关系。
- 支持按 id 查询最新写入数据。
- 支持达到阈值后触发 flush。
- 支持与主索引查询结果合并。
- 支持简单的删除标记或覆盖更新标记，如果暂不做删除，可预留 tombstone 字段。

#### 建议接口

```cpp
class MemTable {
public:
    bool insert(NodeId id, const Vector& vec, const NodeMeta& meta);
    bool get(NodeId id, Vector& vec, NodeMeta& meta) const;
    bool contains(NodeId id) const;
    size_t size() const;
    size_t bytes() const;
    bool should_flush() const;
    std::vector<Record> snapshot() const;
    void clear();
};
```

#### 验收标准

- 插入后可立即查询到。
- 达到 flush 阈值后可生成快照。
- MemTable 清空后数据不丢失，应已写入 WAL 或 SSTable。
- 查询路径可以同时读取主索引和 MemTable。

---

### 3.2 WAL

#### 目标

WAL 用于保证写入操作在系统崩溃后可以恢复。

#### 写入流程

```text
insert request
    ↓
append WAL
    ↓
fsync / group commit
    ↓
insert MemTable
    ↓
return success
```

#### 建议记录格式

```text
magic | version | op_type | node_id | vector_dim | payload_size | checksum | payload
```

其中：

- `magic`：用于识别 WAL 文件格式。
- `version`：支持后续格式升级。
- `op_type`：insert / delete / update。
- `node_id`：节点编号。
- `vector_dim`：向量维度。
- `payload_size`：记录长度。
- `checksum`：用于检测 WAL 尾部损坏。
- `payload`：向量数据、邻居信息、元信息等。

#### 恢复逻辑

启动时扫描 WAL：

1. 从头读取 WAL record。
2. 校验 magic、size、checksum。
3. 遇到损坏尾部时停止读取。
4. 将有效记录 replay 到 MemTable。
5. 如果存在已经 flush 的 checkpoint，则只 replay checkpoint 之后的 WAL。

#### 验收标准

- 写入后进程崩溃，重启后数据仍可恢复。
- WAL 尾部半条记录损坏时不会导致系统启动失败。
- WAL replay 后查询结果和崩溃前一致。
- 支持 WAL truncate 或 rotate，避免文件无限增长。

---

### 3.3 SSTable

#### 目标

SSTable 是 MemTable flush 后的磁盘不可变数据文件，用于保存增量写入的数据。

#### 文件组成

建议拆分为：

```text
sst_xxx.data   // record 数据区
sst_xxx.index  // node_id -> offset 索引
sst_xxx.meta   // 文件元信息
```

也可以合并为单文件格式：

```text
header | data blocks | index block | footer
```

#### 建议元信息

- SSTable id
- level
- record count
- min node id
- max node id
- data offset
- index offset
- checksum
- created timestamp

#### 查询方式

查询时按以下优先级读取：

```text
MemTable
  ↓ miss
Level-0 SSTable，新文件优先
  ↓ miss
更高 Level SSTable
  ↓ miss
主索引 / base graph
```

#### 验收标准

- MemTable flush 后生成合法 SSTable。
- SSTable 文件可重新加载。
- 根据 node_id 能够快速定位 record。
- 多个 SSTable 同时存在时，新版本优先。
- SSTable 读路径不会破坏原有静态索引搜索逻辑。

---

### 3.4 Compaction

#### 目标

Compaction 用于将多个 SSTable 合并，减少读放大、空间放大和过期数据。

#### 初始策略

P5 阶段建议先实现简单版本：

```text
Level-0 SSTable 数量超过阈值
    ↓
合并多个 Level-0 文件
    ↓
去重，保留最新版本
    ↓
生成新的 Level-1 SSTable
    ↓
删除旧文件
```

#### 可选优化

- 后台线程执行 Compaction。
- Compaction 限速，避免影响前台查询。
- 支持按照文件大小触发。
- 支持按照读放大触发。
- 支持 tombstone 清理。

#### 验收标准

- 多个 SSTable 能够合并为更少文件。
- 相同 node_id 保留最新版本。
- Compaction 后查询结果不变。
- Compaction 过程中前台查询不崩溃。
- Compaction 后旧文件可安全删除。

---

### 3.5 增量插入

#### 目标

系统需要支持新增向量 / 新节点在线插入，并能被后续查询访问。

#### 最小实现路径

P5 阶段可以先采用“增量层 + 查询合并”的方式，而不是立即把新节点完全并入主图：

```text
Base Disk Graph
    +
Delta MemTable / Delta SSTable
    ↓
Search Result Merge
```

#### 查询合并策略

查询时：

1. 在原有 Disk Graph 中搜索 top-k。
2. 在 MemTable / SSTable 的增量数据中搜索 top-k。
3. 合并两个候选集合。
4. 按全精度距离 rerank。
5. 返回最终 top-k。

#### 后续可演进方向

- 将增量节点周期性 merge 到主图。
- 对新节点构建临时 Delta Graph。
- 使用 HNSW / Vamana 小图作为增量层。
- Compaction 后触发 graph rebuild 或 partial graph merge。

#### 验收标准

- 插入新向量后，不重建主图也能查询到。
- 混合查询结果包含 base graph 和 delta layer。
- 插入数量增长时，系统仍能稳定运行。
- 增量层查询开销可被 benchmark 量化。

---

## 4. 两周实施安排

## 第 5 周：写入路径打通

### Day 1：设计与接口拆分

- 明确动态写入数据结构。
- 新增 MemTable / WAL / SSTable 相关头文件。
- 确定 Record 格式。
- 确定 flush 阈值和配置项。
- 保留对现有静态搜索逻辑的兼容。

交付物：

- `memtable.h / memtable.cpp`
- `wal.h / wal.cpp`
- `sstable.h / sstable.cpp`
- 配置项：`enable_dynamic_write`、`memtable_flush_bytes`、`wal_path`、`sstable_dir`

---

### Day 2：实现 MemTable

- 实现 insert / get / snapshot / clear。
- 支持线程安全，至少保证单写多读不崩溃。
- 添加单元测试。
- 接入查询路径，使新写入数据可以被简单扫描查询。

交付物：

- MemTable 基础实现。
- MemTable 单测。
- 插入后立即可查的最小 demo。

---

### Day 3：实现 WAL

- 实现 WAL append。
- 实现 record checksum。
- 实现 WAL replay。
- 实现崩溃恢复测试。
- 支持 WAL truncate / rotate 的最小版本。

交付物：

- WAL 写入与恢复。
- WAL replay 单测。
- 崩溃恢复测试脚本。

---

### Day 4：实现 SSTable Flush

- MemTable 达到阈值后生成 SSTable。
- SSTable 支持 index 加载。
- 查询路径支持从 SSTable 读取增量数据。
- flush 成功后清理 MemTable 和 WAL checkpoint。

交付物：

- SSTable 写入。
- SSTable 读取。
- MemTable -> SSTable flush 流程。
- 基础查询合并。

---

### Day 5：联调写入主路径

- 串联 insert -> WAL -> MemTable -> Flush -> SSTable。
- 检查异常路径。
- 检查重复 id、覆盖写入、文件损坏。
- 编写端到端测试。

交付物：

- 动态写入主链路可跑通。
- 插入、重启、查询、flush 全流程可验证。
- 初步性能数据。

---

## 第 6 周：Compaction 与 Benchmark

### Day 6：实现 Level-0 Compaction

- 多个 L0 SSTable 合并。
- 相同 node_id 保留最新版本。
- 生成 L1 SSTable。
- 原子替换元数据。

交付物：

- L0 -> L1 Compaction。
- Compaction 单测。
- Compaction 后查询一致性测试。

---

### Day 7：后台 Compaction 与并发安全

- 支持后台 Compaction 线程。
- 前台查询使用快照视图。
- Compaction 完成后原子切换文件列表。
- 删除旧文件前确认无读者引用。

交付物：

- 后台 Compaction。
- 查询 / 写入 / Compaction 并发测试。
- 文件生命周期管理。

---

### Day 8：增量插入查询优化

- 优化 MemTable / SSTable 增量层扫描。
- 为增量层建立简单 id index 或小型 vector index。
- 查询结果与 base graph 结果合并 rerank。
- 统计 delta layer 带来的延迟开销。

交付物：

- 增量层查询合并。
- top-k rerank 正确性测试。
- delta layer 查询延迟统计。

---

### Day 9：混合读写 Benchmark

设计 benchmark 场景：

| 场景 | 读比例 | 写比例 | 目标 |
|---|---:|---:|---|
| Read-heavy | 95% | 5% | 验证动态写入对查询延迟影响 |
| Balanced | 70% | 30% | 验证混合负载稳定性 |
| Write-heavy | 50% | 50% | 验证 WAL / MemTable / Flush 压力 |
| Recovery | - | - | 验证崩溃恢复正确性 |
| Compaction | - | - | 验证 Compaction 前后性能变化 |

指标：

- Recall@10
- QPS
- Average latency
- P50 / P95 / P99 latency
- Insert throughput
- WAL append latency
- Flush latency
- Compaction duration
- Read amplification
- Write amplification
- Disk usage
- Memory usage

交付物：

- `mixed_rw_benchmark.cpp`
- benchmark 配置文件
- benchmark 输出日志
- 性能对比表

---

### Day 10：收尾与文档

- 整理模块结构。
- 清理调试代码。
- 补充 README。
- 输出 benchmark 报告。
- 标注未完成项和后续优化方向。

交付物：

- P5 阶段总结文档。
- 混合读写 benchmark 报告。
- 动态写入模块说明。
- 后续 P6 优化建议。

---

## 5. 推荐目录结构

```text
include/agentmem/dynamic/
  memtable.h
  wal.h
  sstable.h
  compaction.h
  dynamic_write_manager.h

src/agent_aware/dynamic/
  memtable.cpp
  wal.cpp
  sstable.cpp
  compaction.cpp
  dynamic_write_manager.cpp

tools/benchmarks/
  mixed_rw_benchmark.cpp

tests/unit/dynamic/
  test_memtable.cpp
  test_wal.cpp
  test_sstable.cpp
  test_compaction.cpp
  test_dynamic_insert.cpp
```

---

## 6. 核心流程

### 6.1 插入流程

```text
User insert(vector, id)
    ↓
DynamicWriteManager::insert()
    ↓
WAL::append(record)
    ↓
MemTable::insert(record)
    ↓
if MemTable should flush:
        Flush MemTable to SSTable
    ↓
return success
```

---

### 6.2 查询流程

```text
User search(query)
    ↓
Base Disk Graph Search
    ↓
Delta Layer Search
        ├── MemTable Search
        └── SSTable Search
    ↓
Merge Candidates
    ↓
Full Precision Rerank
    ↓
Return TopK
```

---

### 6.3 恢复流程

```text
System start
    ↓
Load SSTable metadata
    ↓
Replay WAL
    ↓
Rebuild MemTable
    ↓
Ready for read / write
```

---

### 6.4 Compaction 流程

```text
L0 SSTable count exceeds threshold
    ↓
Pick compaction candidates
    ↓
Merge records
    ↓
Drop old versions / tombstones
    ↓
Write new L1 SSTable
    ↓
Atomically update manifest
    ↓
Delete old SSTables
```

---

## 7. Manifest 设计建议

动态写入系统最好增加 manifest 文件记录当前有效 SSTable 列表。

### Manifest 示例

```json
{
  "version": 1,
  "next_sstable_id": 12,
  "levels": {
    "0": [
      "sst_000009",
      "sst_000010",
      "sst_000011"
    ],
    "1": [
      "sst_000005"
    ]
  },
  "wal_checkpoint": 102400
}
```

### 作用

- 系统启动时知道哪些 SSTable 有效。
- Compaction 后可以原子切换文件集合。
- WAL replay 可以从 checkpoint 之后开始。
- 避免旧文件误读或新文件丢失。

---

## 8. 测试计划

### 8.1 单元测试

| 测试项 | 内容 |
|---|---|
| MemTable 插入测试 | 插入后可 get |
| MemTable 快照测试 | snapshot 不受后续写入影响 |
| WAL 写入测试 | append 后文件大小正确 |
| WAL 恢复测试 | replay 后数据一致 |
| WAL 损坏测试 | 半条 record 不导致启动失败 |
| SSTable 写入测试 | flush 后文件存在 |
| SSTable 读取测试 | 根据 node_id 可读取 |
| Compaction 测试 | 多文件合并后结果一致 |
| 增量查询测试 | 新插入数据可被 search 命中 |

---

### 8.2 端到端测试

```text
1. 启动系统
2. 插入 10000 条向量
3. 查询部分新插入向量
4. 触发 flush
5. 重启系统
6. replay WAL / load SSTable
7. 再次查询
8. 插入更多数据
9. 触发 compaction
10. 验证查询结果不变
```

---

### 8.3 Benchmark 测试

| Benchmark | 说明 |
|---|---|
| 纯读 baseline | 原静态索引查询性能 |
| 读 95% / 写 5% | 低写入压力 |
| 读 70% / 写 30% | 常规混合负载 |
| 读 50% / 写 50% | 高写入压力 |
| Flush 压力测试 | 高频 MemTable flush |
| Compaction 压力测试 | 多 SSTable 合并 |
| Recovery 测试 | 大 WAL replay 时间 |

---

## 9. 风险与规避

| 风险 | 影响 | 规避方案 |
|---|---|---|
| WAL fsync 过慢 | 写入延迟高 | 支持 group commit 或可配置 fsync 策略 |
| SSTable 数量过多 | 查询读放大严重 | 设置 L0 阈值并触发 Compaction |
| Compaction 阻塞查询 | P99 延迟升高 | 后台执行，查询使用快照 |
| 增量层扫描过慢 | 查询延迟升高 | 为增量层增加小型索引 |
| 崩溃恢复慢 | 启动时间过长 | WAL checkpoint + truncate |
| 文件状态不一致 | 数据丢失或重复读取 | 使用 manifest 原子更新 |
| 与原有搜索逻辑耦合过重 | 后续难维护 | 使用 DynamicWriteManager 隔离写入层 |

---

## 10. 阶段验收标准

P5 完成时，至少应满足以下条件：

- [ ] 支持在线 insert，不需要重建主索引。
- [ ] 写入先落 WAL，再进入 MemTable。
- [ ] 重启后可以通过 WAL 恢复未 flush 数据。
- [ ] MemTable 达到阈值后可以 flush 成 SSTable。
- [ ] SSTable 可被查询路径读取。
- [ ] 多个 SSTable 可以进行基础 Compaction。
- [ ] 查询结果可以合并 base graph 与 delta layer。
- [ ] 完成混合读写 benchmark。
- [ ] 输出 Recall、QPS、P95、P99、写入吞吐、恢复时间等指标。
- [ ] 文档说明动态写入架构、文件格式、恢复流程和已知限制。

---

## 11. 最小可交付版本

如果时间紧，P5 最小可交付版本可以压缩为：

1. WAL append + replay。
2. MemTable insert + search。
3. MemTable flush 到 SSTable。
4. 查询时合并 base graph 和 delta records。
5. 手动触发 Compaction。
6. 混合读写 benchmark 能跑出结果。

暂时可以不做：

- 完整删除语义。
- 多 Level Compaction。
- 复杂增量图构建。
- 高并发无锁优化。
- 自动 merge 到主 Vamana 图。

---

## 12. 后续 P6 可衔接方向

P5 完成后，P6 可以继续优化：

- 增量层向量索引，避免线性扫描。
- Delta Graph / HNSW 小图。
- 主图周期性 rebuild。
- Compaction 与 graph merge 联动。
- WAL group commit。
- 后台 flush。
- O_DIRECT / io_uring 读写优化。
- 混合负载下 P99 延迟优化。
- crash consistency 更严格验证。
