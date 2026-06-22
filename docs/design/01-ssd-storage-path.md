# 01 SSD 存储链路设计文档

## 当前实现状态（2026-06-21）

SSD 存储链路的核心交付已经进入主路径：项目具备 4KB disk record、磁盘索引读写、packed graph 文件、O_DIRECT/pread 读路径以及对应验证。该文档现在作为 SSD 存储链路的设计归档和验收依据。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| 4KB 记录布局 | 已完成 | `DiskRecord`/packed page codec |
| SSD index 文件读写 | 已完成 | `DiskIndexReader`/`DiskIndexWriter`、packed graph index |
| O_DIRECT / pread fallback | 已完成 | Linux 下可启用 O_DIRECT，io_uring 不可用时回退 |
| 同步读写测试 | 已完成 | `test_disk_record`、`test_disk_index_rw` |
| 大规模 SIFT 复用索引 | 已完成 | `indexes/sift1m_vamana_pq100_p4096_sm.idx` |

## 1. 实现定位

| 编号 | 建议时间 | 核心任务 | 交付物 |
|---|---|---|---|
| 01 SSD 存储链路 | 第 2–3 周 | 完成 4KB 记录布局、`O_DIRECT` 读写、`DiskIndexReader` / `DiskIndexWriter` | SSD index 文件、同步读写验证 |

本设计主题目标是把图索引从“内存结构”落盘为“SSD 可随机读取的索引文件”，为后续图搜索、异步 I/O、预取、缓存和 io_uring 优化提供稳定的磁盘访问基础。

---

## 2. 总体目标

完成一个最小可用的 SSD 图索引存储链路，要求：

1. 每个节点记录固定占用 4KB，便于 page-level 随机读取。
2. 支持 Linux 下 `O_DIRECT` 对齐读写，减少 page cache 干扰。
3. 实现 `DiskIndexWriter`，负责把内存图索引写入 SSD index 文件。
4. 实现 `DiskIndexReader`，负责按 node id 随机读取指定节点记录。
5. 完成同步读写 correctness 测试，确保落盘前后数据一致。
6. 记录基础性能指标：吞吐、平均延迟、P50/P95/P99、随机读取次数。

---

## 3. 目录与文件建议

建议新增或整理如下文件：

```text
include/
  storage/
    disk_record.h
    disk_index_reader.h
    disk_index_writer.h

src/storage/
  disk_record.cpp
  disk_index_reader.cpp
  disk_index_writer.cpp
```

如当前项目已有 `core/` 或 `index/` 目录，也可以将 `storage/` 合并进去，但建议保持“磁盘存储层”和“图搜索层”解耦。

---

## 4. 4KB 记录布局设计

### 4.1 设计原则

每个节点占用一个固定大小记录：

```cpp
static constexpr size_t kPageSize = 4096;
```

固定 4KB 的好处：

- `node_id -> 文件偏移` 可以直接计算：`offset = header_size + node_id * 4096`。
- 适合 SSD page-level 随机读。
- 后续可以直接接入 `pread`、`io_uring`、page cache、block cache、prefetch。

### 4.2 推荐记录结构

建议一个 4KB record 包含：

```text
[RecordHeader]
  uint32_t magic
  uint32_t node_id
  uint16_t degree
  uint16_t dim
  uint32_t flags
  uint32_t checksum 可选

[Vector Payload]
  float vector[dim]

[Neighbor Payload]
  uint32_t neighbors[max_degree]

[Padding]
  zero padding 到 4096 bytes
```

如果向量维度为 SIFT 的 128 维：

```text
vector = 128 * 4 = 512 bytes
neighbors = R * 4 bytes
header 约 32 bytes
总大小远小于 4096，可安全容纳 R=64/96/128 等图邻居数
```

### 4.3 边界检查

写入前必须检查：

```cpp
sizeof(header) + dim * sizeof(float) + degree * sizeof(uint32_t) <= 4096
```

超过 4KB 时直接报错，不允许静默截断。

---

## 5. SSD Index 文件格式

### 5.1 文件整体布局

建议文件格式：

```text
[IndexFileHeader: 4096 bytes]
[NodeRecord 0: 4096 bytes]
[NodeRecord 1: 4096 bytes]
[NodeRecord 2: 4096 bytes]
...
[NodeRecord N-1: 4096 bytes]
```

### 5.2 IndexFileHeader 建议字段

```cpp
struct IndexFileHeader {
    uint32_t magic;          // 文件类型标识
    uint32_t version;        // 格式版本
    uint32_t dim;            // 向量维度
    uint32_t max_degree;     // 最大邻居数
    uint64_t num_nodes;      // 节点数量
    uint64_t record_size;    // 固定为 4096
    uint64_t header_size;    // 固定为 4096
    uint64_t entry_node;     // 图搜索入口点，可选
    uint8_t  reserved[...];  // padding 到 4096
};
```

要求：

- header 本身也对齐到 4096。
- 所有 node record 的起始偏移都是 4096 对齐。
- 后续版本升级只增加 header 字段，不破坏旧格式。

---

## 6. O_DIRECT 读写实现要求

### 6.1 打开文件

Linux 下使用：

```cpp
int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
```

写入时使用：

```cpp
int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT, 0644);
```

### 6.2 对齐要求

`O_DIRECT` 通常要求：

1. buffer 地址按 4096 对齐；
2. read/write size 是 4096 的倍数；
3. file offset 是 4096 的倍数。

建议封装统一分配函数：

```cpp
void* aligned_alloc_4k(size_t size) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 4096, size) != 0) {
        return nullptr;
    }
    std::memset(ptr, 0, size);
    return ptr;
}
```

---

## 7. DiskIndexWriter 设计

### 7.1 职责

`DiskIndexWriter` 负责把内存图结构写入 SSD index 文件。

核心接口建议：

```cpp
class DiskIndexWriter {
public:
    DiskIndexWriter(const std::string& path,
                    uint32_t dim,
                    uint32_t max_degree,
                    bool use_direct_io);

    void write_header(uint64_t num_nodes, uint64_t entry_node);

    void write_node(uint32_t node_id,
                    const float* vector,
                    uint32_t dim,
                    const std::vector<uint32_t>& neighbors);

    void close();
};
```

### 7.2 写入流程

```text
1. 创建 index 文件。
2. 写入 4KB IndexFileHeader。
3. 对每个 node：
   3.1 构造 4KB aligned record buffer。
   3.2 写入 record header。
   3.3 写入 vector。
   3.4 写入 neighbors。
   3.5 padding 清零。
   3.6 pwrite 到 header_size + node_id * 4096。
4. fsync。
5. close。
```

---

## 8. DiskIndexReader 设计

### 8.1 职责

`DiskIndexReader` 负责从 SSD index 文件中读取节点记录。

核心接口建议：

```cpp
class DiskIndexReader {
public:
    explicit DiskIndexReader(const std::string& path, bool use_direct_io);

    const IndexFileHeader& header() const;

    NodeRecord read_node(uint32_t node_id);

    void read_node_into(uint32_t node_id, void* aligned_buffer);

    void close();
};
```

### 8.2 读取流程

```text
1. open index 文件。
2. 读取 4KB header。
3. 校验 magic/version/record_size。
4. 根据 node_id 计算 offset。
5. 使用 pread 同步读取 4KB record。
6. 解析 vector 和 neighbors。
7. 返回 NodeRecord 或填充调用者提供的 buffer。
```

### 8.3 NodeRecord 建议

```cpp
struct NodeRecord {
    uint32_t node_id;
    uint16_t degree;
    uint16_t dim;
    std::vector<float> vector;
    std::vector<uint32_t> neighbors;
};
```

性能路径中不建议频繁创建 `std::vector`，后续图搜索可使用 `read_node_into()` 直接复用 buffer。

---

## 9. 同步读写测试计划

### 9.1 单元测试：record 编码与解码

建议验证目标：`test_disk_record`

测试内容：

- 构造一个 node record。
- 编码到 4KB buffer。
- 再从 buffer 解码。
- 检查 node_id、dim、degree、vector、neighbors 是否一致。
- 检查 padding 是否为 0。
- 检查超过 4KB 时是否正确报错。

### 9.2 集成测试：index 文件写入与读取

建议验证目标：`test_disk_index_rw`

测试内容：

```text
1. 随机生成 N 个向量和邻居表。
2. 使用 DiskIndexWriter 写入 index 文件。
3. 使用 DiskIndexReader 随机读取 node。
4. 对比读取结果与原始内存数据。
5. 分别测试普通 I/O 与 O_DIRECT。
```

建议参数：

```text
N = 1000
Dim = 128
MaxDegree = 64
RandomReadCount = 10000
```

### 9.3 性能测试：同步随机读 benchmark

可选扩展：如需单独压测同步磁盘读，可在 `src/` 下新增独立 benchmark 入口。

记录指标：

- total reads
- QPS
- avg latency
- P50 latency
- P95 latency
- P99 latency
- bytes read
- O_DIRECT on/off

输出格式建议：

```text
sync_disk_read_bench
nodes=100000 dim=128 degree=64 direct_io=1
reads=10000
qps=xxxxx
avg_us=xx
p50_us=xx
p95_us=xx
p99_us=xx
```

---

## 10. CMake 集成

建议新增选项：

```cmake
option(ENABLE_DIRECT_IO "Enable Linux O_DIRECT support" ON)
```

新增 target：

```cmake
add_library(storage
    src/storage/disk_record.cpp
    src/storage/disk_index_reader.cpp
    src/storage/disk_index_writer.cpp
)

target_include_directories(storage PUBLIC include)

# 可选：add_executable(bench_sync_disk_read src/bench_sync_disk_read.cpp)
target_link_libraries(bench_sync_disk_read PRIVATE storage)
```

---

## 11. 验收标准

### 11.1 功能验收

- [ ] 能生成 `.ssdindex` 或 `.disk.index` 文件。
- [ ] 文件 header 正确记录 dim、num_nodes、max_degree、record_size。
- [ ] 每个 node record 固定 4096 bytes。
- [ ] 支持通过 node_id 随机读取 record。
- [ ] 普通 I/O 模式读写测试通过。
- [ ] `O_DIRECT` 模式读写测试通过。
- [ ] 随机读取 10000 次无数据错误。
- [ ] 不存在未检查的 short read / short write。

### 11.2 正确性验收

- [ ] 写入前后 vector 完全一致。
- [ ] 写入前后 neighbors 完全一致。
- [ ] node_id 与 offset 映射正确。
- [ ] 越界 node_id 正确报错。
- [ ] degree 超过 max_degree 正确报错。
- [ ] record 超过 4096 bytes 正确报错。

### 11.3 性能验收

- [ ] benchmark 可以输出 QPS、Avg、P50、P95、P99。
- [ ] 普通 I/O 与 `O_DIRECT` 结果可对比。
- [ ] 在 WSL ext4 或原生 Linux 目录下测试，避免 `/mnt/c`、`/mnt/d` 干扰最终性能。
