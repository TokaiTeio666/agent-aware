#include "agent_aware/core/query_page_session.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "agent_aware/core/async_page_reader.h"

namespace agent_aware {
namespace {

std::mutex& prefetch_trace_mutex() {
  static std::mutex mutex;
  return mutex;
}

bool session_async_prefetch_enabled(const PackedDiskGraphIndex& index,
                                    const DiskGraphSearchConfig& config) {
  const DiskGraphIoStatus& status = index.io_status();
  // Agent-Mem-IO 风格：policy=xgboost 表示启用预取，不依赖 width/top_k
  return status.io_uring_enabled && status.depth > 0 &&
         config.prefetch_depth > 0 &&
         config.prefetch_policy != "none";
}

std::size_t session_demand_io_reserve(const PackedDiskGraphIndex& index,
                                      const DiskGraphSearchConfig& config) {
  (void)config;
  const DiskGraphIoStatus& status = index.io_status();
  if (!status.io_uring_enabled || status.depth == 0) {
    return 1;
  }
  const std::size_t io_depth = std::max<std::size_t>(1, status.depth);
  if (io_depth <= 1) {
    return 1;
  }
  const std::size_t io_batch = std::max<std::size_t>(1, status.batch_size);
  const std::size_t reserve_cap = std::max<std::size_t>(1, io_depth / 2);
  return std::max<std::size_t>(
      1, std::min<std::size_t>(io_batch, reserve_cap));
}

std::size_t session_prefetch_budget(const PackedDiskGraphIndex& index,
                                    const DiskGraphSearchConfig& config) {
  if (!session_async_prefetch_enabled(index, config)) {
    return 0;
  }
  const std::size_t io_depth = std::max<std::size_t>(1, index.io_status().depth);
  const std::size_t demand_reserve =
      session_demand_io_reserve(index, config);
  if (io_depth <= demand_reserve) {
    return 0;
  }
  // Agent-Mem-IO 风格：width/top_k=0 时用 io_depth/4 作为默认预算
  std::size_t width = std::max(config.prefetch_width, config.prefetch_top_k);
  if (width == 0) {
    width = std::max<std::size_t>(1, io_depth / 4);
  }
  const std::size_t requested =
      width * std::max<std::size_t>(1, config.prefetch_depth);
  return std::min(requested, io_depth - demand_reserve);
}

PrefetchPlanner::Config session_prefetch_config(
    const PackedDiskGraphIndex& index, const DiskGraphSearchConfig& config) {
  (void)index;
  PrefetchPlanner::Config planner;
  planner.policy = config.prefetch_policy;
  planner.prefetch_width = config.prefetch_width;
  planner.prefetch_depth = config.prefetch_depth;
  planner.min_candidates_per_page =
      config.prefetch_min_candidates_per_page;
  planner.model_path = config.prefetch_model_path;
  planner.prefetch_top_k = config.prefetch_top_k;
  planner.score_threshold = config.prefetch_score_threshold;
  planner.max_prefetch_inflight = config.prefetch_max_inflight;
  return planner;
}

}  // namespace

QueryPageSession::QueryPageSession(PackedDiskGraphIndex& index,
                                   const DiskGraphSearchConfig& config,
                                   DiskGraphSearchStats& stats)
    : index_(index),
      stats_(stats),
      prefetch_planner_(session_prefetch_config(index, config)),
      async_prefetch_(session_async_prefetch_enabled(index, config)),
      prefetch_budget_pages_(session_prefetch_budget(index, config)),
      demand_io_reserve_pages_(session_demand_io_reserve(index, config)),
      search_width_(config.search_width),
      beam_width_(config.beam_width == 0 ? config.search_width
                                         : config.beam_width),
      query_id_(config.query_id),
      prefetch_trace_path_(config.prefetch_trace_path),
      trace_start_(std::chrono::steady_clock::now()) {}

QueryPageSession::~QueryPageSession() {
  try {
    finish_query();
  } catch (...) {
  }
}

bool QueryPageSession::async_prefetch_enabled() const {
  return async_prefetch_;
}

bool QueryPageSession::frontier_prefetch_enabled() const {
  return async_prefetch_ && prefetch_planner_.frontier_enabled();
}

bool QueryPageSession::candidate_prefetch_enabled() const {
  return frontier_prefetch_enabled();
}

std::size_t QueryPageSession::prefetch_width() const {
  return prefetch_planner_.width();
}

const QueryPageSession::Page& QueryPageSession::get_or_load_page(
    std::uint32_t page_id) {
  if (page_id >= index_.metadata_.page_count) {
    throw std::runtime_error("Packed page id out of range");
  }

  if (finished_) {
    const Page& page = index_.load_page(page_id, stats_);
    materialize_page(page);
    return page;
  }

  visited_pages_.insert(page_id);
  ++stats_.page_requests;
  ++stats_.page_requests_before_dedup;
  record_prefetch_demand(page_id);

  const auto owned = owned_pages_.find(page_id);
  if (owned != owned_pages_.end()) {
    ++stats_.pinned_hits;
    return owned->second;
  }

  if (const Page* cached = try_get_cached_page(page_id)) {
    materialize_page(*cached);
    return *cached;
  }

  if (const Page* ready = take_ready_prefetch_page(page_id)) {
    materialize_page(*ready);
    return *ready;
  }

  if (pending_pages_.find(page_id) != pending_pages_.end()) {
    ++stats_.pending_hits;
    ++stats_.prefetch_pending_hit;
    pending_hit_pages_.insert(page_id);
    wait_for_prefetch_page(page_id);
  }

  if (const Page* prefetched = try_get_cached_page(page_id)) {
    materialize_page(*prefetched);
    return *prefetched;
  }

  if (const Page* ready = take_ready_prefetch_page(page_id)) {
    materialize_page(*ready);
    return *ready;
  }

  ++stats_.page_cache_misses;
  ++stats_.demand_reads;
  ++stats_.node_reads;
  ++stats_.page_requests_after_dedup;
  demand_pages_.insert(page_id);
  Page page = read_demand_page(page_id);
  index_.record_hub_cache_access(index_.page_is_hub(page), false, stats_);

  if (index_.cache_enabled()) {
    const Page& cached_page = index_.store_cached_page(std::move(page), stats_);
    materialize_page(cached_page);
    return cached_page;
  }

  auto inserted = owned_pages_.insert_or_assign(page_id, std::move(page));
  materialize_page(inserted.first->second);
  return inserted.first->second;
}

const QueryPageSession::Page* QueryPageSession::try_get_cached_page(
    std::uint32_t page_id) {
  const Page* cached = index_.lookup_cached_page(page_id, stats_);
  if (cached != nullptr) {
    mark_prefetch_used(page_id);
  }
  return cached;
}

const QueryPageSession::Node& QueryPageSession::get_node(
    std::uint32_t node_id, bool count_same_page_reuse) {
  auto found = local_nodes_.find(node_id);
  if (found != local_nodes_.end()) {
    if (count_same_page_reuse) {
      ++stats_.same_page_node_reuse;
    }
    return found->second;
  }

  const std::uint32_t page_id = page_for_node(node_id);
  (void)get_or_load_page(page_id);

  found = local_nodes_.find(node_id);
  if (found == local_nodes_.end()) {
    throw std::runtime_error("Node is missing from packed page");
  }
  return found->second;
}

std::vector<std::uint32_t> QueryPageSession::collect_missing_pages(
    const std::vector<std::uint32_t>& page_ids) const {
  std::vector<std::uint32_t> missing;
  missing.reserve(page_ids.size());
  std::unordered_set<std::uint32_t> seen;
  seen.reserve(page_ids.size());

  for (const auto page_id : page_ids) {
    if (page_id >= index_.metadata_.page_count) {
      throw std::runtime_error("Packed page id out of range");
    }
    if (!seen.insert(page_id).second) {
      continue;
    }
    if (page_ready_in_session(page_id) || page_in_cache(page_id) ||
        pending_pages_.find(page_id) != pending_pages_.end()) {
      continue;
    }
    missing.push_back(page_id);
  }
  return missing;
}

void QueryPageSession::ensure_pages_loaded_batch(
    const std::vector<std::uint32_t>& page_ids) {
  if (page_ids.empty()) {
    return;
  }

  if (finished_) {
    for (const auto page_id : page_ids) {
      (void)get_or_load_page(page_id);
    }
    return;
  }

  for (const auto page_id : page_ids) {
    if (page_id >= index_.metadata_.page_count) {
      throw std::runtime_error("Packed page id out of range");
    }
  }

  stats_.page_requests += page_ids.size();
  stats_.page_requests_before_dedup += page_ids.size();
  stats_.page_dedup_requests += page_ids.size();

  std::vector<std::uint32_t> unique_pages;
  unique_pages.reserve(page_ids.size());
  std::unordered_set<std::uint32_t> seen;
  seen.reserve(page_ids.size());
  for (const auto page_id : page_ids) {
    visited_pages_.insert(page_id);
    if (seen.insert(page_id).second) {
      unique_pages.push_back(page_id);
    }
  }

  const std::size_t duplicate_count = page_ids.size() - unique_pages.size();
  if (duplicate_count > 0) {
    stats_.page_dedup_hits += duplicate_count;
    stats_.duplicate_pages_eliminated += duplicate_count;
    stats_.p4_io.dedup_hits += duplicate_count;
    stats_.duplicate_skipped += duplicate_count;
  }

  std::vector<std::uint32_t> missing_pages;
  missing_pages.reserve(unique_pages.size());
  for (const auto page_id : unique_pages) {
    record_prefetch_demand(page_id);
    if (materialize_available_page(page_id)) {
      continue;
    }
    if (pending_pages_.find(page_id) != pending_pages_.end()) {
      ++stats_.pending_hits;
      ++stats_.prefetch_pending_hit;
      pending_hit_pages_.insert(page_id);
      wait_for_prefetch_page(page_id);
      if (materialize_available_page(page_id)) {
        continue;
      }
    }
    missing_pages.push_back(page_id);
  }

  if (async_prefetch_) {
    while (harvest_prefetch(false)) {
    }
  }

  if (missing_pages.empty()) {
    return;
  }

  const std::size_t chunk_limit =
      index_.page_reader_->async_enabled()
          ? std::max<std::size_t>(1, demand_io_reserve())
          : missing_pages.size();

  for (std::size_t begin = 0; begin < missing_pages.size();
       begin += chunk_limit) {
    const std::size_t end =
        std::min(begin + chunk_limit, missing_pages.size());
    std::vector<AsyncPageReader::ReadRequest> requests;
    requests.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      const std::uint32_t page_id = missing_pages[i];
      if (materialize_available_page(page_id)) {
        record_duplicate_skipped();
        continue;
      }
      if (pending_pages_.find(page_id) != pending_pages_.end()) {
        ++stats_.pending_hits;
        ++stats_.prefetch_pending_hit;
        pending_hit_pages_.insert(page_id);
        wait_for_prefetch_page(page_id);
        if (materialize_available_page(page_id)) {
          record_duplicate_skipped();
          continue;
        }
      }
      ++stats_.page_cache_misses;
      ++stats_.demand_reads;
      ++stats_.node_reads;
      ++stats_.page_requests_after_dedup;
      demand_pages_.insert(page_id);
      requests.push_back(AsyncPageReader::ReadRequest{
          page_id, page_offset(page_id), index_.metadata_.page_size});
    }

    if (requests.empty()) {
      continue;
    }

    const auto submitted = index_.page_reader_->batch_submit(requests, stats_);
    std::unordered_map<std::uint64_t, std::uint32_t> submitted_pages;
    submitted_pages.reserve(submitted.size());
    for (const auto& read : submitted) {
      submitted_pages.emplace(read.token, read.page_id);
    }

    demand_inflight_pages_ += submitted_pages.size();
    std::size_t remaining = submitted_pages.size();
    while (remaining > 0) {
      AsyncPageReader::CompletedRead completed;
      if (!index_.page_reader_->reap_async_read(true, completed, stats_)) {
        throw std::runtime_error("Failed to wait for packed graph page batch");
      }

      const auto token = submitted_pages.find(completed.token);
      if (token == submitted_pages.end()) {
        (void)consume_prefetch_completion(std::move(completed), true);
        continue;
      }
      const std::uint32_t page_id = token->second;
      submitted_pages.erase(token);

      Page decoded = index_.decode_page(page_id, std::move(completed.data));
      index_.record_hub_cache_access(index_.page_is_hub(decoded), false,
                                     stats_);
      if (index_.cache_enabled()) {
        const Page& cached_page =
            index_.store_cached_page(std::move(decoded), stats_);
        materialize_page(cached_page);
      } else {
        auto inserted = owned_pages_.insert_or_assign(page_id,
                                                      std::move(decoded));
        materialize_page(inserted.first->second);
      }
      --remaining;
      if (demand_inflight_pages_ > 0) {
        --demand_inflight_pages_;
      }
    }
  }
}

bool QueryPageSession::is_node_materialized(std::uint32_t node_id) const {
  return local_nodes_.find(node_id) != local_nodes_.end();
}

std::uint32_t QueryPageSession::page_for_node(std::uint32_t node_id) const {
  if (node_id >= index_.node_to_page_.size()) {
    throw std::runtime_error("Packed graph node id out of range");
  }
  return index_.node_to_page_[node_id];
}

void QueryPageSession::submit_prefetch(
    const std::vector<std::uint32_t>& page_ids) {
  if (!frontier_prefetch_enabled() || page_ids.empty()) {
    return;
  }

  auto context = prefetch_context();
  context.search_step = prefetch_step_++;
  const auto plan = prefetch_planner_.plan_frontier(
      page_ids, [this](std::uint32_t page_id) {
        return page_availability(page_id);
      },
      context);
  record_prefetch_plan(plan, "manual");
  apply_prefetch_plan_stats(plan.stats);
  submit_prefetch_plan(plan.pages);
}

void QueryPageSession::submit_pages_direct(
    const std::vector<std::uint32_t>& page_ids) {
  // Agent-Mem-IO 风格：直接提交，不做 XGBoost 评分
  // 调用侧已用 PQ 距离排好序 + 去重 + 去已缓存
  if (!async_prefetch_ || page_ids.empty()) {
    return;
  }
  submit_prefetch_plan(page_ids);
}

void QueryPageSession::submit_candidate_prefetch(
    const std::vector<std::uint32_t>& page_ids,
    const std::string& trigger) {
  if (!frontier_prefetch_enabled() || page_ids.empty()) {
    return;
  }

  auto context = prefetch_context();
  context.search_step = prefetch_step_++;
  const auto plan = prefetch_planner_.plan_candidates(
      page_ids, [this](std::uint32_t page_id) {
        return page_availability(page_id);
      },
      0, true, context);
  record_prefetch_plan(plan, trigger);
  apply_prefetch_plan_stats(plan.stats);
  submit_prefetch_plan(plan.pages);
}

std::size_t QueryPageSession::submit_jit_prefetch(
    const std::vector<std::uint32_t>& page_ids,
    const std::string& trigger,
    std::size_t min_candidates_per_page) {
  if (!async_prefetch_ || page_ids.empty()) {
    return 0;
  }

  const std::size_t submitted_before = stats_.prefetch_submitted;
  auto context = prefetch_context();
  context.search_step = prefetch_step_++;
  context.min_candidates_per_page = min_candidates_per_page;
  const auto plan = prefetch_planner_.plan_candidates(
      page_ids, [this](std::uint32_t page_id) {
        return page_availability(page_id);
      },
      0, true, context);
  record_prefetch_plan(plan, trigger);
  apply_prefetch_plan_stats(plan.stats);
  submit_prefetch_plan(plan.pages);
  return stats_.prefetch_submitted - submitted_before;
}

bool QueryPageSession::page_available_for_scheduling(
    std::uint32_t page_id) const {
  return page_ready_in_session(page_id) || page_in_cache(page_id) ||
         pending_pages_.find(page_id) != pending_pages_.end();
}

void QueryPageSession::drain_ready_pages() {
  if (!async_prefetch_) {
    return;
  }
  submit_prefetches();
}

bool QueryPageSession::pin_page(std::uint32_t page_id) {
  if (pinned_pages_.find(page_id) != pinned_pages_.end()) {
    return false;
  }
  if (!index_.pin_if_cached(page_id)) {
    return false;
  }
  pinned_pages_.insert(page_id);
  ++stats_.page_cache_pins;
  return true;
}

void QueryPageSession::unpin_all() {
  for (const auto page_id : pinned_pages_) {
    index_.unpin_if_cached(page_id);
  }
  pinned_pages_.clear();
}

void QueryPageSession::finish_query() {
  if (finished_) {
    return;
  }

  try {
    while (async_prefetch_ && harvest_prefetch(false)) {
    }
  } catch (...) {
    unpin_all();
    finished_ = true;
    throw;
  }

  for (const auto page_id : pending_pages_) {
    record_prefetch_dropped(page_id);
  }
  for (const auto& entry : ready_prefetch_pages_) {
    record_prefetch_dropped(entry.first);
  }
  stats_.prefetch_dropped += pending_pages_.size();
  stats_.prefetch_dropped += ready_prefetch_pages_.size();
  pending_pages_.clear();
  token_to_page_.clear();
  ready_prefetch_pages_.clear();
  completed_prefetch_pages_.clear();
  pending_hit_pages_.clear();
  stats_.prefetch_wasted_pages = stats_.prefetch_dropped;
  stats_.p4_io.prefetch_dropped = stats_.prefetch_dropped;
  stats_.unique_pages_touched = visited_pages_.size();
  stats_.unique_demand_pages = demand_pages_.size();
  stats_.unique_prefetch_pages = prefetch_pages_.size();
  std::size_t prefetch_only = 0;
  for (const auto page_id : prefetch_pages_) {
    if (visited_pages_.find(page_id) == visited_pages_.end()) {
      ++prefetch_only;
    }
  }
  stats_.prefetch_only_pages = prefetch_only;
  write_prefetch_trace();
  unpin_all();
  finished_ = true;
}

std::uint64_t QueryPageSession::page_offset(std::uint32_t page_id) const {
  return index_.metadata_.records_offset +
         static_cast<std::uint64_t>(page_id) * index_.metadata_.page_size;
}

std::size_t QueryPageSession::async_page_footprint() const {
  return pending_pages_.size() + ready_prefetch_pages_.size();
}

std::size_t QueryPageSession::demand_io_reserve() const {
  return std::max<std::size_t>(1, demand_io_reserve_pages_);
}

void QueryPageSession::update_pending_peak() {
  stats_.io_pending_pages_peak =
      std::max(stats_.io_pending_pages_peak, async_page_footprint());
}

bool QueryPageSession::page_in_cache(std::uint32_t page_id) const {
  if (!index_.cache_enabled()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(index_.cache_mutex_);
  return index_.page_cache_.find(page_id) != index_.page_cache_.end();
}

PrefetchPlanner::PlanningContext QueryPageSession::prefetch_context() const {
  PrefetchPlanner::PlanningContext context;
  context.visited_count = visited_pages_.size();
  context.ef_search = search_width_;
  context.beam_width = beam_width_;
  context.io_queue_depth = async_page_footprint();
  context.max_prefetch_inflight = prefetch_budget_pages_;
  context.cache_pressure =
      prefetch_budget_pages_ == 0
          ? 0.0
          : static_cast<double>(async_page_footprint()) /
                static_cast<double>(prefetch_budget_pages_);
  return context;
}

PrefetchPlanner::PageAvailability QueryPageSession::page_availability(
    std::uint32_t page_id) const {
  return PrefetchPlanner::PageAvailability{
      page_in_cache(page_id),
      ready_prefetch_pages_.find(page_id) != ready_prefetch_pages_.end(),
      pending_pages_.find(page_id) != pending_pages_.end(),
      materialized_pages_.find(page_id) != materialized_pages_.end() ||
          owned_pages_.find(page_id) != owned_pages_.end()};
}

void QueryPageSession::apply_prefetch_plan_stats(
    const PrefetchPlanner::PlanStats& stats) {
  stats_.page_requests_before_dedup += stats.page_requests_before_dedup;
  stats_.page_dedup_requests += stats.dedup_requests;
  stats_.page_dedup_hits += stats.dedup_hits;
  stats_.p4_io.dedup_hits += stats.dedup_hits;
  stats_.duplicate_skipped += stats.dedup_hits;
  stats_.prefetch_skip_seen_before += stats.skip_seen_before;
  stats_.prefetch_skip_cached += stats.skip_cached;
  stats_.prefetch_skip_pending += stats.skip_pending;
  stats_.prefetch_skip_materialized += stats.skip_materialized;
  stats_.prefetch_skip_budget_full += stats.skip_budget_full;
  stats_.prefetch_skip_low_page_reuse += stats.skip_low_page_reuse;
  stats_.prefetch_skip_score_threshold += stats.skip_score_threshold;
  stats_.prefetch_skip_inflight_full += stats.skip_inflight_full;
}

void QueryPageSession::record_duplicate_skipped(std::size_t count) {
  stats_.page_dedup_hits += count;
  stats_.duplicate_pages_eliminated += count;
  stats_.duplicate_skipped += count;
  stats_.p4_io.dedup_hits += count;
}

void QueryPageSession::record_prefetch_plan(
    const PrefetchPlanner::Plan& plan, const std::string& trigger) {
  if (prefetch_trace_path_.empty() || plan.candidates.empty()) {
    return;
  }

  prefetch_trace_records_.reserve(prefetch_trace_records_.size() +
                                  plan.candidates.size());
  for (const auto& feature : plan.candidates) {
    PrefetchTraceRecord record;
    record.query_id = query_id_;
    record.step_id = feature.search_step;
    record.trigger = trigger;
    record.page_id = feature.page_id;
    record.feature = feature;
    record.decision_time_us = trace_elapsed_us();
    record.sync_fallback_used = !index_.io_status().io_uring_enabled;
    const std::size_t index = prefetch_trace_records_.size();
    prefetch_trace_records_.push_back(std::move(record));
    prefetch_trace_by_page_[feature.page_id].push_back(index);
  }
}

void QueryPageSession::record_prefetch_submit(std::uint32_t page_id) {
  if (prefetch_trace_path_.empty()) {
    return;
  }
  const auto found = prefetch_trace_by_page_.find(page_id);
  if (found == prefetch_trace_by_page_.end()) {
    return;
  }
  const std::int64_t now = trace_elapsed_us();
  for (const auto index : found->second) {
    PrefetchTraceRecord& record = prefetch_trace_records_[index];
    if (!record.feature.selected || record.was_prefetched) {
      continue;
    }
    record.was_prefetched = true;
    record.submit_time_us = now;
    return;
  }
}

void QueryPageSession::record_prefetch_rejected(
    std::uint32_t page_id, const std::string& reason) {
  if (prefetch_trace_path_.empty()) {
    return;
  }
  const auto found = prefetch_trace_by_page_.find(page_id);
  if (found == prefetch_trace_by_page_.end()) {
    return;
  }
  for (const auto index : found->second) {
    PrefetchTraceRecord& record = prefetch_trace_records_[index];
    if (!record.feature.selected || record.was_prefetched) {
      continue;
    }
    if (record.feature.skip_reason.empty()) {
      record.feature.skip_reason = reason;
    }
    return;
  }
}

void QueryPageSession::record_prefetch_ready(std::uint32_t page_id) {
  if (prefetch_trace_path_.empty()) {
    return;
  }
  const auto found = prefetch_trace_by_page_.find(page_id);
  if (found == prefetch_trace_by_page_.end()) {
    return;
  }
  const std::int64_t now = trace_elapsed_us();
  for (const auto index : found->second) {
    PrefetchTraceRecord& record = prefetch_trace_records_[index];
    if (record.was_prefetched && record.ready_time_us < 0) {
      record.ready_time_us = now;
    }
  }
}

void QueryPageSession::record_prefetch_demand(std::uint32_t page_id) {
  if (prefetch_trace_path_.empty()) {
    return;
  }
  const auto found = prefetch_trace_by_page_.find(page_id);
  if (found == prefetch_trace_by_page_.end()) {
    return;
  }
  const std::int64_t now = trace_elapsed_us();
  for (const auto index : found->second) {
    PrefetchTraceRecord& record = prefetch_trace_records_[index];
    if (!record.was_demanded) {
      record.was_demanded = true;
      record.demand_step_id = prefetch_step_;
      record.demand_time_us = now;
    }
  }
}

void QueryPageSession::record_prefetch_dropped(std::uint32_t page_id) {
  if (prefetch_trace_path_.empty()) {
    return;
  }
  const auto found = prefetch_trace_by_page_.find(page_id);
  if (found == prefetch_trace_by_page_.end()) {
    return;
  }
  for (const auto index : found->second) {
    PrefetchTraceRecord& record = prefetch_trace_records_[index];
    if (record.was_prefetched && !record.was_demanded &&
        !record.was_evicted_before_use) {
      record.was_evicted_before_use = true;
      ++stats_.prefetch_evicted_before_use;
    }
  }
}

void QueryPageSession::write_prefetch_trace() {
  if (prefetch_trace_path_.empty() || prefetch_trace_records_.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(prefetch_trace_mutex());
  const std::filesystem::path path(prefetch_trace_path_);
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  const bool write_header =
      !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
  std::ofstream output(path, std::ios::app);
  if (!output) {
    throw std::runtime_error("Cannot write prefetch trace: " +
                             path.string());
  }

  if (write_header) {
    output
        << "query_id,step_id,trigger,group_id,candidate_node_id,"
        << "candidate_page_id,"
        << "num_candidates_on_page,min_pq_rank_on_page,"
        << "avg_pq_rank_on_page,candidate_rank_span,"
        << "contains_top1_candidate,contains_topk_candidate,"
        << "is_cached_at_decision,is_inflight_at_decision,"
        << "io_queue_depth,cache_pressure,prefetch_rank,prefetch_score,"
        << "selected_by_policy,skip_reason,prefetch_decision_time,"
        << "was_prefetched,prefetch_submit_time,prefetch_ready_time,"
        << "demand_time,was_demanded,demand_step_id,lead_steps,"
        << "submit_to_demand_us,ready_before_demand_us,"
        << "was_ready_before_demand,"
        << "was_evicted_before_use,sync_fallback_used,label\n";
  }

  for (const auto& record : prefetch_trace_records_) {
    const bool ready_before_demand =
        record.was_prefetched && record.was_demanded &&
        record.ready_time_us >= 0 && record.demand_time_us >= 0 &&
        record.ready_time_us <= record.demand_time_us;
    const std::int64_t lead_steps =
        record.was_demanded
            ? static_cast<std::int64_t>(record.demand_step_id) -
                  static_cast<std::int64_t>(record.step_id)
            : -1;
    const std::int64_t submit_to_demand_us =
        record.was_prefetched && record.was_demanded &&
                record.submit_time_us >= 0 && record.demand_time_us >= 0
            ? record.demand_time_us - record.submit_time_us
            : -1;
    const std::int64_t ready_before_demand_us =
        record.was_prefetched && record.was_demanded &&
                record.ready_time_us >= 0 && record.demand_time_us >= 0
            ? record.demand_time_us - record.ready_time_us
            : -1;
    int label = 0;
    if (ready_before_demand) {
      label = 3;
    } else if (record.was_demanded && !record.was_prefetched) {
      label = 2;
    } else if (record.was_demanded && record.was_prefetched) {
      label = 1;
    }

    const auto& feature = record.feature;
    output << record.query_id << ',' << record.step_id << ','
           << record.trigger << ','
           << record.query_id << '_' << record.step_id << ','
           << ',' << record.page_id << ','
           << feature.num_candidates_on_page << ','
           << feature.min_pq_rank_on_page << ','
           << feature.avg_pq_rank_on_page << ','
           << feature.candidate_rank_span << ','
           << (feature.contains_top1_candidate ? 1 : 0) << ','
           << (feature.contains_topk_candidate ? 1 : 0) << ','
           << (feature.is_cached ? 1 : 0) << ','
           << (feature.is_inflight ? 1 : 0) << ','
           << feature.io_queue_depth << ',' << feature.cache_pressure << ','
           << feature.rank << ',' << feature.score << ','
           << (feature.selected ? 1 : 0) << ','
           << feature.skip_reason << ','
           << record.decision_time_us << ','
           << (record.was_prefetched ? 1 : 0) << ','
           << record.submit_time_us << ',' << record.ready_time_us << ','
           << record.demand_time_us << ','
           << (record.was_demanded ? 1 : 0) << ','
           << record.demand_step_id << ',' << lead_steps << ','
           << submit_to_demand_us << ',' << ready_before_demand_us << ','
           << (ready_before_demand ? 1 : 0) << ','
           << (record.was_evicted_before_use ? 1 : 0) << ','
           << (record.sync_fallback_used ? 1 : 0) << ',' << label
           << '\n';
  }
}

std::int64_t QueryPageSession::trace_elapsed_us() const {
  return static_cast<std::int64_t>(
      std::chrono::duration<double, std::micro>(
          std::chrono::steady_clock::now() - trace_start_)
          .count());
}

bool QueryPageSession::page_ready_in_session(std::uint32_t page_id) const {
  return materialized_pages_.find(page_id) != materialized_pages_.end() ||
         owned_pages_.find(page_id) != owned_pages_.end() ||
         ready_prefetch_pages_.find(page_id) != ready_prefetch_pages_.end();
}

bool QueryPageSession::materialize_available_page(std::uint32_t page_id) {
  const auto owned = owned_pages_.find(page_id);
  if (owned != owned_pages_.end()) {
    ++stats_.pinned_hits;
    materialize_page(owned->second);
    return true;
  }

  if (materialized_pages_.find(page_id) != materialized_pages_.end()) {
    ++stats_.pinned_hits;
    return true;
  }

  if (const Page* cached = try_get_cached_page(page_id)) {
    materialize_page(*cached);
    return true;
  }

  if (const Page* ready = take_ready_prefetch_page(page_id)) {
    materialize_page(*ready);
    return true;
  }

  return false;
}

void QueryPageSession::materialize_page(const Page& page) {
  if (!materialized_pages_.insert(page.page_id).second) {
    return;
  }

  for (const auto& node : page.nodes) {
    auto inserted = local_nodes_.emplace(node.id, node);
    Node& local_node = inserted.first->second;
    if (local_node.vector.empty()) {
      const float* values = index_.vector_data(page, node);
      local_node.vector.assign(values, values + index_.metadata_.dim);
    }
  }
}

QueryPageSession::Page QueryPageSession::read_demand_page(
    std::uint32_t page_id) {
  if (!index_.page_reader_->async_enabled()) {
    return index_.read_page(page_id, stats_);
  }

  const auto submitted = index_.page_reader_->batch_submit(
      {AsyncPageReader::ReadRequest{page_id, page_offset(page_id),
                                    index_.metadata_.page_size}},
      stats_);
  if (submitted.empty()) {
    throw std::runtime_error("Failed to submit packed graph demand page");
  }
  const std::uint64_t demand_token = submitted.front().token;
  ++demand_inflight_pages_;
  for (;;) {
    AsyncPageReader::CompletedRead completed;
    if (!index_.page_reader_->reap_async_read(true, completed, stats_)) {
      throw std::runtime_error("Failed to wait for packed graph demand page");
    }
    if (completed.token != demand_token) {
      (void)consume_prefetch_completion(std::move(completed), true);
      continue;
    }
    if (demand_inflight_pages_ > 0) {
      --demand_inflight_pages_;
    }
    return index_.decode_page(page_id, std::move(completed.data));
  }
}

bool QueryPageSession::consume_prefetch_completion(
    AsyncPageReader::CompletedRead completed, bool keep_ready) {
  const auto token_found = token_to_page_.find(completed.token);
  if (token_found == token_to_page_.end()) {
    return false;
  }
  const std::uint32_t page_id = token_found->second;
  token_to_page_.erase(token_found);
  pending_pages_.erase(page_id);
  ++stats_.prefetch_completed;

  if (page_ready_in_session(page_id) || page_in_cache(page_id)) {
    ++stats_.prefetch_late_hit;
    ++stats_.prefetch_dropped;
    record_prefetch_dropped(page_id);
    update_pending_peak();
    return true;
  }

  Page decoded = index_.decode_page(page_id, std::move(completed.data));
  completed_prefetch_pages_.insert(page_id);
  record_prefetch_ready(page_id);
  if (index_.cache_enabled()) {
    ++stats_.prefetch_cache_pollution_avoided;
  }
  if (keep_ready && ready_prefetch_pages_.size() < prefetch_budget_pages_) {
    ready_prefetch_pages_.emplace(page_id, std::move(decoded));
  } else {
    completed_prefetch_pages_.erase(page_id);
    ++stats_.prefetch_dropped;
    record_prefetch_dropped(page_id);
  }
  update_pending_peak();
  return true;
}

bool QueryPageSession::harvest_prefetch(bool wait) {
  if (!async_prefetch_) {
    return false;
  }
  index_.page_reader_->submit_async_reads(stats_);

  bool harvested = false;
  AsyncPageReader::CompletedRead completed;
  while (index_.page_reader_->reap_async_read(wait && !harvested, completed,
                                              stats_)) {
    harvested = true;
    (void)consume_prefetch_completion(std::move(completed), true);
    completed = AsyncPageReader::CompletedRead{};
  }
  return harvested;
}

void QueryPageSession::wait_for_prefetch_page(std::uint32_t page_id) {
  bool waited = false;
  const auto wait_start = std::chrono::steady_clock::now();
  while (pending_pages_.find(page_id) != pending_pages_.end()) {
    waited = true;
    ++stats_.io_prefetch_waits;
    if (!harvest_prefetch(true)) {
      throw std::runtime_error("Failed to wait for packed graph prefetch");
    }
  }
  if (waited) {
    const auto wait_end = std::chrono::steady_clock::now();
    ++stats_.demand_read_waits;
    stats_.demand_read_wait_us +=
        std::chrono::duration<double, std::micro>(wait_end - wait_start)
            .count();
  }
}

std::size_t QueryPageSession::prefetch_batch_limit() const {
  const std::size_t io_batch =
      std::max<std::size_t>(1, index_.io_status().batch_size);
  if (prefetch_budget_pages_ == 0) {
    return io_batch;
  }
  return std::max<std::size_t>(
      1, std::min<std::size_t>(io_batch, prefetch_budget_pages_));
}

bool QueryPageSession::queue_prefetch_request(
    std::uint32_t page_id,
    std::vector<AsyncPageReader::ReadRequest>& requests,
    std::unordered_set<std::uint32_t>& queued_pages) {
  if (!async_prefetch_ || page_id >= index_.metadata_.page_count) {
    record_prefetch_rejected(page_id, "submit_invalid");
    return false;
  }

  if (prefetch_budget_pages_ == 0 ||
      demand_inflight_pages_ >= demand_io_reserve()) {
    ++stats_.demand_priority_blocks_prefetch;
    record_prefetch_rejected(page_id, "demand_priority");
    return false;
  }

  if (!queued_pages.insert(page_id).second) {
    ++stats_.prefetch_skip_seen_before;
    record_duplicate_skipped();
    record_prefetch_rejected(page_id, "submit_duplicate");
    return false;
  }

  if (async_page_footprint() + requests.size() >= prefetch_budget_pages_) {
    (void)harvest_prefetch(false);
  }
  if (async_page_footprint() + requests.size() >= prefetch_budget_pages_) {
    ++stats_.prefetch_skip_budget_full;
    record_prefetch_rejected(page_id, "submit_budget_full");
    return false;
  }

  if (page_in_cache(page_id)) {
    ++stats_.prefetch_skip_cached;
    record_duplicate_skipped();
    record_prefetch_rejected(page_id, "submit_cached");
    return false;
  }
  if (pending_pages_.find(page_id) != pending_pages_.end()) {
    ++stats_.prefetch_skip_pending;
    record_duplicate_skipped();
    record_prefetch_rejected(page_id, "submit_pending");
    return false;
  }
  if (page_ready_in_session(page_id)) {
    ++stats_.prefetch_skip_materialized;
    record_duplicate_skipped();
    record_prefetch_rejected(page_id, "submit_materialized");
    return false;
  }

  requests.push_back(AsyncPageReader::ReadRequest{
      page_id, page_offset(page_id), index_.metadata_.page_size});
  return true;
}

void QueryPageSession::flush_prefetch_requests(
    std::vector<AsyncPageReader::ReadRequest>& requests) {
  if (requests.empty()) {
    return;
  }

  const auto submitted = index_.page_reader_->batch_submit(requests, stats_);
  for (const auto& read : submitted) {
    token_to_page_[read.token] = read.page_id;
    pending_pages_.insert(read.page_id);
    prefetch_pages_.insert(read.page_id);
    record_prefetch_submit(read.page_id);
    ++stats_.prefetch_reads;
    ++stats_.node_reads;
    ++stats_.io_prefetches;
    ++stats_.prefetch_submitted_pages;
    ++stats_.prefetch_submitted;
    ++stats_.p4_io.prefetch_issued;
  }
  update_pending_peak();
  requests.clear();
}

void QueryPageSession::submit_prefetch_plan(
    const std::vector<std::uint32_t>& page_ids) {
  if (!async_prefetch_ || page_ids.empty()) {
    return;
  }

  const std::size_t batch_limit = prefetch_batch_limit();
  std::vector<AsyncPageReader::ReadRequest> requests;
  requests.reserve(std::min(batch_limit, page_ids.size()));
  std::unordered_set<std::uint32_t> queued_pages;
  queued_pages.reserve(page_ids.size());

  for (const auto page_id : page_ids) {
    if (requests.size() >= batch_limit) {
      flush_prefetch_requests(requests);
    }
    (void)queue_prefetch_request(page_id, requests, queued_pages);
  }
  flush_prefetch_requests(requests);
  (void)harvest_prefetch(false);
}

void QueryPageSession::submit_prefetches() {
  if (!async_prefetch_) {
    return;
  }
  index_.page_reader_->submit_async_reads(stats_);
  (void)harvest_prefetch(false);
}

const QueryPageSession::Page* QueryPageSession::take_ready_prefetch_page(
    std::uint32_t page_id) {
  auto ready = ready_prefetch_pages_.find(page_id);
  if (ready == ready_prefetch_pages_.end()) {
    return nullptr;
  }

  const bool was_pending_hit = pending_hit_pages_.erase(page_id) > 0;
  if (!was_pending_hit) {
    ++stats_.prefetch_ready_hit;
  }
  mark_prefetch_used(page_id);
  ++stats_.prefetch_promoted;
  Page page = std::move(ready->second);
  ready_prefetch_pages_.erase(ready);
  if (index_.cache_enabled()) {
    const Page& cached = index_.store_cached_page(std::move(page), stats_);
    return &cached;
  }
  auto inserted = owned_pages_.insert_or_assign(page_id, std::move(page));
  return &inserted.first->second;
}

void QueryPageSession::mark_prefetch_used(std::uint32_t page_id) {
  if (completed_prefetch_pages_.erase(page_id) == 0) {
    return;
  }
  ++stats_.io_prefetch_hits;
  ++stats_.prefetch_useful_pages;
  ++stats_.p4_io.prefetch_used;
}

}  // namespace agent_aware
