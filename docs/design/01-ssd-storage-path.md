# 01 SSD 存储链路设计文档

## 当前实现状态（2026-06-21）

SSD 存储链路的核心交付已经进入主路径：项目具备 4KB disk record、磁盘索引读写、packed graph 文件、O_DIRECT/pread 读路径以及对应验证。该文档现在作为 SSD 存储链路的设计归档和验收依据。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| 4KB 记录布局 | 已完成 | `DiskRecord`/packed page codec |
| SSD index 文件读写 | 已完成 | `DiskIndexReader`/`DiskIndexWriter`、packed graph index |
| O_DIRECT / pread fallback | 已完成 | Linux 下可启用 O_DIRECT，io_uring 不可用时回退 |
| 同步读写验证 | 已完成 | record 编解码与 index 读写验证 |
| 大规模 SIFT 复用索引 | 已完成 | `indexes/sift1m_vamana_pq100_p4096_sm.idx` |

## 设计背景

本设计主题目标是把图索引从“内存结构”落盘为“SSD 可随机读取的索引文件”，为后续图搜索、异步 I/O、预取、缓存和 io_uring 优化提供稳定的磁盘访问基础。

该阶段对应第 2-3 周的核心任务：完成固定 4KB 记录布局、`O_DIRECT` 读写、`DiskIndexReader` / `DiskIndexWriter`，并形成可以被图搜索主路径复用的 SSD index 文件。

## 核心目标

完成一个最小可用的 SSD 图索引存储链路，要求：

1. 每个节点记录固定占用 4KB，便于 page-level 随机读取。
2. 支持 Linux 下 `O_DIRECT` 对齐读写，减少 page cache 干扰。
3. 实现 `DiskIndexWriter`，负责把内存图索引写入 SSD index 文件。
4. 实现 `DiskIndexReader`，负责按 node id 随机读取指定节点记录。
5. 完成同步读写 correctness 验证，确保落盘前后数据一致。
6. 记录基础性能指标：吞吐、平均延迟、P50/P95/P99、随机读取次数。

## 相关实现文件

相关源码入口如下：

```text
include/storage/disk_record.h
include/storage/disk_index_reader.h
include/storage/disk_index_writer.h
src/storage/disk_record.cpp
src/storage/disk_index_reader.cpp
src/storage/disk_index_writer.cpp
```

如当前项目已有 `core/` 或 `index/` 目录，也可以将 `storage/` 合并进去，但建议保持“磁盘存储层”和“图搜索层”解耦。

## 具体设计要求

## 一、固定 4KB 记录布局

每个节点占用一个固定大小记录：

```cpp
static constexpr size_t kPageSize = 4096;
```

固定 4KB 的好处：

1. `node_id -> 文件偏移` 可以直接计算：`offset = header_size + node_id * 4096`。
2. 适合 SSD page-level 随机读。
3. 后续可以直接接入 `pread`、`io_uring`、page cache、block cache、prefetch。

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

写入前必须检查：

```cpp
sizeof(header) + dim * sizeof(float) + degree * sizeof(uint32_t) <= 4096
```

超过 4KB 时直接报错，不允许静默截断。

## 二、SSD Index 文件格式

建议文件格式：

```text
[IndexFileHeader: 4096 bytes]
[NodeRecord 0: 4096 bytes]
[NodeRecord 1: 4096 bytes]
[NodeRecord 2: 4096 bytes]
...
[NodeRecord N-1: 4096 bytes]
```

`IndexFileHeader` 建议字段：

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

文件格式要求：

1. header 本身也对齐到 4096。
2. 所有 node record 的起始偏移都是 4096 对齐。
3. 后续版本升级只增加 header 字段，不破坏旧格式。

## 三、O_DIRECT 读写实现

Linux 下读路径使用：

```cpp
int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
```

写路径使用：

```cpp
int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT, 0644);
```

`O_DIRECT` 通常要求：

1. buffer 地址按 4096 对齐。
2. read/write size 是 4096 的倍数。
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

实现必须检查 short read / short write，并在不支持 `O_DIRECT` 或对齐条件不满足时提供明确 fallback 或错误。

## 四、DiskIndexWriter 设计

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

写入流程：

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

## 五、DiskIndexReader 设计

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

读取流程：

```text
1. open index 文件。
2. 读取 4KB header。
3. 校验 magic/version/record_size。
4. 根据 node_id 计算 offset。
5. 使用 pread 同步读取 4KB record。
6. 解析 vector 和 neighbors。
7. 返回 NodeRecord 或填充调用者提供的 buffer。
```

`NodeRecord` 建议：

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

## 六、CMake 与模块集成

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
target_link_libraries(bench_sync_disk_read PRIVATE storage)
```

存储层应作为可复用模块提供给 packed graph、同步读 benchmark 和后续异步 I/O 路径。

## 七、验证与性能指标

record 编解码验证需要覆盖：

1. 构造一个 node record。
2. 编码到 4KB buffer。
3. 再从 buffer 解码。
4. 检查 node_id、dim、degree、vector、neighbors 是否一致。
5. 检查 padding 是否为 0。
6. 检查超过 4KB 时是否正确报错。

index 文件读写验证需要覆盖：

1. 随机生成 N 个向量和邻居表。
2. 使用 `DiskIndexWriter` 写入 index 文件。
3. 使用 `DiskIndexReader` 随机读取 node。
4. 对比读取结果与原始内存数据。
5. 分别覆盖普通 I/O 与 `O_DIRECT`。

建议验证参数：

```text
N = 1000
Dim = 128
MaxDegree = 64
RandomReadCount = 10000
```

同步随机读性能记录指标：

1. total reads
2. QPS
3. avg latency
4. P50 latency
5. P95 latency
6. P99 latency
7. bytes read
8. O_DIRECT on/off

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

## 验收标准

最终实现应满足：

1. 能生成 `.ssdindex` 或 `.disk.index` 文件。
2. 文件 header 正确记录 dim、num_nodes、max_degree、record_size。
3. 每个 node record 固定 4096 bytes。
4. 支持通过 node id 随机读取 record。
5. 普通 I/O 模式读写验证通过。
6. `O_DIRECT` 模式读写验证通过。
7. 随机读取 10000 次无数据错误。
8. 不存在未检查的 short read / short write。
9. 写入前后 vector 完全一致。
10. 写入前后 neighbors 完全一致。
11. node id 与 offset 映射正确。
12. 越界 node id 正确报错。
13. degree 超过 max_degree 正确报错。
14. record 超过 4096 bytes 正确报错。
15. benchmark 可以输出 QPS、Avg、P50、P95、P99。
16. 普通 I/O 与 `O_DIRECT` 结果可对比。
17. 在 WSL ext4 或原生 Linux 目录下验证，避免 `/mnt/c`、`/mnt/d` 干扰最终性能。
