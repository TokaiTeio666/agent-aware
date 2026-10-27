#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "graph/disk_graph_index.h"

namespace agent_aware {

class QueryPageSession {
 public:
  using Page = PackedDiskGraphIndex::DecodedPage;
  using Node = PackedDiskGraphIndex::DiskNode;

  QueryPageSession(PackedDiskGraphIndex& index,
                   const DiskGraphSearchConfig& config,
                   DiskGraphSearchStats& stats);
  QueryPageSession(const QueryPageSession&) = delete;
  QueryPageSession& operator=(const QueryPageSession&) = delete;
  ~QueryPageSession();

  const Page& get_or_load_page(std::uint32_t page_id);
  const Node& get_node(std::uint32_t node_id, bool count_same_page_reuse);
  std::vector<std::uint32_t> collect_missing_pages(
      const std::vector<std::uint32_t>& page_ids) const;
  // 批量载入页。stage 只用于把 demand 读归因到搜索阶段（见
  // DemandReadStage）。未列出的调用方一律使用 kCandidateExpand。
  void ensure_pages_loaded_batch(const std::vector<std::uint32_t>& page_ids,
                                 DemandReadStage stage =
                                     DemandReadStage::kCandidateExpand);
  std::size_t submit_topology_prefetch(
      const std::vector<std::uint32_t>& page_ids);
  void drain_ready_prefetches();
  void record_topology_candidates(
      const std::vector<TopologyPageCandidate>& candidates);

  bool is_node_materialized(std::uint32_t node_id) const;
  std::uint32_t page_for_node(std::uint32_t node_id) const;
  bool page_available(std::uint32_t page_id) const;
  bool pin_page(std::uint32_t page_id);
  void unpin_all();
  // 扩展时为邻居页打分期间置位：这期间发生的单页 demand 读归因到
  // kNeighborRank，而不是锚点探测。
  void set_reading_neighbor_pages(bool value) {
    reading_neighbor_pages_ = value;
  }
  void finish_query();

 private:
  std::uint64_t page_offset(std::uint32_t page_id) const;
  std::size_t demand_batch_limit() const;
  bool page_in_cache(std::uint32_t page_id) const;
  const Page* try_get_cached_page(std::uint32_t page_id);
  void record_duplicate_skipped(std::size_t count = 1);
  bool page_ready_in_session(std::uint32_t page_id) const;
  bool materialize_available_page(std::uint32_t page_id);
  void materialize_page(const Page& page);
  Page read_demand_page(std::uint32_t page_id, DemandReadStage stage);
  bool consume_prefetch_completion(bool wait);
  bool wait_for_prefetch_page(std::uint32_t page_id);
  void store_prefetched_page(std::uint32_t page_id, Page page);

  PackedDiskGraphIndex& index_;
  DiskGraphSearchStats& stats_;
  bool finished_ = false;
  bool reading_neighbor_pages_ = false;
  bool async_prefetch_ = false;
  bool prefetch_adaptive_ = true;
  std::size_t prefetch_top_k_ = 2;
  std::size_t prefetch_max_inflight_ = 2;
  std::size_t prefetch_query_limit_ = 0;
  std::size_t prefetch_query_submitted_ = 0;

  std::unordered_set<std::uint32_t> visited_pages_;
  std::unordered_set<std::uint32_t> accessed_nodes_;
  std::unordered_set<std::uint32_t> demand_pages_;
  std::unordered_set<std::uint32_t> pinned_pages_;
  std::unordered_map<std::uint32_t, Page> owned_pages_;
  std::unordered_set<std::uint32_t> materialized_pages_;
  std::unordered_map<std::uint32_t, Node> local_nodes_;
  std::unordered_map<std::uint64_t, std::uint32_t> prefetch_tokens_;
  std::unordered_set<std::uint32_t> pending_prefetch_pages_;
  std::unordered_map<std::uint32_t, Page> ready_prefetch_pages_;
  std::unordered_set<std::uint32_t> submitted_prefetch_pages_;
  std::unordered_set<std::uint32_t> useful_prefetch_pages_;
  std::string prefetch_trace_path_;
  std::string run_id_;
  std::uint64_t query_id_ = 0;
  std::vector<TopologyPageCandidate> topology_trace_candidates_;
};

}  // namespace agent_aware
