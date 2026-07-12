#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "graph/disk_graph_index.h"

namespace agent_aware {

class QueryPageSession {
 public:
  using Page = PackedDiskGraphIndex::DecodedPage;
  using PageHandle = std::shared_ptr<const Page>;
  struct Node {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> neighbors;
    std::vector<float> vector;
    PageHandle page;
  };

  QueryPageSession(PackedDiskGraphIndex& index,
                   const DiskGraphSearchConfig& config,
                   DiskGraphSearchStats& stats);
  QueryPageSession(const QueryPageSession&) = delete;
  QueryPageSession& operator=(const QueryPageSession&) = delete;
  ~QueryPageSession();

  PageHandle get_or_load_page(std::uint32_t page_id);
  const Node& get_node(std::uint32_t node_id, bool count_same_page_reuse,
                       bool require_vector = false);
  std::vector<std::uint32_t> collect_missing_pages(
      const std::vector<std::uint32_t>& page_ids) const;
  void ensure_pages_loaded_batch(const std::vector<std::uint32_t>& page_ids);
  std::size_t submit_topology_prefetch(
      const std::vector<std::uint32_t>& page_ids);
  void drain_ready_prefetches();
  void record_topology_candidates(
      const std::vector<TopologyPageCandidate>& candidates);

  bool is_node_materialized(std::uint32_t node_id) const;
  std::uint32_t page_for_node(std::uint32_t node_id) const;
  bool page_available(std::uint32_t page_id) const;
  bool pin_page(std::uint32_t page_id);
  void unpin_page(std::uint32_t page_id);
  void unpin_all();
  void finish_query();
  void apply_workspace_limits(std::size_t prefetch_top_k,
                              std::size_t prefetch_max_inflight);

 private:
  std::uint64_t page_offset(std::uint32_t page_id) const;
  std::size_t demand_batch_limit() const;
  bool page_in_cache(std::uint32_t page_id) const;
  PageHandle try_get_cached_page(std::uint32_t page_id);
  void record_duplicate_skipped(std::size_t count = 1);
  bool page_ready_in_session(std::uint32_t page_id) const;
  bool mark_unique_page(std::vector<std::uint64_t>& bits,
                        std::uint32_t page_id, std::size_t& count);
  bool materialize_available_page(std::uint32_t page_id);
  void remember_page(PageHandle page);
  Page read_demand_page(std::uint32_t page_id);
  bool consume_prefetch_completion(bool wait);
  bool wait_for_prefetch_page(std::uint32_t page_id);
  void store_prefetched_page(std::uint32_t page_id, Page page);

  PackedDiskGraphIndex& index_;
  DiskGraphSearchStats& stats_;
  bool finished_ = false;
  bool async_prefetch_ = false;
  bool prefetch_adaptive_ = true;
  std::size_t prefetch_top_k_ = 2;
  std::size_t prefetch_max_inflight_ = 2;
  std::size_t prefetch_query_limit_ = 0;
  std::size_t prefetch_query_submitted_ = 0;

  std::unordered_set<std::uint32_t> pinned_pages_;
  std::unordered_map<std::uint32_t, PageHandle> resident_pages_;
  std::unordered_set<std::uint32_t> materialized_pages_;
  std::unordered_map<std::uint64_t, std::uint32_t> prefetch_tokens_;
  std::unordered_set<std::uint32_t> pending_prefetch_pages_;
  std::unordered_map<std::uint32_t, Page> ready_prefetch_pages_;
  std::unordered_set<std::uint32_t> submitted_prefetch_pages_;
  std::unordered_set<std::uint32_t> useful_prefetch_pages_;
  std::string prefetch_trace_path_;
  std::string run_id_;
  std::uint64_t query_id_ = 0;
  std::vector<std::uint64_t> touched_page_bits_;
  std::vector<std::uint64_t> demand_page_bits_;
  std::size_t pages_touched_ = 0;
  std::size_t nodes_accessed_ = 0;
  std::size_t demand_pages_count_ = 0;
  std::vector<TopologyPageCandidate> topology_trace_candidates_;
  Node current_node_;
};

}  // namespace agent_aware
