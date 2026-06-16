#include "agentmem/core/query_page_session.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "agentmem/core/async_page_reader.h"

namespace agentmem {
namespace {

bool session_async_prefetch_enabled(const PackedDiskGraphIndex& index,
                                    const DiskGraphSearchConfig& config) {
  const DiskGraphIoStatus& status = index.io_status();
  return status.io_uring_enabled && status.depth > 0 &&
         config.prefetch_policy != "none";
}

std::size_t session_prefetch_budget(const PackedDiskGraphIndex& index,
                                    const DiskGraphSearchConfig& config) {
  if (!session_async_prefetch_enabled(index, config)) {
    return 0;
  }
  return std::max<std::size_t>(1, index.io_status().depth);
}

PrefetchPlanner::Config session_prefetch_config(
    const PackedDiskGraphIndex& index, const DiskGraphSearchConfig& config) {
  return PrefetchPlanner::Config{config.prefetch_policy,
                                 config.prefetch_width,
                                 config.prefetch_depth,
                                 session_prefetch_budget(index, config),
                                 config.page_dedup,
                                 config.page_coalesce};
}

}  // namespace

QueryPageSession::QueryPageSession(PackedDiskGraphIndex& index,
                                   const DiskGraphSearchConfig& config,
                                   DiskGraphSearchStats& stats)
    : index_(index),
      stats_(stats),
      prefetch_planner_(session_prefetch_config(index, config)),
      async_prefetch_(session_async_prefetch_enabled(index, config)),
      prefetch_budget_pages_(session_prefetch_budget(index, config)) {}

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

bool QueryPageSession::next_hop_prefetch_enabled() const {
  return async_prefetch_ && prefetch_planner_.next_hop_enabled();
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

  const auto owned = owned_pages_.find(page_id);
  if (owned != owned_pages_.end()) {
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

  if (pending_pages_.find(page_id) == pending_pages_.end() &&
      async_prefetch_) {
    while (async_page_footprint() >= prefetch_budget_pages_ &&
           !pending_pages_.empty()) {
      (void)harvest_prefetch(true);
    }
    (void)prefetch_page(page_id);
    submit_prefetches();
  }

  if (pending_pages_.find(page_id) != pending_pages_.end()) {
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

  while (async_prefetch_ && !pending_pages_.empty()) {
    (void)harvest_prefetch(true);
  }

  ++stats_.page_cache_misses;
  ++stats_.node_reads;
  ++stats_.page_requests_after_dedup;
  Page page = index_.read_page(page_id, stats_);
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

  const auto plan = prefetch_planner_.plan_frontier(
      page_ids, [this](std::uint32_t page_id) {
        return page_availability(page_id);
      });
  apply_prefetch_plan_stats(plan.stats);
  for (const auto page_id : plan.pages) {
    if (async_page_footprint() >= prefetch_budget_pages_) {
      break;
    }
    (void)prefetch_page(page_id);
  }
  submit_prefetches();
}

void QueryPageSession::submit_next_hop_prefetch(
    const std::vector<std::uint32_t>& node_ids,
    const std::unordered_set<std::uint32_t>& visited_nodes) {
  if (!next_hop_prefetch_enabled() || node_ids.empty()) {
    return;
  }

  const auto plan = prefetch_planner_.plan_next_hop(
      node_ids, index_.metadata_.vector_count,
      [this](std::uint32_t node_id) { return page_for_node(node_id); },
      [this, &visited_nodes](std::uint32_t node_id) {
        return visited_nodes.find(node_id) != visited_nodes.end() ||
               is_node_materialized(node_id);
      },
      [this](std::uint32_t page_id) {
        return page_availability(page_id);
      });
  apply_prefetch_plan_stats(plan.stats);
  for (const auto page_id : plan.pages) {
    if (async_page_footprint() >= prefetch_budget_pages_) {
      break;
    }
    (void)prefetch_page(page_id);
  }
  submit_prefetches();
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
    while (async_prefetch_ && !pending_pages_.empty()) {
      (void)harvest_prefetch(true);
    }
  } catch (...) {
    unpin_all();
    finished_ = true;
    throw;
  }

  stats_.prefetch_wasted_pages =
      stats_.prefetch_submitted_pages > stats_.prefetch_useful_pages
          ? stats_.prefetch_submitted_pages - stats_.prefetch_useful_pages
          : 0;
  stats_.p4_io.prefetch_dropped = stats_.prefetch_wasted_pages;
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

PrefetchPlanner::PageAvailability QueryPageSession::page_availability(
    std::uint32_t page_id) const {
  return PrefetchPlanner::PageAvailability{
      page_in_cache(page_id),
      ready_prefetch_pages_.find(page_id) != ready_prefetch_pages_.end() ||
          owned_pages_.find(page_id) != owned_pages_.end(),
      pending_pages_.find(page_id) != pending_pages_.end()};
}

void QueryPageSession::apply_prefetch_plan_stats(
    const PrefetchPlanner::PlanStats& stats) {
  stats_.page_requests_before_dedup += stats.page_requests_before_dedup;
  stats_.page_dedup_requests += stats.dedup_requests;
  stats_.page_dedup_hits += stats.dedup_hits;
  stats_.p4_io.dedup_hits += stats.dedup_hits;
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
    const auto token_found = token_to_page_.find(completed.token);
    if (token_found == token_to_page_.end()) {
      throw std::runtime_error("Packed graph async page token is unknown");
    }
    const std::uint32_t page_id = token_found->second;
    token_to_page_.erase(token_found);
    pending_pages_.erase(page_id);

    Page decoded = index_.decode_page(page_id, std::move(completed.data));
    completed_prefetch_pages_.insert(page_id);
    if (index_.cache_enabled()) {
      (void)index_.store_cached_page(std::move(decoded), stats_);
    } else if (ready_prefetch_pages_.size() < prefetch_budget_pages_) {
      ready_prefetch_pages_.emplace(page_id, std::move(decoded));
    }
    update_pending_peak();
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

bool QueryPageSession::prefetch_page(std::uint32_t page_id) {
  if (!async_prefetch_ || page_id >= index_.metadata_.page_count) {
    return false;
  }

  if (async_page_footprint() >= prefetch_budget_pages_) {
    (void)harvest_prefetch(false);
  }
  if (async_page_footprint() >= prefetch_budget_pages_) {
    return false;
  }

  const std::uint64_t token = index_.page_reader_->start_async_read(
      page_offset(page_id), index_.metadata_.page_size, stats_);
  token_to_page_[token] = page_id;
  pending_pages_.insert(page_id);
  visited_pages_.insert(page_id);
  ++stats_.node_reads;
  ++stats_.page_requests_after_dedup;
  ++stats_.io_prefetches;
  ++stats_.prefetch_submitted_pages;
  ++stats_.p4_io.prefetch_issued;
  update_pending_peak();
  return true;
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

  ++stats_.page_cache_hits;
  mark_prefetch_used(page_id);

  Page page = std::move(ready->second);
  ready_prefetch_pages_.erase(ready);
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

}  // namespace agentmem
