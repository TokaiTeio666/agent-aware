#include "core/query_page_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "core/async_page_reader.h"
#include "graph/disk_page_codec.h"

namespace agent_aware {
namespace {
std::atomic<std::uint64_t> next_query_id{1};

std::mutex& prefetch_trace_mutex() {
  static std::mutex mutex;
  return mutex;
}
}

QueryPageSession::QueryPageSession(PackedDiskGraphIndex& index,
                                   const DiskGraphSearchConfig& config,
                                   DiskGraphSearchStats& stats)
    : index_(index), stats_(stats) {
  async_prefetch_ = config.prefetch_policy == "topology" &&
                    index_.io_status().io_uring_enabled &&
                    index_.page_reader_->async_enabled();
  prefetch_adaptive_ = config.prefetch_adaptive;
  prefetch_top_k_ = config.prefetch_top_k;
  prefetch_max_inflight_ = config.prefetch_max_inflight;
  prefetch_query_limit_ = config.prefetch_max_pages_per_query;
  prefetch_trace_path_ = config.prefetch_trace_path;
  run_id_ = config.run_id;
  query_id_ = config.query_id == 0
                  ? next_query_id.fetch_add(1, std::memory_order_relaxed)
                  : config.query_id;
  const std::size_t page_words =
      (static_cast<std::size_t>(index_.metadata_.page_count) + 63) / 64;
  touched_page_bits_.assign(page_words, 0);
  demand_page_bits_.assign(page_words, 0);
}

QueryPageSession::~QueryPageSession() {
  try {
    finish_query();
  } catch (...) {
  }
}

void QueryPageSession::apply_workspace_limits(
    std::size_t prefetch_top_k, std::size_t prefetch_max_inflight) {
  prefetch_top_k_ = std::min(prefetch_top_k_, prefetch_top_k);
  prefetch_max_inflight_ =
      std::min(prefetch_max_inflight_, prefetch_max_inflight);
  if (prefetch_top_k_ == 0 || prefetch_max_inflight_ == 0) {
    async_prefetch_ = false;
  }
  constexpr std::size_t kMaxPrefetchPagesPerQuery = 64;
  const std::size_t limit = std::max<std::size_t>(
      prefetch_max_inflight_ * 4, prefetch_top_k_);
  prefetch_query_limit_ = prefetch_query_limit_ == 0
                              ? std::min(limit, kMaxPrefetchPagesPerQuery)
                              : std::min(prefetch_query_limit_,
                                         kMaxPrefetchPagesPerQuery);
}

QueryPageSession::PageHandle QueryPageSession::get_or_load_page(
    std::uint32_t page_id) {
  if (page_id >= index_.metadata_.page_count) {
    throw std::runtime_error("Packed page id out of range");
  }

  if (finished_) {
    auto page = index_.load_page(page_id, stats_);
    remember_page(page);
    return page;
  }

  mark_unique_page(touched_page_bits_, page_id, pages_touched_);
  ++stats_.page_requests;
  ++stats_.page_requests_before_dedup;

  const auto resident = resident_pages_.find(page_id);
  if (resident != resident_pages_.end()) {
    ++stats_.pinned_hits;
    return resident->second;
  }

  if (auto cached = try_get_cached_page(page_id)) {
    remember_page(cached);
    return cached;
  }

  ++stats_.page_cache_misses;
  ++stats_.demand_reads;
  ++stats_.node_reads;
  ++stats_.page_requests_after_dedup;
  mark_unique_page(demand_page_bits_, page_id, demand_pages_count_);
  Page page = read_demand_page(page_id);

  if (index_.cache_enabled()) {
    auto cached_page = index_.store_cached_page(std::move(page), stats_);
    remember_page(cached_page);
    return cached_page;
  }

  auto handle = std::make_shared<Page>(std::move(page));
  remember_page(handle);
  return handle;
}

QueryPageSession::PageHandle QueryPageSession::try_get_cached_page(
    std::uint32_t page_id) {
  return index_.lookup_cached_page(page_id, stats_);
}

const QueryPageSession::Node& QueryPageSession::get_node(
    std::uint32_t node_id, bool count_same_page_reuse, bool require_vector) {
  ++nodes_accessed_;
  const std::uint32_t page_id = page_for_node(node_id);
  if (count_same_page_reuse && resident_pages_.find(page_id) !=
                                  resident_pages_.end()) {
    ++stats_.same_page_node_reuse;
  }
  auto page = get_or_load_page(page_id);
  const auto disk_node = index_.find_node_in_page(*page, node_id);
  current_node_ = Node{};
  current_node_.id = node_id;
  current_node_.page = page;
  const std::uint32_t* neighbors = index_.neighbor_data(*page, disk_node);
  current_node_.neighbors.assign(neighbors,
                                 neighbors + disk_node.neighbor_count);
  if (require_vector) {
    const float* values = index_.vector_data(*page, disk_node);
    current_node_.vector.assign(values, values + index_.metadata_.dim);
  }
  resident_pages_.erase(page_id);
  materialized_pages_.erase(page_id);
  return current_node_;
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
    if (seen.insert(page_id).second && !page_ready_in_session(page_id) &&
        !page_in_cache(page_id)) {
      missing.push_back(page_id);
    }
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

  drain_ready_prefetches();

  stats_.page_requests += page_ids.size();
  stats_.page_requests_before_dedup += page_ids.size();
  stats_.page_dedup_requests += page_ids.size();

  std::vector<std::uint32_t> unique_pages;
  unique_pages.reserve(page_ids.size());
  std::unordered_set<std::uint32_t> seen;
  seen.reserve(page_ids.size());
  for (const auto page_id : page_ids) {
    if (page_id >= index_.metadata_.page_count) {
      throw std::runtime_error("Packed page id out of range");
    }
    mark_unique_page(touched_page_bits_, page_id, pages_touched_);
    if (seen.insert(page_id).second) {
      unique_pages.push_back(page_id);
    }
  }

  const std::size_t duplicate_count = page_ids.size() - unique_pages.size();
  if (duplicate_count > 0) {
    record_duplicate_skipped(duplicate_count);
  }

  std::vector<std::uint32_t> missing_pages;
  missing_pages.reserve(unique_pages.size());
  for (const auto page_id : unique_pages) {
    if (materialize_available_page(page_id)) {
      continue;
    }
    if (pending_prefetch_pages_.find(page_id) !=
        pending_prefetch_pages_.end()) {
      ++stats_.prefetch_pending_hit;
      useful_prefetch_pages_.insert(page_id);
      const auto wait_start = std::chrono::steady_clock::now();
      if (!wait_for_prefetch_page(page_id)) {
        throw std::runtime_error(
            "Pending topology prefetch did not materialize its page");
      }
      if (!materialize_available_page(page_id)) {
        throw std::runtime_error(
            "Pending topology prefetch page could not be materialized");
      }
      const double wait_us = std::chrono::duration<double, std::micro>(
          std::chrono::steady_clock::now() - wait_start).count();
      stats_.demand_read_wait_us += wait_us;
      index_.prefetch_controller_.record_feedback(
          "post_expand", prefetch_adaptive_, "pending_hit", wait_us, stats_);
    } else {
      missing_pages.push_back(page_id);
    }
  }
  if (missing_pages.empty()) {
    return;
  }

  const std::size_t chunk_limit = index_.page_reader_->async_enabled()
                                      ? demand_batch_limit()
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
      ++stats_.page_cache_misses;
      ++stats_.demand_reads;
      ++stats_.node_reads;
      ++stats_.page_requests_after_dedup;
      mark_unique_page(demand_page_bits_, page_id, demand_pages_count_);
      requests.push_back(AsyncPageReader::ReadRequest{
          page_id, page_offset(page_id), index_.metadata_.page_size, false,
          query_id_});
    }
    if (requests.empty()) {
      continue;
    }

    const auto wait_start = std::chrono::steady_clock::now();
    const auto submitted = index_.page_reader_->batch_submit(requests, stats_);
    std::unordered_map<std::uint64_t, std::uint32_t> submitted_pages;
    submitted_pages.reserve(submitted.size());
    for (const auto& read : submitted) {
      submitted_pages.emplace(read.token, read.page_id);
    }

    while (!submitted_pages.empty()) {
      AsyncPageReader::CompletedRead completed;
      if (!index_.page_reader_->reap_async_read(query_id_, true, completed,
                                                stats_)) {
        throw std::runtime_error("Failed to wait for packed graph page batch");
      }
      const auto token = submitted_pages.find(completed.token);
      if (token == submitted_pages.end()) {
        const auto prefetch = prefetch_tokens_.find(completed.token);
        if (prefetch == prefetch_tokens_.end()) {
          throw std::runtime_error("Packed graph reader returned unknown token");
        }
        const std::uint32_t prefetch_page = prefetch->second;
        prefetch_tokens_.erase(prefetch);
        pending_prefetch_pages_.erase(prefetch_page);
        store_prefetched_page(
            prefetch_page,
            index_.decode_page(prefetch_page, std::move(completed.data)));
        continue;
      }
      const std::uint32_t page_id = token->second;
      submitted_pages.erase(token);
      Page decoded = index_.decode_page(page_id, std::move(completed.data));
      if (index_.cache_enabled()) {
        auto cached_page =
            index_.store_cached_page(std::move(decoded), stats_);
        remember_page(cached_page);
      } else {
        remember_page(std::make_shared<Page>(std::move(decoded)));
      }
    }
    stats_.demand_read_wait_us +=
        std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - wait_start)
            .count();
  }
}

std::size_t QueryPageSession::submit_topology_prefetch(
    const std::vector<std::uint32_t>& page_ids) {
  if (!async_prefetch_ || page_ids.empty() || prefetch_top_k_ == 0) return 0;
  drain_ready_prefetches();
  const auto decision = index_.prefetch_controller_.decide(
      "post_expand", prefetch_adaptive_, prefetch_top_k_, stats_);
  if (decision.paused || decision.top_k == 0) return 0;

  const std::size_t reader_capacity = index_.page_reader_->max_pending_reads();
  const std::size_t reader_pending = index_.page_reader_->pending_async_reads();
  const std::size_t inflight_room = prefetch_max_inflight_ > pending_prefetch_pages_.size()
      ? prefetch_max_inflight_ - pending_prefetch_pages_.size() : 0;
  const std::size_t reader_room = reader_capacity > reader_pending
      ? reader_capacity - reader_pending : 0;
  const std::size_t query_room = prefetch_query_limit_ == 0
      ? decision.top_k
      : (prefetch_query_limit_ > prefetch_query_submitted_
             ? prefetch_query_limit_ - prefetch_query_submitted_ : 0);
  const std::size_t limit = std::min({decision.top_k, inflight_room,
                                      reader_room, query_room});
  if (limit == 0) {
    ++stats_.demand_priority_blocks_prefetch;
    ++stats_.prefetch_skip_budget;
    return 0;
  }

  std::vector<AsyncPageReader::ReadRequest> requests;
  requests.reserve(limit);
  for (const auto page_id : page_ids) {
    if (requests.size() >= limit) break;
    if (page_id >= index_.metadata_.page_count) continue;
    if (page_ready_in_session(page_id) || page_in_cache(page_id)) {
      ++stats_.prefetch_skip_cached;
      continue;
    }
    if (pending_prefetch_pages_.find(page_id) != pending_prefetch_pages_.end()) {
      ++stats_.prefetch_skip_pending;
      continue;
    }
    if (!submitted_prefetch_pages_.insert(page_id).second) {
      ++stats_.prefetch_skip_seen;
      continue;
    }
    requests.push_back(AsyncPageReader::ReadRequest{
        page_id, page_offset(page_id), index_.metadata_.page_size, true,
        query_id_});
  }
  if (requests.empty()) return 0;

  const auto submitted = index_.page_reader_->batch_submit(requests, stats_);
  if (!submitted.empty()) ++stats_.prefetch_submit_batches;
  for (const auto& read : submitted) {
    prefetch_tokens_.emplace(read.token, read.page_id);
    pending_prefetch_pages_.insert(read.page_id);
    ++stats_.prefetch_submitted;
    ++prefetch_query_submitted_;
  }
  return submitted.size();
}

void QueryPageSession::drain_ready_prefetches() {
  while (consume_prefetch_completion(false)) {
  }
}

void QueryPageSession::record_topology_candidates(
    const std::vector<TopologyPageCandidate>& candidates) {
  if (prefetch_trace_path_.empty()) return;
  topology_trace_candidates_.insert(topology_trace_candidates_.end(),
                                    candidates.begin(), candidates.end());
}

bool QueryPageSession::is_node_materialized(std::uint32_t node_id) const {
  return resident_pages_.find(page_for_node(node_id)) != resident_pages_.end();
}

std::uint32_t QueryPageSession::page_for_node(std::uint32_t node_id) const {
  if (node_id >= index_.node_to_page_.size()) {
    throw std::runtime_error("Packed graph node id out of range");
  }
  return index_.node_to_page_[node_id];
}

bool QueryPageSession::page_available(std::uint32_t page_id) const {
  return page_ready_in_session(page_id) || page_in_cache(page_id) ||
         ready_prefetch_pages_.find(page_id) != ready_prefetch_pages_.end() ||
         pending_prefetch_pages_.find(page_id) != pending_prefetch_pages_.end();
}

bool QueryPageSession::pin_page(std::uint32_t page_id) {
  if (pinned_pages_.find(page_id) != pinned_pages_.end() ||
      !index_.pin_if_cached(page_id)) {
    return false;
  }
  pinned_pages_.insert(page_id);
  return true;
}

void QueryPageSession::unpin_page(std::uint32_t page_id) {
  const auto found = pinned_pages_.find(page_id);
  if (found == pinned_pages_.end()) return;
  index_.unpin_if_cached(page_id);
  pinned_pages_.erase(found);
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
  while (!prefetch_tokens_.empty()) {
    if (!consume_prefetch_completion(true)) break;
  }
  for (const auto page_id : submitted_prefetch_pages_) {
    if (useful_prefetch_pages_.find(page_id) == useful_prefetch_pages_.end()) {
      ++stats_.prefetch_wasted_pages;
      index_.prefetch_controller_.record_feedback(
          "post_expand", prefetch_adaptive_, "wasted", 0.0, stats_);
    }
  }
  stats_.prefetch_useful_pages = useful_prefetch_pages_.size();
  stats_.unique_prefetch_pages = submitted_prefetch_pages_.size();
  stats_.unique_pages_touched = pages_touched_;
  stats_.unique_nodes_accessed = nodes_accessed_;
  stats_.unique_demand_pages = demand_pages_count_;
  if (!prefetch_trace_path_.empty()) {
    std::lock_guard<std::mutex> lock(prefetch_trace_mutex());
    const std::filesystem::path path(prefetch_trace_path_);
    if (!path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path());
    }
    const bool write_header = !std::filesystem::exists(path) ||
                              std::filesystem::file_size(path) == 0;
    std::ofstream trace(path, std::ios::app);
    if (!trace) throw std::runtime_error("Cannot open prefetch trace: " + prefetch_trace_path_);
    if (write_header) {
      trace << "run_id,query_id,trigger,page_id,min_pq_rank,candidate_count,selected,useful,wasted\n";
    }
    for (const auto& candidate : topology_trace_candidates_) {
      const bool selected = submitted_prefetch_pages_.find(candidate.page_id) !=
                            submitted_prefetch_pages_.end();
      const bool useful = useful_prefetch_pages_.find(candidate.page_id) !=
                          useful_prefetch_pages_.end();
      trace << run_id_ << ',' << query_id_ << ",post_expand,"
            << candidate.page_id << ',' << candidate.min_pq_rank << ','
            << candidate.candidate_count << ',' << selected << ',' << useful
            << ',' << (selected && !useful) << '\n';
    }
  }
  unpin_all();
  finished_ = true;
}

std::uint64_t QueryPageSession::page_offset(std::uint32_t page_id) const {
  return index_.metadata_.records_offset +
         static_cast<std::uint64_t>(page_id) * index_.metadata_.page_size;
}

std::size_t QueryPageSession::demand_batch_limit() const {
  const DiskGraphIoStatus& status = index_.io_status();
  if (!status.io_uring_enabled || status.depth == 0) {
    return 1;
  }
  const std::size_t io_depth = std::max<std::size_t>(1, status.depth);
  const std::size_t io_batch = std::max<std::size_t>(1, status.batch_size);
  const std::size_t depth_limit = std::max<std::size_t>(1, io_depth / 2);
  return std::min(io_batch, depth_limit);
}

bool QueryPageSession::page_in_cache(std::uint32_t page_id) const {
  if (!index_.cache_enabled()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(index_.cache_mutex_);
  return index_.page_cache_.find(
             GraphPageIdentity{0, 0, page_id}) !=
         index_.page_cache_.end();
}

void QueryPageSession::record_duplicate_skipped(std::size_t count) {
  stats_.page_dedup_hits += count;
  stats_.duplicate_pages_eliminated += count;
  stats_.duplicate_skipped += count;
}

bool QueryPageSession::page_ready_in_session(std::uint32_t page_id) const {
  return materialized_pages_.find(page_id) != materialized_pages_.end() ||
         resident_pages_.find(page_id) != resident_pages_.end();
}

bool QueryPageSession::mark_unique_page(std::vector<std::uint64_t>& bits,
                                         std::uint32_t page_id,
                                         std::size_t& count) {
  const std::size_t word = page_id >> 6;
  const std::uint64_t mask = std::uint64_t{1} << (page_id & 63);
  if ((bits[word] & mask) != 0) return false;
  bits[word] |= mask;
  ++count;
  return true;
}

bool QueryPageSession::materialize_available_page(std::uint32_t page_id) {
  const auto ready = ready_prefetch_pages_.find(page_id);
  if (ready != ready_prefetch_pages_.end()) {
    ++stats_.prefetch_ready_hit;
    useful_prefetch_pages_.insert(page_id);
    Page page = std::move(ready->second);
    ready_prefetch_pages_.erase(ready);
    store_prefetched_page(page_id, std::move(page));
    index_.prefetch_controller_.record_feedback(
        "post_expand", prefetch_adaptive_, "ready_hit", 0.0, stats_);
  }
  const auto resident = resident_pages_.find(page_id);
  if (resident != resident_pages_.end()) {
    ++stats_.pinned_hits;
    return true;
  }
  if (materialized_pages_.find(page_id) != materialized_pages_.end()) {
    ++stats_.pinned_hits;
    return true;
  }
  if (auto cached = try_get_cached_page(page_id)) {
    remember_page(cached);
    return true;
  }
  return false;
}

void QueryPageSession::remember_page(PageHandle page) {
  materialized_pages_.insert(page->page_id);
  resident_pages_.insert_or_assign(page->page_id, std::move(page));
}

QueryPageSession::Page QueryPageSession::read_demand_page(
    std::uint32_t page_id) {
  const auto wait_start = std::chrono::steady_clock::now();
  if (!index_.page_reader_->async_enabled()) {
    Page page = index_.read_page(page_id, stats_);
    stats_.demand_read_wait_us +=
        std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - wait_start)
            .count();
    return page;
  }
  const auto submitted = index_.page_reader_->batch_submit(
      {AsyncPageReader::ReadRequest{page_id, page_offset(page_id),
                                    index_.metadata_.page_size, false,
                                    query_id_}},
      stats_);
  if (submitted.size() != 1) {
    throw std::runtime_error("Failed to submit packed graph demand page");
  }
  AsyncPageReader::CompletedRead completed;
  while (true) {
    if (!index_.page_reader_->reap_async_read(query_id_, true, completed,
                                              stats_)) {
      throw std::runtime_error("Failed to complete packed graph demand page");
    }
    if (completed.token == submitted.front().token) break;
    const auto prefetch = prefetch_tokens_.find(completed.token);
    if (prefetch == prefetch_tokens_.end()) {
      throw std::runtime_error("Packed graph reader returned unknown token");
    }
    const std::uint32_t prefetch_page = prefetch->second;
    prefetch_tokens_.erase(prefetch);
    pending_prefetch_pages_.erase(prefetch_page);
    store_prefetched_page(
        prefetch_page,
        index_.decode_page(prefetch_page, std::move(completed.data)));
  }
  stats_.demand_read_wait_us +=
      std::chrono::duration<double, std::micro>(
          std::chrono::steady_clock::now() - wait_start)
          .count();
  return index_.decode_page(page_id, std::move(completed.data));
}

bool QueryPageSession::consume_prefetch_completion(bool wait) {
  if (prefetch_tokens_.empty()) return false;
  AsyncPageReader::CompletedRead completed;
  if (!index_.page_reader_->reap_async_read(query_id_, wait, completed,
                                            stats_)) {
    return false;
  }
  const auto found = prefetch_tokens_.find(completed.token);
  if (found == prefetch_tokens_.end()) {
    throw std::runtime_error("Unexpected demand completion while harvesting prefetch");
  }
  const std::uint32_t page_id = found->second;
  prefetch_tokens_.erase(found);
  pending_prefetch_pages_.erase(page_id);
  store_prefetched_page(
      page_id, index_.decode_page(page_id, std::move(completed.data)));
  return true;
}

bool QueryPageSession::wait_for_prefetch_page(std::uint32_t page_id) {
  while (pending_prefetch_pages_.find(page_id) != pending_prefetch_pages_.end()) {
    if (!consume_prefetch_completion(true)) return false;
  }
  return ready_prefetch_pages_.find(page_id) != ready_prefetch_pages_.end() ||
         page_ready_in_session(page_id) || page_in_cache(page_id);
}

void QueryPageSession::store_prefetched_page(std::uint32_t page_id, Page page) {
  if (index_.cache_enabled()) {
    auto cached = index_.store_cached_page(std::move(page), stats_);
    if (useful_prefetch_pages_.find(page_id) != useful_prefetch_pages_.end()) {
      remember_page(cached);
    }
  } else {
    if (useful_prefetch_pages_.find(page_id) != useful_prefetch_pages_.end()) {
      remember_page(std::make_shared<Page>(std::move(page)));
    }
  }
}

}  // namespace agent_aware
