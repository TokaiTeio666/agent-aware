# 05 动态写入设计文档

## 当前实现状态（2026-06-21）

动态写入已经完成最小可交付闭环：WAL、MemTable、SSTable、manifest、manual/background compaction、recovery、base/delta Top-K merge、动态 Recall 抽样和真实并发 mixed RW benchmark 均已落地。本轮进一步把 `DynamicWriteManager` 读侧改为 immutable read view 发布，避免 reader 查询 base 覆盖记录时和 writer/flush/compaction 抢同一把全局 mutex。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| MemTable/WAL/SSTable | 已完成 | `include/dynamic`、`src/dynamic` |
| Manifest/recovery | 已完成 | `Manifest` + manager `open()` 重放 |
| Compaction | 已完成可运行版 | `CompactionJob`、`DynamicWriteManager::compact_once` |
| mixed RW benchmark | 已完成 | `src/mixed_rw_benchmark.cpp`，支持 JSON 和多线程 |
| immutable read view | 已完成 | `DynamicWriteManager::DynamicReadView` + atomic `shared_ptr` |
| 真正异步 flush queue | 未完成 | 当前仍沿用 manager flush 路径 |
| 周期性 rebuild packed graph | 未完成 | 后续优化将 delta 批量吸收到 base |

## 设计背景

本设计主题目标是把系统从“静态构建 / 静态查询”推进到“支持在线增量写入和混合读写”的状态。完成后，系统应能够在已有索引或数据文件基础上继续插入新数据，并保证写入数据可恢复、可查询、可合并、可压缩。

该阶段对应第 5-6 周的核心任务：完成 MemTable、WAL、SSTable、Compaction 和增量插入，并通过混合读写 benchmark 验证读写路径。

## 核心目标

动态写入路径需要实现一套类 LSM-Tree 的机制，核心包括：

1. **MemTable**：承接在线写入，提供内存态增量数据管理。
2. **WAL**：保证写入的崩溃恢复能力。
3. **SSTable**：将 MemTable 中的数据刷盘，形成不可变磁盘文件。
4. **Compaction**：合并多个 SSTable，减少读放大和空间放大。
5. **增量插入**：将新向量 / 新节点动态加入系统，并可被查询路径访问。
6. **混合读写 Benchmark**：验证写入、读取、恢复、Compaction 对延迟和吞吐的影响。

## 相关实现文件

相关源码入口如下：

```text
include/dynamic/memtable.h
include/dynamic/wal.h
include/dynamic/sstable.h
include/dynamic/compaction.h
include/dynamic/dynamic_write_manager.h
src/dynamic/memtable.cpp
src/dynamic/wal.cpp
src/dynamic/sstable.cpp
src/dynamic/compaction.cpp
src/dynamic/dynamic_write_manager.cpp
src/mixed_rw_benchmark.cpp
```

动态写入模块应尽量隔离在 `DynamicWriteManager` 之后，避免把写入层细节扩散到原有静态搜索逻辑中。

## 具体设计要求

## 一、整体写入与查询路径

动态写入系统采用“base graph + delta layer”的结构：

```text
Base Disk Graph
    +
Delta MemTable / Delta SSTable
    ↓
Search Result Merge
```

插入流程：

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

查询流程：

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

恢复流程：

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

## 二、MemTable 设计

MemTable 负责接收最新写入的数据，作为写入路径的第一层缓冲。

建议职责：

1. 维护增量插入的向量、节点元信息和邻接关系。
2. 支持按 id 查询最新写入数据。
3. 支持达到阈值后触发 flush。
4. 支持与主索引查询结果合并。
5. 支持简单的删除标记或覆盖更新标记，如果暂不做删除，可预留 tombstone 字段。

建议接口：

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

MemTable 必须保证：

1. 插入后可立即查询到。
2. 达到 flush 阈值后可生成快照。
3. MemTable 清空后数据不丢失，应已写入 WAL 或 SSTable。
4. 查询路径可以同时读取主索引和 MemTable。

## 三、WAL 设计

WAL 用于保证写入操作在系统崩溃后可以恢复。

写入流程：

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

建议记录格式：

```text
magic | version | op_type | node_id | vector_dim | payload_size | checksum | payload
```

其中：

1. `magic`：用于识别 WAL 文件格式。
2. `version`：支持后续格式升级。
3. `op_type`：insert / delete / update。
4. `node_id`：节点编号。
5. `vector_dim`：向量维度。
6. `payload_size`：记录长度。
7. `checksum`：用于检测 WAL 尾部损坏。
8. `payload`：向量数据、邻居信息、元信息等。

启动时扫描 WAL：

1. 从头读取 WAL record。
2. 校验 magic、size、checksum。
3. 遇到损坏尾部时停止读取。
4. 将有效记录 replay 到 MemTable。
5. 如果存在已经 flush 的 checkpoint，则只 replay checkpoint 之后的 WAL。

WAL 必须保证：

1. 写入后进程崩溃，重启后数据仍可恢复。
2. WAL 尾部半条记录损坏时不会导致系统启动失败。
3. WAL replay 后查询结果和崩溃前一致。
4. 支持 WAL truncate 或 rotate，避免文件无限增长。

## 四、SSTable 设计

SSTable 是 MemTable flush 后的磁盘不可变数据文件，用于保存增量写入的数据。

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

建议元信息：

1. SSTable id。
2. level。
3. record count。
4. min node id。
5. max node id。
6. data offset。
7. index offset。
8. checksum。
9. created timestamp。

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

SSTable 必须保证：

1. MemTable flush 后生成合法 SSTable。
2. SSTable 文件可重新加载。
3. 根据 node id 能够快速定位 record。
4. 多个 SSTable 同时存在时，新版本优先。
5. SSTable 读路径不会破坏原有静态索引搜索逻辑。

## 五、Compaction 设计

Compaction 用于将多个 SSTable 合并，减少读放大、空间放大和过期数据。

初始策略：

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

Compaction 流程：

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

可选优化：

1. 后台线程执行 Compaction。
2. Compaction 限速，避免影响前台查询。
3. 支持按照文件大小触发。
4. 支持按照读放大触发。
5. 支持 tombstone 清理。

Compaction 必须保证：

1. 多个 SSTable 能够合并为更少文件。
2. 相同 node id 保留最新版本。
3. Compaction 后查询结果不变。
4. Compaction 过程中前台查询不崩溃。
5. Compaction 后旧文件可安全删除。

## 六、增量插入与结果合并

系统需要支持新增向量 / 新节点在线插入，并能被后续查询访问。最小实现路径可以先采用“增量层 + 查询合并”的方式，而不是立即把新节点完全并入主图。

查询时：

1. 在原有 Disk Graph 中搜索 top-k。
2. 在 MemTable / SSTable 的增量数据中搜索 top-k。
3. 合并两个候选集合。
4. 按全精度距离 rerank。
5. 返回最终 top-k。

后续可演进方向：

1. 将增量节点周期性 merge 到主图。
2. 对新节点构建临时 Delta Graph。
3. 使用 HNSW / Vamana 小图作为增量层。
4. Compaction 后触发 graph rebuild 或 partial graph merge。

增量插入必须保证：

1. 插入新向量后，不重建主图也能查询到。
2. 混合查询结果包含 base graph 和 delta layer。
3. 插入数量增长时，系统仍能稳定运行。
4. 增量层查询开销可被 benchmark 量化。

## 七、Manifest 设计

动态写入系统最好增加 manifest 文件记录当前有效 SSTable 列表。

Manifest 示例：

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

Manifest 作用：

1. 系统启动时知道哪些 SSTable 有效。
2. Compaction 后可以原子切换文件集合。
3. WAL replay 可以从 checkpoint 之后开始。
4. 避免旧文件误读或新文件丢失。

## 八、实施阶段

第 5 周打通写入路径：

| 阶段 | 任务 | 交付物 |
| --- | --- | --- |
| Day 1 | 设计与接口拆分，明确数据结构、Record 格式、flush 阈值和配置项 | `memtable`、`wal`、`sstable` 接口与动态写入配置项 |
| Day 2 | 实现 MemTable insert/get/snapshot/clear，支持线程安全和查询接入 | MemTable 基础实现，插入后立即可查 |
| Day 3 | 实现 WAL append、checksum、replay、truncate/rotate | WAL 写入与恢复 |
| Day 4 | 实现 MemTable 到 SSTable flush，SSTable index 加载和查询读取 | SSTable 写入读取，基础查询合并 |
| Day 5 | 串联 insert -> WAL -> MemTable -> Flush -> SSTable | 动态写入主链路可跑通，初步性能数据 |

第 6 周完成 Compaction 与 Benchmark：

| 阶段 | 任务 | 交付物 |
| --- | --- | --- |
| Day 6 | 实现 Level-0 Compaction，保留最新版本并原子替换元数据 | L0 -> L1 Compaction，查询一致 |
| Day 7 | 支持后台 Compaction，查询使用快照视图，管理文件生命周期 | 后台 Compaction，并发读写稳定 |
| Day 8 | 优化增量层扫描，加入简单 id index 或小型 vector index | 增量层查询合并，top-k rerank 正确 |
| Day 9 | 设计 Read-heavy/Balanced/Write-heavy/Recovery/Compaction 场景 | mixed RW benchmark 与性能对比表 |
| Day 10 | 整理模块结构，输出报告和后续优化建议 | 动态写入总结文档与模块说明 |

## 九、Benchmark 与验证场景

混合读写 benchmark 场景：

| 场景 | 读比例 | 写比例 | 目标 |
| --- | ---: | ---: | --- |
| Read-heavy | 95% | 5% | 验证动态写入对查询延迟影响 |
| Balanced | 70% | 30% | 验证混合负载稳定性 |
| Write-heavy | 50% | 50% | 验证 WAL / MemTable / Flush 压力 |
| Recovery | - | - | 验证崩溃恢复正确性 |
| Compaction | - | - | 验证 Compaction 前后性能变化 |

指标：

1. Recall@10。
2. QPS。
3. Average latency。
4. P50 / P95 / P99 latency。
5. Insert throughput。
6. WAL append latency。
7. Flush latency。
8. Compaction duration。
9. Read amplification。
10. Write amplification。
11. Disk usage。
12. Memory usage。

单元验证内容：

| 验证项 | 内容 |
| --- | --- |
| MemTable 插入 | 插入后可 get |
| MemTable 快照 | snapshot 不受后续写入影响 |
| WAL 写入 | append 后文件大小正确 |
| WAL 恢复 | replay 后数据一致 |
| WAL 损坏 | 半条 record 不导致启动失败 |
| SSTable 写入 | flush 后文件存在 |
| SSTable 读取 | 根据 node id 可读取 |
| Compaction | 多文件合并后结果一致 |
| 增量查询 | 新插入数据可被 search 命中 |

端到端验证内容：

```text
1. 启动系统。
2. 插入 10000 条向量。
3. 查询部分新插入向量。
4. 触发 flush。
5. 重启系统。
6. replay WAL / load SSTable。
7. 再次查询。
8. 插入更多数据。
9. 触发 compaction。
10. 验证查询结果不变。
```

Benchmark 需要覆盖：

1. 纯读 baseline。
2. 读 95% / 写 5%。
3. 读 70% / 写 30%。
4. 读 50% / 写 50%。
5. Flush 压力。
6. Compaction 压力。
7. Recovery 的大 WAL replay 时间。

## 十、风险与后续方向

风险与规避：

| 风险 | 影响 | 规避方案 |
| --- | --- | --- |
| WAL fsync 过慢 | 写入延迟高 | 支持 group commit 或可配置 fsync 策略 |
| SSTable 数量过多 | 查询读放大严重 | 设置 L0 阈值并触发 Compaction |
| Compaction 阻塞查询 | P99 延迟升高 | 后台执行，查询使用快照 |
| 增量层扫描过慢 | 查询延迟升高 | 为增量层增加小型索引 |
| 崩溃恢复慢 | 启动时间过长 | WAL checkpoint + truncate |
| 文件状态不一致 | 数据丢失或重复读取 | 使用 manifest 原子更新 |
| 与原有搜索逻辑耦合过重 | 后续难维护 | 使用 DynamicWriteManager 隔离写入层 |

最小可交付版本可以压缩为：

1. WAL append + replay。
2. MemTable insert + search。
3. MemTable flush 到 SSTable。
4. 查询时合并 base graph 和 delta records。
5. 手动触发 Compaction。
6. 混合读写 benchmark 能跑出结果。

暂时可以不做：

1. 完整删除语义。
2. 多 Level Compaction。
3. 复杂增量图构建。
4. 高并发无锁优化。
5. 自动 merge 到主 Vamana 图。

动态写入闭环完成后，可以继续优化：

1. 增量层向量索引，避免线性扫描。
2. Delta Graph / HNSW 小图。
3. 主图周期性 rebuild。
4. Compaction 与 graph merge 联动。
5. WAL group commit。
6. 后台 flush。
7. O_DIRECT / io_uring 读写优化。
8. 混合负载下 P99 延迟优化。
9. crash consistency 更严格验证。

## 验收标准

最终实现应满足：

1. 支持在线 insert，不需要重建主索引。
2. 写入先落 WAL，再进入 MemTable。
3. 重启后可以通过 WAL 恢复未 flush 数据。
4. MemTable 达到阈值后可以 flush 成 SSTable。
5. SSTable 可被查询路径读取。
6. 多个 SSTable 可以进行基础 Compaction。
7. 查询结果可以合并 base graph 与 delta layer。
8. 完成混合读写 benchmark。
9. 输出 Recall、QPS、P95、P99、写入吞吐、恢复时间等指标。
10. 文档说明动态写入架构、文件格式、恢复流程和已知限制。
