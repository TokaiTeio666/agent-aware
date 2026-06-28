# 04 beam_width 与 io_uring 批量读取设计

## 当前实现状态（2026-06-21）

该文档作为 `beam_width + io_uring batch read` 的语义归档。当前代码已经具备 effective beam width 计算、batch candidate 扩展、批量 page read、io_uring fallback 和相关统计；后续如果继续改搜索循环，应保持这里定义的参数语义。

| 项目 | 状态 | 当前对应实现 |
| --- | --- | --- |
| `beam_width` 参数语义 | 已完成 | `DiskGraphSearchConfig::beam_width`，`0` 表示跟随 `search_width` |
| batch candidate 收集 | 已完成 | `PackedDiskGraphIndex` 和 naive 路径均有 effective beam width |
| io_uring 批量提交 | 已完成 | `AsyncPageReader::batch_submit` |
| fallback 行为 | 已完成 | io_uring 不可用时回退同步读 |
| 指标输出 | 已完成 | `batch_count/max_batch_size/io_submit_syscalls/uring_*` |

## 设计背景

本设计用于完善磁盘图搜索的 `beam_width + io_uring` 批量读取逻辑。

当前项目已有 `search_width` 和 `beam_width` 两个参数：

1. `search_width` 表示一次 query 的总扩展预算，总 `expanded` 数不能超过 `search_width`。
2. `beam_width` 表示每一轮最多扩展多少个候选点。
3. CLI 层已经有：

```cpp
 effective_beam_width = min(requested_beam_width, search_width);
```

4. `SearchConfig` 中已经分别有 `search_width` 和 `beam_width`。
5. packed 搜索逻辑在 `disk_graph_index.cpp` 中。
6. naive 搜索逻辑在 `naive_disk_graph_index.cpp` 中。
7. 之前已经拆分过 `QueryPageSession`，负责 query-level page/cache/prefetch 生命周期。
8. 项目目标是降低 P99，同时保持 `SIFT1M Recall@10 >= 0.95`，并通过 `memory_budget_pass=1`。

---

## 核心目标

把 `beam_width` 进一步和 `io_uring` 批量 I/O 结合起来，使搜索每一轮可以：

1. 从候选队列中收集最多 `beam_width` 个未访问候选点。
2. 将这些候选点对应的 page 去重。
3. 对缺失 page 批量 submit `io_uring` 读请求。
4. 一次 submit 多个 SQE，而不是每个 page 单独 submit。
5. 批量 wait CQE，等待这一批 page 完成。
6. 然后统一 decode page、计算距离、扩展邻居、更新候选队列。
7. 保持 `search_width` 作为总扩展预算，不能因为 `beam_width` 批处理而超过预算。
8. `beam_width` 必须只控制每轮最多扩展数量，不能覆盖 `search_width`。

---

## 相关实现文件

相关源码入口如下：

```text
src/sift_search_benchmark.cpp
include/graph/disk_graph_index.h
src/graph/disk_graph_index.cpp
include/graph/naive_disk_graph_index.h
src/graph/naive_disk_graph_index.cpp
include/core/query_page_session.h
src/core/query_page_session.cpp
```

同时阅读项目中已有的 `io_uring` / `async` / `DirectIO` 相关实现文件。

---

## 具体设计要求

## 一、保留参数语义

确保所有路径中：

```text
config.search_width = 总 expanded budget
config.beam_width   = 每轮最大 batch expansion 数
```

设计要点：

```cpp
effective_beam_width = clamp(config.beam_width, 1, config.search_width);
```

如果 `beam_width` 未配置或为 0，则默认行为应兼容旧逻辑：

```text
beam_width = search_width
```

或者至少保证不破坏现有测试。

禁止出现以下混淆写法：

```cpp
config.search_width = config.beam_width;
```

---

## 二、实现 beam batch 收集逻辑

在 packed disk graph search 中重构主搜索循环。

目标伪代码如下：

```cpp
while (output.stats.expanded < config.search_width) {
    batch.clear();
    expanded_this_round = 0;

    while (expanded_this_round < effective_beam_width &&
           output.stats.expanded + expanded_this_round < config.search_width &&
           !candidate_queue.empty()) {

        cand = pop_best_unexpanded_candidate();

        if (already_visited(cand.node_id)) {
            continue;
        }

        mark_visited(cand.node_id);
        batch.push_back(cand);
        expanded_this_round++;
    }

    if (batch.empty()) {
        break;
    }

    page_batch = collect_unique_pages(batch);

    session.ensure_pages_loaded_batch(page_batch);
    // 内部使用 io_uring 批量 submit + wait

    for (cand : batch) {
        page = session.get_page_for_node(cand.node_id);
        decode_neighbors(page, cand.node_id);
        compute_distances();
        push_new_candidates();
        output.stats.expanded++;
    }
}
```

注意事项：

1. `visited` 语义要保持正确，不能重复扩展同一个 node。
2. page 去重要在 batch 内完成，避免多个 candidate 位于同一 page 时重复读。
3. `output.stats.expanded` 的增长必须和实际扩展 candidate 数一致。
4. `search_width` 是硬上限。
5. top-k / rerank / early stop 的语义不能被破坏。

---

## 三、扩展 QueryPageSession 批量接口

在 `QueryPageSession` 中增加或完善以下接口，名字可以根据项目风格调整：

```cpp
std::vector<PageId> collect_missing_pages(const std::vector<PageId>& pages);
Status ensure_pages_loaded_batch(const std::vector<PageId>& pages);
```

接口语义：

1. 输入一批 page id。
2. 先过滤已经在 cache / pinned / current query loaded 状态中的 page。
3. 对 missing pages 批量提交 `io_uring` read。
4. 一次性 submit，而不是循环每个 page submit 一次。
5. wait completions 时要检查每个 CQE 的 `res`。
6. 需要处理：
   - 短读；
   - 错误码；
   - EOF；
   - alignment 错误。
7. 完成后把 page 放入 query session 的 page cache / pinned pages。
8. 若当前编译环境没有 `io_uring`，必须保留同步 fallback，保证项目仍能编译和运行。

---

## 四、io_uring 批量提交设计

如果项目已有 `io_uring reader`，应优先复用，避免重复实现完全独立的 reader。

如果需要新增接口，建议形式：

```cpp
Status read_pages_batch(const std::vector<PageReadRequest>& requests);
```

其中 `PageReadRequest` 至少包含：

```cpp
struct PageReadRequest {
    PageId page_id;
    uint64_t file_offset;
    size_t read_size;
    void* dst_buffer;
    uint64_t user_data; // 或 request index
};
```

设计要点：

1. 准备多个 SQE。
2. `io_uring_submit()` 只调用一次或尽量少调用。
3. wait CQE 时能对应回原始 request。
4. 正确处理 partial read。
5. 正确处理 `O_DIRECT` 对齐要求。
6. 不破坏已有同步 DirectIO 路径。

---

## 五、统计指标

扩展或复用 `SearchStats`，至少能观察：

```text
expanded
page_reads
cache_hits
batch_count
avg_batch_size
max_batch_size
uring_submit_count
uring_cqe_count
duplicate_pages_eliminated
```

如果 `SearchStats` 不方便扩展，至少在 benchmark 输出中增加 debug 级别统计，保证可以判断 beam batching 是否真的生效。

---

## 六、benchmark 参数与输出

检查 `src/sift_search_benchmark.cpp`：

1. 保证 `--beam_width` 可以传入。
2. 如果 CLI 已经有 `--beam_width`，则不要重复添加。
3. 输出中打印：

```text
search_width
effective_beam_width
io_mode: sync / io_uring / fallback
batch_count
avg_batch_size
page_reads
cache_hits
```

4. 不允许 `beam_width > search_width` 后造成越界或语义混乱。

---

## 七、兼容 naive 搜索

`naive_disk_graph_index.cpp` 中也要检查 `beam_width` 语义。

设计要点：

1. 如果 naive 路径暂时无法接入 `io_uring`，至少要保证它仍然按 `beam_width` 控制每轮 expansion。
2. 不要让 naive 路径编译失败。
3. 如果 packed 和 naive 共享 `SearchConfig`，二者参数语义必须一致。

---

## 验收标准

最终实现应满足：

1. 编译通过。
2. 现有测试不失败。
3. `search_width` 和 `beam_width` 语义清晰，不混用。
4. `beam_width` 控制每轮候选扩展数量。
5. `io_uring` 能按 beam batch 批量 submit page reads。
6. page 去重生效。
7. fallback 路径可用。
8. SIFT smoke test recall 不明显下降。
9. benchmark 输出能看出 batching 是否生效。
10. 不要大规模重写无关模块。
11. 不要引入复杂全局状态。
