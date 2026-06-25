#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
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
  void submit_candidate_prefetch(
      const std::vector<std::uint32_t>& page_ids,
      const std::string& trigger = "candidate");
  std::size_t submit_jit_prefetch(
      const std::vector<std::uint32_t>& page_ids,
      const std::string& trigger = "pre_beam",
      std::size_t min_candidates_per_page = 0);
  // 直接提交页面预取，绕过 XGBoost planner（Agent-Mem-IO 风格）
  void submit_pages_direct(const std::vector<std::uint32_t>& page_ids);
  bool has_prefetch_model() const { return prefetch_planner_.has_model(); }
  void drain_ready_pages();

  bool page_available_for_scheduling(std::uint32_t page_id) const;
  bool pin_page(std::uint32_t page_id);
  void unpin_all();
  void finish_query();

 private:
  std::uint64_t page_offset(std::uint32_t page_id) const;
  std::size_t async_page_footprint() const;
  std::size_t demand_io_reserve() const;
  void update_pending_peak();
  bool page_in_cache(std::uint32_t page_id) const;
  PrefetchPlanner::PlanningContext prefetch_context() const;
  PrefetchPlanner::PageAvailability page_availability(
      std::uint32_t page_id) const;
  void apply_prefetch_plan_stats(const PrefetchPlanner::PlanStats& stats);
  void record_duplicate_skipped(std::size_t count = 1);
  void record_prefetch_plan(const PrefetchPlanner::Plan& plan,
                            const std::string& trigger);
  void record_prefetch_submit(std::uint32_t page_id);
  void record_prefetch_rejected(std::uint32_t page_id,
                                const std::string& reason);
  void record_prefetch_ready(std::uint32_t page_id);
  void record_prefetch_demand(std::uint32_t page_id);
  void record_prefetch_dropped(std::uint32_t page_id);
  void write_prefetch_trace();
  std::int64_t trace_elapsed_us() const;

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
  std::size_t search_width_ = 0;
  std::size_t beam_width_ = 0;
  std::uint64_t query_id_ = 0;
  std::size_t prefetch_step_ = 0;
  std::string prefetch_trace_path_;
  std::chrono::steady_clock::time_point trace_start_;
  bool finished_ = false;

  struct PrefetchTraceRecord {
    std::uint64_t query_id = 0;
    std::size_t step_id = 0;
    std::size_t demand_step_id = 0;
    std::string trigger;
    std::uint32_t page_id = 0;
    PrefetchPlanner::PrefetchFeature feature;
    std::int64_t decision_time_us = -1;
    std::int64_t submit_time_us = -1;
    std::int64_t ready_time_us = -1;
    std::int64_t demand_time_us = -1;
    bool was_prefetched = false;
    bool was_demanded = false;
    bool was_evicted_before_use = false;
    bool sync_fallback_used = false;
  };

  std::unordered_set<std::uint32_t> visited_pages_;
  std::unordered_set<std::uint32_t> demand_pages_;
  std::unordered_set<std::uint32_t> prefetch_pages_;
  std::unordered_set<std::uint32_t> pinned_pages_;
  std::unordered_map<std::uint32_t, Page> owned_pages_;
  std::unordered_set<std::uint32_t> materialized_pages_;
  std::unordered_map<std::uint32_t, Node> local_nodes_;
  std::unordered_map<std::uint32_t, Page> ready_prefetch_pages_;
  std::unordered_map<std::uint64_t, std::uint32_t> token_to_page_;
  std::unordered_set<std::uint32_t> pending_pages_;
  std::unordered_set<std::uint32_t> completed_prefetch_pages_;
  std::unordered_set<std::uint32_t> pending_hit_pages_;
  std::vector<PrefetchTraceRecord> prefetch_trace_records_;
  std::unordered_map<std::uint32_t, std::vector<std::size_t>>
      prefetch_trace_by_page_;
};

}  // namespace agent_aware
