# 08 预取时机设计文档

## 当前实现状态（2026-06-25）

该文档用于归档预取在搜索循环中的触发时机。根据代码和文档，预取时机可以归纳为启用条件、进入当前节点后的邻居预取、处理当前 batch 时的滚动窗口预取，以及 CPU/I/O 重叠时间线。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| 预取启用条件 | 已归档 | `io_engine`、`disk_reader`、磁盘文件状态 |
| 当前节点邻居预取 | 已归档 | 弹出候选节点后预取 PQ 筛选后的邻居 |
| batch 滚动窗口预取 | 已归档 | 当前 batch 计算时预取下一个 batch |
| CPU/I/O 重叠说明 | 已归档 | 时间线解释 I/O 与精确距离计算重叠 |

## 设计背景

本设计用于说明异步预取在搜索过程中的触发位置。预取的核心价值不是改变候选扩展语义，而是把后续可能发生的 SSD 读取提前提交，让 CPU 计算当前 batch 时，下一批 page 的 I/O 可以在后台推进。

## 核心目标

预取时机设计需要说明：

1. async prefetch 在什么前置条件下启用。
2. 搜索循环中哪些位置会触发预取。
3. 当前节点邻居预取与 batch 滚动窗口预取如何配合。
4. CPU 精确距离计算和 SSD I/O 如何形成流水线重叠。

## 相关实现文件

相关源码入口如下：

```text
src/graph/disk_graph_index.cpp
include/core/query_page_session.h
src/core/query_page_session.cpp
include/core/async_page_reader.h
src/core/async_page_reader.cpp
```

历史说明中也涉及 `diskann_search_enhanced()`、`DiskIndexReader::submit_async_batch()`、`DiskIndexReader::wait_async_batch()` 和 `DiskIndexReader::compute_distance_direct()` 等接口。

## 具体设计要求

## 一、预取启用条件

在 `diskann_search_enhanced()` 中，async prefetch 必须同时满足三个条件才会启用：

```cpp
bool use_async = (io_engine != nullptr) &&
                 (disk_reader != nullptr) &&
                 disk_reader->is_open();
```

即：

1. `IoEngine` 已创建。
2. `DiskIndexReader` 已打开磁盘文件。
3. 命令行未指定 `--no-async-prefetch`。

## 二、进入当前节点时预取邻居

当从候选堆中弹出当前节点 `cn` 后，立即对其图邻居发起预取：

```text
cn = candidates.pop_min()
-> 读取 cn 的图邻居（next-hop）
-> 用 PQ ADC 粗筛（估计距离排序）
-> 用 worst_dist * 4.0 做保守截断
-> 选 top (2 * beam_width) 个未访问、未缓存的节点
-> submit_async_batch(prefetch_ids)
```

默认 `beam_width = 8`，所以最多预取 16 个候选节点。

该触发点的作用是：当前扩展节点刚刚暴露出下一跳候选时，立刻把最可能被访问的邻居 page 提交到异步 I/O 队列。

## 三、处理当前 batch 时预取下一个 batch

预取的第二个触发点在 batch 处理过程中，形成滚动窗口：

```cpp
for (batch in chunks(pq_filtered, beam_width)) {
    wait_async_batch(batch);        // 等待当前 batch 的 I/O 完成

    // 趁 CPU 计算当前 batch 时，预取下一个 batch
    next_prefetch_ids = pq_filtered[batch_end : batch_end + 2 * beam_width];
    submit_async_batch(next_prefetch_ids);

    // CPU 计算当前 batch 的精确距离
    for (n : batch) {
        compute_distance_direct(n, query);
    }
}
```

该触发点的作用是：等待当前 batch page 就绪后，立即提交下一个窗口的 page，让下一批 I/O 与当前 batch 的 CPU 距离计算重叠。

## 四、CPU 与 I/O 重叠时间线

时间线可以抽象为：

```text
CPU:  PQ筛选 cn -> wait batch0 -> 精确计算 batch0 -> wait batch1 -> 精确计算 batch1 -> ...
I/O:  prefetch top16 -> prefetch [8..23] -> prefetch [16..31] -> ...
```

核心思想：当前 batch 在 CPU 上做精确距离计算时，下一个 batch 的 SSD 读取已经在后台进行，从而实现 CPU 和 I/O 的真正重叠。

## 五、预取时机总结

| 时机 | 位置 | 描述 |
| --- | --- | --- |
| 前置条件 | 搜索开始前 | `io_engine` 已初始化、`disk_reader` 已打开、未禁用 `--no-async-prefetch` |
| 时机一 | 弹出候选节点 `cn` 后 | 预取 `cn` 的 PQ 排名前 `2*beam_width` 的邻居 |
| 时机二 | 每个 `batch` 开始时 | 在等待当前 `batch` 完成后，立即为下一个 `batch` 发起预取 |

## 验收标准

最终实现或文档说明应满足：

1. 清楚说明 async prefetch 的启用条件。
2. 清楚说明当前节点邻居预取的触发位置。
3. 清楚说明 batch 滚动窗口预取的触发位置。
4. 能解释 `beam_width` 与预取候选数量的关系。
5. 能解释 CPU 精确距离计算和 SSD I/O 如何重叠。
6. 预取只改变读取时机，不改变搜索候选扩展语义。
