#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agent_aware/core/async_page_reader.h"
#include "agent_aware/core/prefetch_planner.h"
#include "agent_aware/graph/disk_graph_index.h"

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

  bool async_prefetch_enabled() const;
  bool frontier_prefetch_enabled() const;
  bool next_hop_prefetch_enabled() const;
  bool candidate_prefetch_enabled() const;
  std::size_t prefetch_width() const;

  const Page& get_or_load_page(std::uint32_t page_id);
  const Page* try_get_cached_page(std::uint32_t page_id);
  const Node& get_node(std::uint32_t node_id, bool count_same_page_reuse);
  std::vector<std::uint32_t> collect_missing_pages(
      const std::vector<std::uint32_t>& page_ids) const;
  void ensure_pages_loaded_batch(const std::vector<std::uint32_t>& page_ids);

  bool is_node_materialized(std::uint32_t node_id) const;
  std::uint32_t page_for_node(std::uint32_t node_id) const;

  void submit_prefetch(const std::vector<std::uint32_t>& page_ids);
  void submit_candidate_prefetch(const std::vector<std::uint32_t>& page_ids);
  void submit_next_hop_prefetch(
      const std::vector<std::uint32_t>& node_ids,
      const std::unordered_set<std::uint32_t>& excluded_nodes,
      const std::vector<std::uint32_t>& fallback_page_ids = {});
  void drain_ready_pages();

  bool pin_page(std::uint32_t page_id);
  void unpin_all();
  void finish_query();

 private:
  std::uint64_t page_offset(std::uint32_t page_id) const;
  std::size_t async_page_footprint() const;
  std::size_t demand_io_reserve() const;
  void update_pending_peak();
  bool page_in_cache(std::uint32_t page_id) const;
  PrefetchPlanner::PageAvailability page_availability(
      std::uint32_t page_id) const;
  void apply_prefetch_plan_stats(const PrefetchPlanner::PlanStats& stats);
  void record_duplicate_skipped(std::size_t count = 1);

  bool page_ready_in_session(std::uint32_t page_id) const;
  bool materialize_available_page(std::uint32_t page_id);
  void materialize_page(const Page& page);
  Page read_demand_page(std::uint32_t page_id);
  bool consume_prefetch_completion(AsyncPageReader::CompletedRead completed,
                                   bool keep_ready);
  bool harvest_prefetch(bool wait);
  void wait_for_prefetch_page(std::uint32_t page_id);
  std::size_t prefetch_batch_limit() const;
  bool queue_prefetch_request(
      std::uint32_t page_id,
      std::vector<AsyncPageReader::ReadRequest>& requests,
      std::unordered_set<std::uint32_t>& queued_pages);
  void flush_prefetch_requests(
      std::vector<AsyncPageReader::ReadRequest>& requests);
  void submit_prefetch_plan(const std::vector<std::uint32_t>& page_ids);
  void submit_prefetches();
  const Page* take_ready_prefetch_page(std::uint32_t page_id);
  void mark_prefetch_used(std::uint32_t page_id);

  PackedDiskGraphIndex& index_;
  DiskGraphSearchStats& stats_;
  PrefetchPlanner prefetch_planner_;
  bool async_prefetch_ = false;
  std::size_t prefetch_budget_pages_ = 0;
  std::size_t demand_io_reserve_pages_ = 1;
  std::size_t demand_inflight_pages_ = 0;
  bool finished_ = false;

  std::unordered_set<std::uint32_t> visited_pages_;
  std::unordered_set<std::uint32_t> pinned_pages_;
  std::unordered_map<std::uint32_t, Page> owned_pages_;
  std::unordered_set<std::uint32_t> materialized_pages_;
  std::unordered_map<std::uint32_t, Node> local_nodes_;
  std::unordered_map<std::uint32_t, Page> ready_prefetch_pages_;
  std::unordered_map<std::uint64_t, std::uint32_t> token_to_page_;
  std::unordered_set<std::uint32_t> pending_pages_;
  std::unordered_set<std::uint32_t> completed_prefetch_pages_;
  std::unordered_set<std::uint32_t> pending_hit_pages_;
};

}  // namespace agent_aware
