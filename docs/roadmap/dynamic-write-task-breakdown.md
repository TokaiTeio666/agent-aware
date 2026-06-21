# P5 动态写入拆分任务计划

## 当前实现状态（2026-06-21）

该拆分清单已基本完成 T1-T5 的核心能力：MemTable、WAL、SSTable、Compaction、manager 集成、mixed RW benchmark 和 update/delete 语义已经有测试与 SIFT1M 结果支撑。仍建议保留本文件作为后续拆分异步 flush queue、delta memory graph、周期性 rebuild 的任务模板。

| 子任务 | 状态 | 说明 |
| --- | --- | --- |
| T1 MemTable | 已完成 | 支持 snapshot、flush threshold、latest record |
| T2 WAL | 已完成 | 支持动态记录追加和 recovery 重放 |
| T3 SSTable | 已完成 | 支持写入、索引、scan/get |
| T4 Compaction | 已完成可运行版 | 支持 manifest 发布和 recovery 打开 |
| T5 Manager/查询合并 | 已完成可交付版 | 支持 base/delta merge、snapshot、immutable read view |
| T6 Benchmark/报告 | 已完成当前版本 | mixed RW JSON、分段指标、动态 Recall evidence |
| 后台 flush queue | 未完成 | 可继续按本拆分方式新增任务 |

## 0. 总体说明

P5 阶段不建议把 MemTable、WAL、SSTable、Compaction、增量插入全部交给一次 Codex 任务完成。  
更合理的方式是：先制定统一规范，然后把每个模块拆成独立任务，逐步实现、逐步测试、逐步合并。

本计划将 P5 拆成 6 个部分：

| 编号 | 任务 | 目标 |
|---|---|---|
| T0 | 总规范与接口约束 | 统一目录、数据结构、错误处理、测试标准 |
| T1 | MemTable | 实现内存写入层 |
| T2 | WAL | 实现写前日志和崩溃恢复 |
| T3 | SSTable | 实现 MemTable 刷盘后的不可变文件 |
| T4 | Compaction | 实现 SSTable 合并与旧版本清理 |
| T5 | 增量插入与查询合并 | 将动态写入层接入搜索流程 |
| T6 | 混合读写 Benchmark | 验证整体正确性和性能 |

---

# T0 总规范与接口约束

## 1. 总体架构规范

动态写入层采用类似 LSM-Tree 的分层结构：

```text
Write Path:
insert()
  -> WAL append
  -> MemTable insert
  -> optional flush to SSTable

Read Path:
search()
  -> base graph search
  -> delta layer search
       -> MemTable
       -> SSTable
  -> merge candidates
  -> rerank
  -> return topK

Recovery Path:
startup()
  -> load manifest
  -> load SSTable metadata
  -> replay WAL
  -> rebuild MemTable
```

动态写入层不能直接破坏现有静态图索引结构。  
P5 阶段只要求“新增数据可查”，不强制要求新节点立即并入主 Vamana / DiskANN 图。

---

## 2. 目录结构规范

建议新增目录：

```text
include/agentmem/dynamic/
  dynamic_record.h
  dynamic_write_manager.h
  memtable.h
  wal.h
  sstable.h
  compaction.h
  manifest.h

src/agent_aware/dynamic/
  dynamic_write_manager.cpp
  memtable.cpp
  wal.cpp
  sstable.cpp
  compaction.cpp
  manifest.cpp

tests/unit/dynamic/
  test_memtable.cpp
  test_wal.cpp
  test_sstable.cpp
  test_compaction.cpp
  test_dynamic_insert.cpp

tools/benchmarks/
  mixed_rw_benchmark.cpp
```

如果当前项目不适合新增 `dynamic/` 目录，也可以放在 `core/` 或 `graph/` 下，但必须保持模块边界清晰。

---

## 3. 命名规范

类名建议：

```cpp
DynamicRecord
MemTable
WalWriter
WalReader
SSTableWriter
SSTableReader
CompactionJob
Manifest
DynamicWriteManager
```

文件名建议使用小写下划线：

```text
dynamic_record.h
dynamic_write_manager.h
memtable.h
wal.h
sstable.h
compaction.h
manifest.h
```

---

## 4. 数据结构规范

所有动态写入模块统一使用 `DynamicRecord` 表示一条写入记录。

```cpp
struct DynamicRecord {
    uint64_t sequence_id = 0;
    NodeId node_id = 0;
    uint32_t dim = 0;
    std::vector<float> vector;
    std::vector<NodeId> neighbors;
    bool deleted = false;
};
```

说明：

- `sequence_id` 表示写入版本号，越大越新。
- `node_id` 表示节点 id。
- `dim` 表示向量维度。
- `vector` 保存原始向量。
- `neighbors` 暂时可为空，后续用于增量图。
- `deleted` 为删除预留，P5 可以不实现删除语义，但字段建议保留。

---

## 5. 版本优先级规范

当多个层级存在相同 `node_id` 时，以 `sequence_id` 最大的记录为准。

优先级：

```text
MemTable 最新记录
  >
新 SSTable
  >
旧 SSTable
  >
Base graph
```

如果 `deleted == true`，说明该记录为 tombstone，查询合并时应忽略 base graph 中对应旧数据。  
P5 如果暂不支持删除，可以先不暴露 delete 接口，但内部格式保留。

---

## 6. 持久化规范

动态写入数据目录建议：

```text
data/dynamic/
  manifest.json
  wal/
    wal_000001.log
  sstable/
    sst_000001.data
    sst_000001.index
    sst_000001.meta
```

所有落盘文件都要有：

- magic number
- version
- record count
- checksum 或 basic validation
- 文件生成时间或 sequence range

---

## 7. 错误处理规范

所有模块禁止直接 `exit()`。  
错误应通过返回值、异常或项目已有错误机制返回。

建议最小错误类型：

```cpp
enum class DynamicStatus {
    Ok,
    NotFound,
    IOError,
    Corrupted,
    InvalidArgument,
    ChecksumMismatch,
    Unsupported
};
```

如果项目没有统一 `Status` 类型，可以先用 `bool + error message`，但不要静默失败。

---

## 8. 并发规范

P5 阶段不要求极致并发，但至少要满足：

- 单写多读不崩溃。
- flush 时不破坏正在查询的 MemTable 快照。
- compaction 替换 SSTable 列表时需要原子切换。
- 查询线程不能读到已经删除的 SSTable 文件。

建议策略：

```text
MemTable:
  shared_mutex 保护读写

SSTable list:
  使用 shared_ptr<vector<SSTableReader>>
  compaction 后整体替换

文件删除:
  延迟删除旧文件
```

---

## 9. 测试规范

每个子任务必须有单测。  
不要等所有模块完成后再统一测试。

最低测试要求：

| 模块 | 必须测试 |
|---|---|
| MemTable | insert / get / snapshot / clear |
| WAL | append / replay / broken tail |
| SSTable | flush / load / get |
| Compaction | merge / deduplicate / newest wins |
| 增量插入 | insert 后 search 可命中 |
| Benchmark | read-heavy / balanced / write-heavy |

---

## 10. Codex 工作规范

每个 Codex 任务必须遵守：

1. 不大规模重写现有搜索主逻辑。
2. 不改变已有 benchmark 的输入输出格式，除非明确说明。
3. 每次只做一个模块。
4. 每个模块完成后必须能编译。
5. 每个模块完成后必须增加测试。
6. 修改 CMakeLists.txt 时只添加必要源文件。
7. 不引入复杂第三方库。
8. 文件格式先保持简单可读，不追求一次性工业级。
9. 保持 P5 目标：支持动态写入，而不是重写整个 DiskANN。

---

# T1 MemTable 任务

## 1. 任务目标

实现动态写入的内存层。  
MemTable 用于保存最新插入的数据，并支持查询路径读取。

---

## 2. 输入输出

输入：

```cpp
DynamicRecord record
```

输出：

```cpp
insert success / failed
get success / not found
snapshot records
```

---

## 3. 需要新增文件

```text
include/agentmem/dynamic/dynamic_record.h
include/agentmem/dynamic/memtable.h
src/agent_aware/dynamic/memtable.cpp
tests/unit/dynamic/test_memtable.cpp
```

---

## 4. 建议接口

```cpp
class MemTable {
public:
    explicit MemTable(size_t flush_threshold_bytes);

    bool insert(const DynamicRecord& record);
    bool get(NodeId node_id, DynamicRecord& out) const;
    bool contains(NodeId node_id) const;

    std::vector<DynamicRecord> snapshot() const;
    void clear();

    size_t size() const;
    size_t bytes() const;
    bool should_flush() const;

private:
    size_t flush_threshold_bytes_;
    size_t current_bytes_;
    std::unordered_map<NodeId, DynamicRecord> records_;
    mutable std::shared_mutex mutex_;
};
```

---

## 5. 实现要求

- 插入相同 `node_id` 时，保留 `sequence_id` 更大的记录。
- `snapshot()` 返回稳定副本，不能被后续 insert 影响。
- `bytes()` 需要粗略估算内存占用。
- `should_flush()` 根据阈值判断是否需要刷盘。
- 不负责 WAL，不负责 SSTable，只管理内存数据。

---

## 6. 验收标准

- 插入一条记录后可以 get。
- 插入相同 node_id 的旧版本不会覆盖新版本。
- 插入相同 node_id 的新版本可以覆盖旧版本。
- snapshot 后继续插入，旧 snapshot 不变化。
- 达到阈值后 `should_flush()` 返回 true。
- 单测全部通过。

---

## 7. Codex Prompt

```text
请在当前项目中实现 P5 动态写入的 T1 MemTable 模块。

要求：
1. 新增 dynamic_record.h，定义 DynamicRecord，包括 sequence_id、node_id、dim、vector、neighbors、deleted 字段。
2. 新增 memtable.h / memtable.cpp，实现 MemTable。
3. MemTable 支持 insert、get、contains、snapshot、clear、size、bytes、should_flush。
4. 相同 node_id 只保留 sequence_id 最大的记录。
5. 使用 shared_mutex 保证基本单写多读安全。
6. 增加 tests/unit/dynamic/test_memtable.cpp，覆盖插入、覆盖、snapshot、flush threshold。
7. 修改 CMakeLists.txt，加入新源文件和测试。
8. 不要修改现有搜索主逻辑。
9. 保证项目可以编译并运行测试。
```

---

# T2 WAL 任务

## 1. 任务目标

实现写前日志，保证写入记录在崩溃后可以恢复。

---

## 2. 依赖

依赖 T1 的：

```text
DynamicRecord
```

不依赖 MemTable 的内部实现。

---

## 3. 需要新增文件

```text
include/agentmem/dynamic/wal.h
src/agent_aware/dynamic/wal.cpp
tests/unit/dynamic/test_wal.cpp
```

---

## 4. WAL 记录格式

建议二进制格式：

```text
magic: uint32_t
version: uint16_t
op_type: uint16_t
sequence_id: uint64_t
node_id: uint64_t
dim: uint32_t
vector_bytes: uint32_t
neighbor_count: uint32_t
deleted: uint8_t
payload_checksum: uint32_t
payload:
  vector float array
  neighbors NodeId array
```

---

## 5. 建议接口

```cpp
class WalWriter {
public:
    explicit WalWriter(const std::filesystem::path& path);
    bool append(const DynamicRecord& record);
    bool sync();
    bool close();
};

class WalReader {
public:
    explicit WalReader(const std::filesystem::path& path);
    std::vector<DynamicRecord> replay();
};
```

---

## 6. 实现要求

- append 必须追加写。
- replay 遇到损坏尾部时停止，不应崩溃。
- 校验 magic、version、payload size、checksum。
- 支持空 WAL 文件。
- 支持多条记录 replay。
- 不负责 MemTable insert，只返回 records。

---

## 7. 验收标准

- append 一条，replay 得到一条。
- append 多条，replay 顺序一致。
- 文件尾部写入半条垃圾数据，replay 仍能返回前面的完整记录。
- checksum 错误时停止读取或返回错误，不允许崩溃。
- WAL 文件不存在时行为明确，可以返回空或错误，但不能崩溃。

---

## 8. Codex Prompt

```text
请在当前项目中实现 P5 动态写入的 T2 WAL 模块。

要求：
1. 基于 include/agentmem/dynamic/dynamic_record.h 的 DynamicRecord 实现 WAL。
2. 新增 wal.h / wal.cpp。
3. 实现 WalWriter：append、sync、close。
4. 实现 WalReader：replay。
5. WAL 使用简单二进制格式，包含 magic、version、sequence_id、node_id、dim、vector、neighbors、deleted、checksum。
6. replay 遇到文件尾部半条损坏记录时停止读取，不要崩溃。
7. 增加 tests/unit/dynamic/test_wal.cpp，测试 append/replay、多条记录、损坏尾部、空文件。
8. 修改 CMakeLists.txt。
9. 不要接入搜索逻辑，不要实现 SSTable，不要做 Compaction。
10. 保证项目可以编译并运行测试。
```

---

# T3 SSTable 任务

## 1. 任务目标

实现 MemTable flush 后的不可变磁盘文件。  
SSTable 用于保存已经落盘的动态增量数据。

---

## 2. 依赖

依赖：

```text
DynamicRecord
MemTable::snapshot()
```

不依赖 WAL。

---

## 3. 需要新增文件

```text
include/agentmem/dynamic/sstable.h
src/agent_aware/dynamic/sstable.cpp
tests/unit/dynamic/test_sstable.cpp
```

---

## 4. 文件设计

建议先使用三文件结构：

```text
sst_000001.data
sst_000001.index
sst_000001.meta
```

### data 文件

保存 DynamicRecord 的二进制内容。

### index 文件

保存：

```text
node_id -> offset
node_id -> sequence_id
```

### meta 文件

保存：

```json
{
  "sstable_id": 1,
  "level": 0,
  "record_count": 1000,
  "min_sequence_id": 1,
  "max_sequence_id": 1000
}
```

---

## 5. 建议接口

```cpp
class SSTableWriter {
public:
    SSTableWriter(const std::filesystem::path& dir, uint64_t sstable_id, int level);
    bool write(const std::vector<DynamicRecord>& records);
};

class SSTableReader {
public:
    explicit SSTableReader(const std::filesystem::path& base_path);

    bool open();
    bool get(NodeId node_id, DynamicRecord& out) const;

    uint64_t id() const;
    int level() const;
    size_t record_count() const;
    uint64_t max_sequence_id() const;
};
```

---

## 6. 实现要求

- 写入时按 `node_id` 建 index。
- 相同 node_id 如果出现在输入 records 中，只保留 sequence_id 最大的版本。
- 读取时通过 index 定位 data offset。
- SSTable 文件不可变，不支持原地修改。
- 支持重新打开已有 SSTable。
- SSTableReader 可被查询路径长期持有。

---

## 7. 验收标准

- 输入 records 可以写成 SSTable。
- 关闭后重新打开，仍可以读取。
- get 已存在 node_id 成功。
- get 不存在 node_id 返回 not found。
- 相同 node_id 保留最新 sequence_id。
- meta 信息正确。

---

## 8. Codex Prompt

```text
请在当前项目中实现 P5 动态写入的 T3 SSTable 模块。

要求：
1. 基于 DynamicRecord 实现 SSTableWriter 和 SSTableReader。
2. 新增 sstable.h / sstable.cpp。
3. 使用三文件结构：.data、.index、.meta。
4. SSTableWriter::write 接收 vector<DynamicRecord>，写入不可变 SSTable。
5. 相同 node_id 只保留 sequence_id 最大记录。
6. SSTableReader::open 加载 index 和 meta。
7. SSTableReader::get 根据 node_id 快速读取 DynamicRecord。
8. 增加 tests/unit/dynamic/test_sstable.cpp，覆盖写入、重新打开、读取、not found、重复 node_id。
9. 修改 CMakeLists.txt。
10. 不要实现 WAL，不要实现 Compaction，不要修改搜索主逻辑。
11. 保证项目可以编译并运行测试。
```

---

# T4 Compaction 任务

## 1. 任务目标

实现多个 SSTable 的合并，减少 SSTable 数量和读放大。

---

## 2. 依赖

依赖：

```text
DynamicRecord
SSTableReader
SSTableWriter
```

---

## 3. 需要新增文件

```text
include/agentmem/dynamic/compaction.h
src/agent_aware/dynamic/compaction.cpp
tests/unit/dynamic/test_compaction.cpp
```

---

## 4. Compaction 策略

P5 只实现最小版本：

```text
多个 L0 SSTable
  -> 读取所有 records
  -> 按 node_id 去重
  -> 保留 sequence_id 最大的版本
  -> 写成一个 L1 SSTable
```

暂时不做复杂 level size policy。

---

## 5. 建议接口

```cpp
struct CompactionInput {
    std::vector<std::shared_ptr<SSTableReader>> input_tables;
    std::filesystem::path output_dir;
    uint64_t output_sstable_id;
    int output_level = 1;
};

struct CompactionResult {
    bool success = false;
    std::filesystem::path output_base_path;
    size_t input_record_count = 0;
    size_t output_record_count = 0;
};

class CompactionJob {
public:
    explicit CompactionJob(CompactionInput input);
    CompactionResult run();
};
```

---

## 6. 实现要求

- 合并多个 SSTable。
- 相同 node_id 保留最新 sequence_id。
- deleted record 可以保留，后续查询层根据 tombstone 处理。
- 输出新的 SSTable。
- 不直接删除旧文件，删除由 Manifest 或 DynamicWriteManager 管理。
- CompactionJob 不负责后台线程，只负责一次合并任务。

---

## 7. 验收标准

- 2 个 SSTable 可以合并为 1 个。
- 重复 node_id 保留最新版本。
- 不重复 node_id 全部保留。
- 输出 SSTable 可以正常打开和读取。
- 原输入 SSTable 文件不被 CompactionJob 删除。

---

## 8. Codex Prompt

```text
请在当前项目中实现 P5 动态写入的 T4 Compaction 模块。

要求：
1. 基于 SSTableReader / SSTableWriter 实现 CompactionJob。
2. 新增 compaction.h / compaction.cpp。
3. CompactionInput 包含输入 SSTableReader 列表、输出目录、输出 sstable id、输出 level。
4. CompactionJob::run 读取所有输入 records，按 node_id 去重，保留 sequence_id 最大版本，写出新的 SSTable。
5. CompactionJob 不负责删除旧文件。
6. 增加 tests/unit/dynamic/test_compaction.cpp，覆盖多 SSTable 合并、重复 node_id、新版本优先、输出可读取。
7. 修改 CMakeLists.txt。
8. 不要接入搜索逻辑，不要实现后台线程，不要实现复杂多 Level 策略。
9. 保证项目可以编译并运行测试。
```

---

# T5 增量插入与查询合并任务

## 1. 任务目标

把 WAL、MemTable、SSTable 接入系统，让新插入的数据能够被查询命中。

---

## 2. 依赖

依赖：

```text
DynamicRecord
MemTable
WalWriter
WalReader
SSTableReader
SSTableWriter
CompactionJob 可选
```

---

## 3. 需要新增文件

```text
include/agentmem/dynamic/dynamic_write_manager.h
include/agentmem/dynamic/manifest.h
src/agent_aware/dynamic/dynamic_write_manager.cpp
src/agent_aware/dynamic/manifest.cpp
tests/unit/dynamic/test_dynamic_insert.cpp
```

---

## 4. DynamicWriteManager 职责

```text
insert:
  WAL append
  MemTable insert
  maybe flush

startup:
  load manifest
  load SSTables
  replay WAL

search delta:
  search MemTable
  search SSTables
  return candidate records

flush:
  MemTable snapshot
  write SSTable
  update manifest
  clear MemTable
  checkpoint WAL
```

---

## 5. 建议接口

```cpp
struct DynamicWriteOptions {
    std::filesystem::path dynamic_dir;
    size_t memtable_flush_bytes = 64 * 1024 * 1024;
    bool enable_wal = true;
    bool enable_auto_flush = true;
};

class DynamicWriteManager {
public:
    explicit DynamicWriteManager(DynamicWriteOptions options);

    bool open();
    bool insert(NodeId node_id, const float* vector, uint32_t dim);
    bool flush();

    bool get(NodeId node_id, DynamicRecord& out) const;

    std::vector<DynamicRecord> collect_all_delta_records() const;
    std::vector<DynamicRecord> search_delta_l2(
        const float* query,
        uint32_t dim,
        size_t topk
    ) const;

    bool close();
};
```

---

## 6. 查询合并策略

P5 先使用简单版本：

```text
base_results = base graph search(query, topk)
delta_results = dynamic_manager.search_delta_l2(query, dim, topk)
merged = merge(base_results, delta_results)
rerank by full precision L2
return topk
```

注意：

- 不要强行把 delta 节点插入主图。
- delta 规模小的时候可以线性扫描。
- benchmark 需要统计 delta 扫描耗时。
- 如果 delta 很大，P6 再做 delta vector index。

---

## 7. Manifest 设计

`manifest.json` 记录当前有效 SSTable：

```json
{
  "version": 1,
  "next_sequence_id": 10001,
  "next_sstable_id": 5,
  "sstables": [
    {
      "id": 1,
      "level": 0,
      "base_path": "sstable/sst_000001"
    },
    {
      "id": 2,
      "level": 0,
      "base_path": "sstable/sst_000002"
    }
  ],
  "wal_checkpoint": 0
}
```

---

## 8. 实现要求

- `insert()` 必须先写 WAL，再写 MemTable。
- `open()` 时必须能加载 manifest 和 SSTable。
- `open()` 时必须 replay WAL。
- `flush()` 后生成 SSTable，并更新 manifest。
- 查询 delta 层时，新版本覆盖旧版本。
- 先实现线性扫描 delta records 的 L2 top-k。
- 接入现有搜索时尽量少改主逻辑，只在最终 top-k 前增加 merge/rerank。

---

## 9. 验收标准

- 插入新向量后，不重启即可查询到。
- 插入新向量后，重启仍可查询到。
- flush 后 MemTable 清空，但数据仍可查询到。
- base graph 和 delta layer 的结果能合并。
- 相同 node_id 的新版本覆盖旧版本。
- 不影响原有静态搜索测试。

---

## 10. Codex Prompt

```text
请在当前项目中实现 P5 动态写入的 T5 增量插入与查询合并模块。

要求：
1. 新增 DynamicWriteManager 和 Manifest。
2. DynamicWriteManager 负责 open、insert、flush、get、search_delta_l2、close。
3. insert 必须先 WAL append，再 MemTable insert。
4. open 时加载 manifest、加载 SSTable、replay WAL。
5. flush 时将 MemTable snapshot 写成新的 SSTable，更新 manifest，然后清理 MemTable。
6. Manifest 使用简单 JSON 或项目已有配置格式保存有效 SSTable 列表、next_sequence_id、next_sstable_id。
7. search_delta_l2 使用线性扫描 MemTable + SSTable records，返回 top-k DynamicRecord。
8. 在现有搜索流程最终返回前接入 delta result merge 和 full precision rerank。
9. 不要把新节点强行写入 base graph，不要重建主图。
10. 增加 tests/unit/dynamic/test_dynamic_insert.cpp，覆盖 insert 后可查、重启恢复、flush 后可查、base+delta 合并。
11. 修改 CMakeLists.txt。
12. 保证原有静态搜索测试不被破坏。
```

---

# T6 混合读写 Benchmark 任务

## 1. 任务目标

验证动态写入层在混合读写场景下的正确性和性能影响。

---

## 2. 需要新增文件

```text
tools/benchmarks/mixed_rw_benchmark.cpp
README.md 中的 mixed RW 运行说明
docs/experiments/sift1m-mixed-rw-immutable-view.md
docs/experiments/high-concurrency-mixed-rw.md
```

---

## 3. Benchmark 场景

| 场景 | 读比例 | 写比例 | 目的 |
|---|---:|---:|---|
| Read-heavy | 95% | 5% | 观察动态层对查询延迟影响 |
| Balanced | 70% | 30% | 观察常规混合负载 |
| Write-heavy | 50% | 50% | 观察写入压力 |
| Flush-heavy | 读写混合 | 高频 flush | 观察 flush 对 P99 的影响 |
| Recovery | - | - | 观察 WAL replay 时间 |
| Compaction | - | - | 观察 compaction 前后读放大变化 |

---

## 4. 指标

必须输出：

```text
read_qps
write_qps
insert_throughput
avg_latency
p50_latency
p95_latency
p99_latency
recall_at_10
wal_append_avg_us
memtable_insert_avg_us
flush_duration_ms
sstable_count
delta_record_count
memory_usage_mb
disk_usage_mb
recovery_time_ms
```

如果当前 recall groundtruth 不方便接入，可以先输出 latency 和 throughput，但文档中要明确说明 recall 暂未统计。

---

## 5. 实现要求

- 支持配置读写比例。
- 支持配置总操作数。
- 支持配置写入 batch 大小。
- 支持配置是否启用 flush。
- 支持配置是否启用 compaction。
- 输出 CSV 或 JSON。
- 不要只跑一个 case，至少跑 read-heavy、balanced、write-heavy 三组。

---

## 6. 验收标准

- benchmark 能独立运行。
- 输出稳定指标。
- 能对比静态 baseline 和动态写入版本。
- 能看到 MemTable、SSTable 数量变化。
- 能看到 P95 / P99 延迟变化。
- 有 benchmark 结果文档。

---

## 7. Codex Prompt

```text
请在当前项目中实现 P5 动态写入的 T6 混合读写 Benchmark。

要求：
1. 新增 tools/benchmarks/mixed_rw_benchmark.cpp。
2. 支持配置 read_ratio、write_ratio、num_operations、topk、enable_flush、enable_compaction。
3. benchmark 混合执行 search 和 insert。
4. 统计 read_qps、write_qps、avg_latency、p50、p95、p99、insert_throughput、flush_duration、sstable_count、delta_record_count。
5. 至少提供 read-heavy 95/5、balanced 70/30、write-heavy 50/50 三种场景。
6. 输出 CSV 或 JSON。
7. 在 README 中补充 mixed RW 常用运行命令。
8. 在 `docs/experiments/sift1m-mixed-rw-immutable-view.md` 和 `docs/experiments/high-concurrency-mixed-rw.md` 说明如何运行和如何解读。
9. 不要修改动态写入核心逻辑。
10. 保证项目可以编译运行。
```

---

# 统一开发顺序

推荐按以下顺序推进：

```text
T0 总规范
  ↓
T1 MemTable
  ↓
T2 WAL
  ↓
T3 SSTable
  ↓
T4 Compaction
  ↓
T5 增量插入与查询合并
  ↓
T6 混合读写 Benchmark
```

原因：

1. MemTable 是最小可跑写入层。
2. WAL 依赖统一 record 格式，但不依赖 SSTable。
3. SSTable 依赖 MemTable snapshot。
4. Compaction 依赖 SSTable。
5. 增量插入需要把前面模块串起来。
6. Benchmark 必须最后做，否则测不到完整路径。

---

# 最小可交付版本

如果第 5–6 周时间不够，优先完成：

```text
T0 总规范
T1 MemTable
T2 WAL
T3 SSTable
T5 增量插入与查询合并
T6 简化 Benchmark
```

可以暂时弱化：

```text
T4 Compaction
```

Compaction 可以先做手动触发，不做后台线程，不做多 level 策略。

---

# 每个任务完成后的检查清单

每个任务结束都要检查：

- [ ] 是否能编译。
- [ ] 是否新增对应单测。
- [ ] 是否没有破坏原有测试。
- [ ] 是否没有把一个模块的职责写到另一个模块里。
- [ ] 是否更新 CMakeLists.txt。
- [ ] 是否有清晰错误处理。
- [ ] 是否保留和现有静态索引兼容。
- [ ] 是否可以被下一个任务复用。

---

# 最终验收清单

P5 阶段最终完成时，应满足：

- [ ] 支持动态 insert。
- [ ] insert 先写 WAL，再写 MemTable。
- [ ] WAL 可以 replay。
- [ ] MemTable 可以 flush 成 SSTable。
- [ ] SSTable 可以重新加载。
- [ ] 多个 SSTable 可以合并。
- [ ] 查询可以合并 base graph 和 delta layer。
- [ ] 新插入数据不重建主图也能被查到。
- [ ] 重启后未丢失动态写入数据。
- [ ] 混合读写 benchmark 能输出 P50 / P95 / P99 / QPS / 写入吞吐。
- [ ] 文档说明当前限制和后续优化方向。
