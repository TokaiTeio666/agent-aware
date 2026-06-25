# 预取的时机

根据代码和文档，这个项目中预取（prefetch）的时机可以归纳为以下几个层面：

---

## 1. 触发条件（何时启用预取）

在 `diskann_search_enhanced()` 中，async prefetch 必须同时满足三个条件才会启用：

```cpp
bool use_async = (io_engine != nullptr) &&
                 (disk_reader != nullptr) &&
                 disk_reader->is_open();
```

即：IoEngine 已创建 + DiskIndexReader 已打开磁盘文件 + 命令行未指定 `--no-async-prefetch`。

---

## 2. 搜索循环中的两个预取时机

预取在主搜索循环中有两个触发点，形成滚动流水线：

### 时机一：进入当前节点时 → 预取其邻居（Phase 1）

当从候选堆中弹出当前节点 `cn` 后，立即对其图邻居发起预取：

```text
cn = candidates.pop_min()
→ 读取 cn 的图邻居（next-hop）
→ 用 PQ ADC 粗筛（估计距离排序）
→ 用 worst_dist * 4.0 做保守截断
→ 选 top (2 * beam_width) 个未访问、未缓存的节点
→ submit_async_batch(prefetch_ids)  // 🔴 第一个预取点
```

默认 `beam_width = 8`，所以最多预取 16 个候选节点。

### 时机二：处理当前 batch 时 → 预取下一个 batch（滚动窗口）

```cpp
for (batch in chunks(pq_filtered, beam_width)) {
    wait_async_batch(batch);        // 等待当前 batch 的 I/O 完成

    // 🔴 第二个预取点：趁 CPU 计算当前 batch 时，预取下一个 batch
    next_prefetch_ids = pq_filtered[batch_end : batch_end + 2 * beam_width];
    submit_async_batch(next_prefetch_ids);

    // CPU 计算当前 batch 的精确距离
    for (n : batch) {
        compute_distance_direct(n, query);
    }
}
```

---

## 3. 时间线（CPU 与 I/O 重叠）

时间 →

```text
CPU:  PQ筛选 cn → wait batch0 → 精确计算 batch0 → wait batch1 → 精确计算 batch1 → ...
I/O:  prefetch top16 ──→ prefetch [8..23] ────→ prefetch [16..31] ────→ ...
```

核心思想：当前 batch 在 CPU 上做精确距离计算时，下一个 batch 的 SSD 读取已经在后台进行，从而实现 CPU 和 I/O 的真正重叠。

---

## 总结

| 时机 | 位置 | 描述 |
|---|---|---|
| 时机一 | 弹出候选节点 `cn` 后 | 预取 `cn` 的 PQ 排名前 `2*beam_width` 的邻居 |
| 时机二 | 每个 `batch` 开始时 | 在等待当前 `batch` 完成后，立即为下一个 `batch` 发起预取 |
| 前置条件 | 搜索开始前 | `io_engine` 已初始化、`disk_reader` 已打开、未禁用 `--no-async-prefetch` |
