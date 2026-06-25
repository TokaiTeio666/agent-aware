#include "agent_aware/graph/disk_graph_index.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "agent_aware/core/async_page_reader.h"
#include "agent_aware/core/brute_force.h"
#include "agent_aware/core/query_page_session.h"
#include "agent_aware/graph/disk_page_codec.h"
#include "agent_aware/graph/entry_selector.h"
#include "agent_aware/graph/vamana_builder.h"

namespace agent_aware {
namespace {

struct CloserFirst {
  bool operator()(const SearchResult& lhs, const SearchResult& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id > rhs.id;
    }
    return lhs.distance > rhs.distance;
  }
};

struct WorseResultFirst {
  bool operator()(const SearchResult& lhs, const SearchResult& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id < rhs.id;
    }
    return lhs.distance < rhs.distance;
  }
};

std::vector<SearchResult> sorted_results(
    std::priority_queue<SearchResult, std::vector<SearchResult>,
                        WorseResultFirst>& heap) {
  std::vector<SearchResult> results;
  results.reserve(heap.size());
  while (!heap.empty()) {
    results.push_back(heap.top());
    heap.pop();
  }
  std::sort(results.begin(), results.end(),
            [](const SearchResult& lhs, const SearchResult& rhs) {
              if (lhs.distance == rhs.distance) {
                return lhs.id < rhs.id;
              }
              return lhs.distance < rhs.distance;
            });
  return results;
}

bool pre_beam_prefetch_enabled(const DiskGraphSearchConfig& config) {
  return config.prefetch_policy == "xgboost" &&
         (config.prefetch_early_trigger == "pre-beam" ||
          config.prefetch_early_trigger == "all");
}

bool entry_warmup_prefetch_enabled(const DiskGraphSearchConfig& config) {
  return config.prefetch_policy == "xgboost" &&
         (config.prefetch_early_trigger == "entry-warmup" ||
          config.prefetch_early_trigger == "all");
}

bool post_expand_prefetch_enabled(const DiskGraphSearchConfig& config) {
  return config.prefetch_policy == "xgboost" &&
         (config.prefetch_early_trigger == "off" ||
          config.prefetch_early_trigger == "all");
}

bool rerank_prefetch_enabled(const DiskGraphSearchConfig& config) {
  return config.prefetch_policy == "xgboost" &&
         (config.prefetch_early_trigger == "rerank" ||
          config.prefetch_early_trigger == "all");
}

}  // namespace

struct PackedDiskGraphIndex::SearchState {
  SearchState(PackedDiskGraphIndex& index, const float* query,
              const DiskGraphSearchConfig& config,
              DiskGraphSearchResult& output)
      : index(index),
        query(query),
        config(config),
        output(output),
        session(index, config, output.stats) {}

  const DiskNode& load_node(std::uint32_t id) {
    return session.get_node(id, config.same_page_reuse);
  }

  float candidate_distance(std::uint32_t id) {
    if (adc_table.empty() || config.pq_model == nullptr) {
      throw std::runtime_error(
          "Packed graph search requires PQ+ADC candidate generation");
    }
    ++output.stats.pq_filter_accept_count;
    return config.pq_model->adc_distance(id, adc_table);
  }

  std::vector<SearchResult> rank_neighbors_by_distance(
      const std::vector<std::uint32_t>& node_ids) {
    std::vector<SearchResult> ranked;
    ranked.reserve(node_ids.size());
    for (const auto node_id : node_ids) {
      ranked.push_back(SearchResult{node_id, candidate_distance(node_id)});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const SearchResult& lhs, const SearchResult& rhs) {
                if (lhs.distance == rhs.distance) {
                  return lhs.id < rhs.id;
                }
                return lhs.distance < rhs.distance;
              });
    return ranked;
  }

  void add_best(const SearchResult& result) {
    if (best.size() < result_capacity) {
      best.push(result);
    } else {
      const auto& worst = best.top();
      if (result.distance < worst.distance ||
          (result.distance == worst.distance && result.id < worst.id)) {
        best.pop();
        best.push(result);
      }
    }
  }

  bool should_stop(const SearchResult& next) const {
    if (!config.early_stop || best.size() < result_capacity ||
        output.stats.expanded < config.early_stop_min_expansions) {
      return false;
    }
    const auto& worst = best.top();
    return next.distance > worst.distance;
  }

  void update_adaptive_stop() {
    if (!config.adaptive_early_stop || best.size() < result_capacity) {
      return;
    }
    const double current = best.top().distance;
    const double improvement =
        std::isfinite(previous_worst_distance)
            ? (previous_worst_distance - current) /
                  std::max(1.0, std::abs(previous_worst_distance))
            : std::numeric_limits<double>::infinity();
    if (improvement > config.early_stop_eps) {
      stagnant_expansions = 0;
    } else {
      ++stagnant_expansions;
    }
    previous_worst_distance = current;
  }

  PackedDiskGraphIndex& index;
  const float* query = nullptr;
  const DiskGraphSearchConfig& config;
  DiskGraphSearchResult& output;
  QueryPageSession session;
  std::unordered_set<std::uint32_t> visited;
  std::priority_queue<SearchResult, std::vector<SearchResult>, CloserFirst>
      candidates;
  std::priority_queue<SearchResult, std::vector<SearchResult>, WorseResultFirst>
      best;
  std::vector<std::uint32_t> entry_seed_ids;
  std::vector<float> adc_table;
  std::size_t effective_k = 0;
  std::size_t result_capacity = 0;
  std::size_t stagnant_expansions = 0;
  std::size_t jit_prefetch_submitted_pages = 0;
  double previous_worst_distance = std::numeric_limits<double>::infinity();
};

PackedDiskGraphIndex::PackedDiskGraphIndex(const std::string& path)
    : path_(path), input_(path, std::ios::binary) {
  if (!input_) {
    throw std::runtime_error("Cannot open packed graph index: " + path);
  }

  char magic[8] = {};
  input_.read(magic, sizeof(magic));
  if (!input_ || std::memcmp(magic, kPackedGraphMagic.data(),
                             kPackedGraphMagic.size()) != 0) {
    throw std::runtime_error("Invalid packed graph index magic: " + path);
  }

  const auto version = read_value<std::uint32_t>(input_);
  if (version != kPackedGraphVersion) {
    throw std::runtime_error("Unsupported packed graph index version");
  }

  metadata_.vector_count = read_value<std::uint64_t>(input_);
  metadata_.dim = read_value<std::uint32_t>(input_);
  metadata_.degree = read_value<std::uint32_t>(input_);
  metadata_.page_size = read_value<std::uint32_t>(input_);
  metadata_.records_offset = read_value<std::uint64_t>(input_);
  metadata_.directory_offset = read_value<std::uint64_t>(input_);
  metadata_.page_count = read_value<std::uint64_t>(input_);
  metadata_.nodes_per_page = read_value<std::uint32_t>(input_);
  metadata_.neighbor_pq_code_bytes = read_value<std::uint32_t>(input_);

  if (metadata_.vector_count == 0 || metadata_.dim == 0 ||
      metadata_.degree == 0 || metadata_.page_size == 0 ||
      metadata_.page_count == 0 || metadata_.nodes_per_page == 0) {
    throw std::runtime_error("Packed graph index metadata is invalid");
  }
  if (graph_record_bytes(metadata_.dim, metadata_.degree,
                         metadata_.neighbor_pq_code_bytes) >
      metadata_.page_size) {
    throw std::runtime_error("Packed graph node payload exceeds page size");
  }

  node_to_page_.resize(static_cast<std::size_t>(metadata_.vector_count));
  input_.clear();
  input_.seekg(static_cast<std::streamoff>(metadata_.directory_offset),
               std::ios::beg);
  input_.read(reinterpret_cast<char*>(node_to_page_.data()),
              static_cast<std::streamsize>(node_to_page_.size() *
                                           sizeof(std::uint32_t)));
  if (!input_) {
    throw std::runtime_error("Failed to read packed graph directory");
  }
  page_reader_ = std::make_unique<AsyncPageReader>(path_, metadata_.page_size);
}

PackedDiskGraphIndex::~PackedDiskGraphIndex() = default;

void PackedDiskGraphIndex::configure_cache(const std::string& policy,
                                           std::size_t capacity_pages,
                                           bool protect_hot_pages,
                                           std::size_t hot_degree_threshold) {
  if (policy != "none" && policy != "lru" && policy != "2q" &&
      policy != "graph-aware-2q" && policy != "agent") {
    throw std::runtime_error(
        "Cache policy must be none, lru, 2q, or graph-aware-2q");
  }
  if (policy == "graph-aware-2q" || policy == "agent" ||
      protect_hot_pages) {
    ensure_node_in_degrees();
  }
  std::lock_guard<std::mutex> lock(cache_mutex_);
  if (policy != cache_policy_ || capacity_pages != cache_capacity_pages_ ||
      protect_hot_pages != cache_protect_hot_pages_ ||
      hot_degree_threshold != cache_hot_degree_threshold_) {
    page_cache_.clear();
    cache_clock_ = 0;
  }
  cache_policy_ = policy;
  cache_capacity_pages_ = capacity_pages;
  cache_protect_hot_pages_ = protect_hot_pages;
  cache_hot_degree_threshold_ = hot_degree_threshold;
}

void PackedDiskGraphIndex::pin(std::uint32_t page_id) {
  (void)pin_if_cached(page_id);
}

void PackedDiskGraphIndex::unpin(std::uint32_t page_id) {
  unpin_if_cached(page_id);
}

bool PackedDiskGraphIndex::is_pinned(std::uint32_t page_id) const {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  const auto found = page_cache_.find(page_id);
  return found != page_cache_.end() && found->second.pin_count > 0;
}

PackedDiskGraphIndex::PagePinGuard::PagePinGuard(
    PackedDiskGraphIndex& index, std::uint32_t page_id)
    : index_(&index), page_id_(page_id), owns_pin_(index.pin_if_cached(page_id)) {
}

PackedDiskGraphIndex::PagePinGuard::PagePinGuard(
    PagePinGuard&& other) noexcept
    : index_(other.index_),
      page_id_(other.page_id_),
      owns_pin_(other.owns_pin_) {
  other.index_ = nullptr;
  other.owns_pin_ = false;
}

PackedDiskGraphIndex::PagePinGuard&
PackedDiskGraphIndex::PagePinGuard::operator=(PagePinGuard&& other) noexcept {
  if (this != &other) {
    release();
    index_ = other.index_;
    page_id_ = other.page_id_;
    owns_pin_ = other.owns_pin_;
    other.index_ = nullptr;
    other.owns_pin_ = false;
  }
  return *this;
}

PackedDiskGraphIndex::PagePinGuard::~PagePinGuard() {
  release();
}

void PackedDiskGraphIndex::PagePinGuard::release() {
  if (index_ != nullptr && owns_pin_) {
    index_->unpin_if_cached(page_id_);
  }
  index_ = nullptr;
  owns_pin_ = false;
}

bool PackedDiskGraphIndex::pin_if_cached(std::uint32_t page_id) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto found = page_cache_.find(page_id);
  if (found == page_cache_.end()) {
    return false;
  }
  ++found->second.pin_count;
  return true;
}

void PackedDiskGraphIndex::unpin_if_cached(std::uint32_t page_id) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto found = page_cache_.find(page_id);
  if (found == page_cache_.end()) {
    return;
  }
  if (found->second.pin_count == 0) {
    throw std::runtime_error("Packed graph page pin underflow");
  }
  --found->second.pin_count;
}

void PackedDiskGraphIndex::configure_io(const std::string& mode,
                                        std::size_t batch_size,
                                        std::size_t io_depth) {
  page_reader_->configure(mode, batch_size, io_depth);
}

const DiskGraphIoStatus& PackedDiskGraphIndex::io_status() const {
  return page_reader_->status();
}

const std::vector<std::uint32_t>& PackedDiskGraphIndex::node_in_degrees() {
  ensure_node_in_degrees();
  return node_in_degrees_;
}

PackedDiskGraphIndex::DecodedPage PackedDiskGraphIndex::read_page(
    std::uint32_t page_id, DiskGraphSearchStats& stats) {
  // 解码顺序必须与 PackedDiskGraphBuilder 写入的四个 SoA 区域严格一致。
  if (page_id >= metadata_.page_count) {
    throw std::runtime_error("Packed page id out of range");
  }

  const std::uint64_t offset =
      metadata_.records_offset +
      static_cast<std::uint64_t>(page_id) * metadata_.page_size;
  std::vector<char> page = page_reader_->read(offset, metadata_.page_size,
                                               stats);
  return decode_page(page_id, std::move(page));
}

PackedDiskGraphIndex::DecodedPage PackedDiskGraphIndex::decode_page(
    std::uint32_t page_id, std::vector<char> page) const {
  return DiskPageCodec::decode_packed_page(metadata_, page_id,
                                           std::move(page));
}

bool PackedDiskGraphIndex::cache_enabled() const {
  return cache_policy_ != "none" && cache_capacity_pages_ > 0;
}

void PackedDiskGraphIndex::ensure_node_in_degrees() {
  if (node_in_degrees_ready_) {
    return;
  }

  node_in_degrees_.assign(static_cast<std::size_t>(metadata_.vector_count), 0);
  DiskGraphSearchStats ignored;
  for (std::uint32_t page_id = 0; page_id < metadata_.page_count; ++page_id) {
    DecodedPage page = read_page(page_id, ignored);
    for (const auto& node : page.nodes) {
      for (const auto neighbor : node.neighbors) {
        if (neighbor < node_in_degrees_.size()) {
          ++node_in_degrees_[neighbor];
        }
      }
    }
  }
  node_in_degrees_ready_ = true;
}

bool PackedDiskGraphIndex::page_is_hub(const DecodedPage& page) const {
  return page_hot_score(page) > 0;
}

bool PackedDiskGraphIndex::cache_entry_is_hub(const CacheEntry& entry) const {
  return entry.hot_score > 0;
}

void PackedDiskGraphIndex::record_hub_cache_access(
    bool hub_page, bool hit, DiskGraphSearchStats& stats) const {
  if (!hub_page) {
    return;
  }
  ++stats.page_cache_hub_requests;
  if (hit) {
    ++stats.page_cache_hub_hits;
  }
}

const PackedDiskGraphIndex::DiskNode& PackedDiskGraphIndex::find_node_in_page(
    const DecodedPage& page, std::uint32_t node_id) const {
  return DiskPageCodec::find_node(page, node_id);
}

const float* PackedDiskGraphIndex::vector_data(const DecodedPage& page,
                                               const DiskNode& node) const {
  return DiskPageCodec::vector_data(metadata_, page, node);
}

float PackedDiskGraphIndex::compute_distance_direct(
    const float* query, const DecodedPage& page, const DiskNode& node) const {
  return squared_l2(query, vector_data(page, node), metadata_.dim);
}

const PackedDiskGraphIndex::DecodedPage*
PackedDiskGraphIndex::lookup_cached_page(std::uint32_t page_id,
                                         DiskGraphSearchStats& stats) {
  if (!cache_enabled()) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto found = page_cache_.find(page_id);
  if (found == page_cache_.end()) {
    return nullptr;
  }

  ++stats.page_cache_hits;
  ++stats.cache_hits;
  record_hub_cache_access(cache_entry_is_hub(found->second), true, stats);
  found->second.last_access = ++cache_clock_;
  ++found->second.frequency;
  if (is_two_queue_policy() && !found->second.protected_queue) {
    found->second.protected_queue = true;
    ++stats.page_cache_promotions;
  }
  return &found->second.page;
}

const PackedDiskGraphIndex::DecodedPage&
PackedDiskGraphIndex::store_cached_page(DecodedPage page,
                                        DiskGraphSearchStats& stats) {
  if (!cache_enabled()) {
    scratch_page_ = std::move(page);
    return scratch_page_;
  }

  const std::uint32_t page_id = page.page_id;
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto found = page_cache_.find(page_id);
  if (found != page_cache_.end()) {
    found->second.page = std::move(page);
    found->second.last_access = ++cache_clock_;
    ++found->second.frequency;
    found->second.hot_score = page_hot_score(found->second.page);
    if (is_two_queue_policy() && !found->second.protected_queue) {
      found->second.protected_queue = true;
      ++stats.page_cache_promotions;
    }
    return found->second.page;
  }

  if (page_cache_.size() >= cache_capacity_pages_) {
    if (!evict_one_page_locked(&stats)) {
      scratch_page_ = std::move(page);
      return scratch_page_;
    }
  }

  CacheEntry entry;
  entry.page = std::move(page);
  entry.last_access = ++cache_clock_;
  entry.frequency = 1;
  entry.protected_queue = false;
  entry.hot_score = page_hot_score(entry.page);
  auto inserted = page_cache_.emplace(page_id, std::move(entry));
  return inserted.first->second.page;
}

bool PackedDiskGraphIndex::is_two_queue_policy() const {
  return cache_policy_ == "2q" || cache_policy_ == "graph-aware-2q" ||
         cache_policy_ == "agent";
}

bool PackedDiskGraphIndex::is_graph_aware_cache_policy() const {
  return cache_policy_ == "graph-aware-2q" || cache_policy_ == "agent";
}

std::uint64_t PackedDiskGraphIndex::page_hot_score(
    const DecodedPage& page) const {
  if (!cache_protect_hot_pages_ && !is_graph_aware_cache_policy()) {
    return 0;
  }
  std::uint64_t score = 0;
  for (const auto& node : page.nodes) {
    const std::uint64_t importance =
        node.id < node_in_degrees_.size()
            ? static_cast<std::uint64_t>(node_in_degrees_[node.id])
            : static_cast<std::uint64_t>(node.neighbors.size());
    if (cache_hot_degree_threshold_ == 0) {
      score += importance;
    } else if (importance >= cache_hot_degree_threshold_) {
      ++score;
    }
  }
  return score;
}

double PackedDiskGraphIndex::cache_score(const CacheEntry& entry) const {
  if (cache_policy_ == "lru") {
    return static_cast<double>(entry.last_access);
  }

  if (cache_policy_ == "2q") {
    const double queue_bonus = entry.protected_queue ? 1000000000.0 : 0.0;
    return queue_bonus + static_cast<double>(entry.last_access);
  }

  if (is_graph_aware_cache_policy()) {
    const double queue_bonus = entry.protected_queue ? 1000000000.0 : 0.0;
    const double hot_bonus = std::log1p(static_cast<double>(entry.hot_score)) *
                             64.0;
    const double frequency =
        static_cast<double>(std::min<std::uint64_t>(entry.frequency, 16));
    const double recency = static_cast<double>(entry.last_access);
    return queue_bonus + recency + hot_bonus + frequency * 4.0;
  }

  const double frequency = static_cast<double>(entry.frequency);
  const double recency = static_cast<double>(entry.last_access);
  const double density = static_cast<double>(entry.page.nodes.size());
  return frequency * 1000.0 + recency + density * 0.01;  // Agent 策略优先长期热点。
}

bool PackedDiskGraphIndex::evict_one_page(DiskGraphSearchStats* stats) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  return evict_one_page_locked(stats);
}

bool PackedDiskGraphIndex::evict_one_page_locked(DiskGraphSearchStats* stats) {
  if (page_cache_.empty()) {
    return false;
  }

  auto victim = page_cache_.end();
  double victim_score = 0.0;
  for (auto it = page_cache_.begin(); it != page_cache_.end(); ++it) {
    if (it->second.pin_count > 0) {
      continue;
    }
    if (victim == page_cache_.end()) {
      victim = it;
      victim_score = cache_score(it->second);
      continue;
    }
    if (is_two_queue_policy() &&
        victim->second.protected_queue != it->second.protected_queue) {
      if (!it->second.protected_queue) {
        victim = it;
        victim_score = cache_score(it->second);
      }
      continue;
    }
    const double score = cache_score(it->second);
    if (score < victim_score ||
        (score == victim_score && it->first < victim->first)) {
      victim = it;
      victim_score = score;
    }
  }
  if (victim == page_cache_.end()) {
    if (stats != nullptr) {
      ++stats->page_cache_pinned_eviction_skips;
    }
    return false;
  }
  page_cache_.erase(victim);
  if (stats != nullptr) {
    ++stats->page_cache_evictions;
  }
  return true;
}

const PackedDiskGraphIndex::DecodedPage& PackedDiskGraphIndex::load_page(
    std::uint32_t page_id, DiskGraphSearchStats& stats) {
  ++stats.page_requests;
  ++stats.page_requests_before_dedup;

  if (const DecodedPage* cached = lookup_cached_page(page_id, stats)) {
    return *cached;
  }

  std::lock_guard<std::mutex> cache_lock(cache_mutex_);
  if (cache_policy_ != "none" && cache_capacity_pages_ > 0) {
    auto found = page_cache_.find(page_id);
    if (found != page_cache_.end()) {
      ++stats.page_cache_hits;  // 跨 query 复用已解码页。
      ++stats.cache_hits;
      found->second.last_access = ++cache_clock_;
      ++found->second.frequency;
      return found->second.page;
    }
  }

  ++stats.page_cache_misses;
  ++stats.demand_reads;
  ++stats.node_reads;
  ++stats.page_requests_after_dedup;
  DecodedPage page = read_page(page_id, stats);
  record_hub_cache_access(page_is_hub(page), false, stats);

  if (cache_policy_ == "none" || cache_capacity_pages_ == 0) {
    scratch_page_ = std::move(page);
    return scratch_page_;
  }

  if (page_cache_.size() >= cache_capacity_pages_) {
    if (!evict_one_page_locked(&stats)) {
      scratch_page_ = std::move(page);
      return scratch_page_;
    }
  }

  CacheEntry entry;
  entry.page = std::move(page);
  entry.last_access = ++cache_clock_;
  entry.frequency = 1;
  entry.protected_queue = false;
  entry.hot_score = page_hot_score(entry.page);
  auto inserted = page_cache_.emplace(page_id, std::move(entry));
  return inserted.first->second.page;
}

std::unique_ptr<PackedDiskGraphIndex::SearchState>
PackedDiskGraphIndex::initialize_search_state(
    const float* query, const DiskGraphSearchConfig& config,
    DiskGraphSearchResult& output) {
  auto state = std::make_unique<SearchState>(*this, query, config, output);
  if (!config.adc_enable || config.pq_model == nullptr ||
      !config.pq_model->enabled()) {
    throw std::runtime_error(
        "Packed graph search requires PQ+ADC; non-PQ candidate generation was removed");
  }

  const std::vector<std::uint32_t> entries =
      config.seed_ids.empty()
          ? select_evenly_spaced_entries(metadata_.vector_count,
                                         config.entry_count)
          : config.seed_ids;

  std::vector<std::uint32_t> accepted_entries;
  accepted_entries.reserve(entries.size());
  for (const auto entry : entries) {
    if (entry >= metadata_.vector_count) {
      continue;
    }
    if (!state->visited.insert(entry).second) {
      continue;
    }
    accepted_entries.push_back(entry);
  }
  state->entry_seed_ids = accepted_entries;

  const auto adc_start = std::chrono::steady_clock::now();
  state->adc_table = config.pq_model->build_adc_table(query);
  const auto adc_end = std::chrono::steady_clock::now();
  output.stats.adc_table_build_us =
      std::chrono::duration<double, std::micro>(adc_end - adc_start).count();

  state->effective_k =
      std::min<std::size_t>(config.top_k,
                            static_cast<std::size_t>(metadata_.vector_count));
  state->result_capacity =
      std::min<std::size_t>(std::max(state->effective_k, config.rerank_topk),
                            static_cast<std::size_t>(metadata_.vector_count));

  std::vector<SearchResult> ranked_entries;
  ranked_entries.reserve(accepted_entries.size());
  for (const auto entry : accepted_entries) {
    const float distance = state->candidate_distance(entry);
    ranked_entries.push_back(SearchResult{entry, distance});
  }
  std::sort(ranked_entries.begin(), ranked_entries.end(),
            [](const SearchResult& lhs, const SearchResult& rhs) {
              if (lhs.distance == rhs.distance) {
                return lhs.id < rhs.id;
              }
              return lhs.distance < rhs.distance;
            });

  state->entry_seed_ids.clear();
  state->entry_seed_ids.reserve(ranked_entries.size());
  for (const auto& result : ranked_entries) {
    state->entry_seed_ids.push_back(result.id);
    state->candidates.push(result);
    state->add_best(result);
  }
  (void)maybe_issue_entry_warmup(*state);
  return state;
}

std::vector<std::uint32_t> PackedDiskGraphIndex::collect_jit_frontier_pages(
    SearchState& state, std::size_t effective_beam_width) const {
  const std::size_t window_multiplier =
      std::max<std::size_t>(1, state.config.jit_window_multiplier);
  const std::size_t scan_width =
      std::max<std::size_t>(1, effective_beam_width * window_multiplier);
  auto frontier = state.candidates;
  std::vector<SearchResult> window;
  window.reserve(scan_width);
  while (!frontier.empty() && window.size() < scan_width) {
    const auto next = frontier.top();
    frontier.pop();
    if (!state.session.is_node_materialized(next.id)) {
      window.push_back(next);
    }
  }

  std::vector<std::uint32_t> page_ids;
  page_ids.reserve(window.size());
  for (const auto& candidate : window) {
    page_ids.push_back(state.session.page_for_node(candidate.id));
  }
  return page_ids;
}

std::vector<std::uint32_t>
PackedDiskGraphIndex::collect_entry_warmup_pages(
    const SearchState& state) const {
  if (state.entry_seed_ids.empty()) {
    return {};
  }
  const std::size_t requested_beam_width =
      state.config.beam_width == 0 ? state.config.search_width
                                   : state.config.beam_width;
  const std::size_t warmup_window =
      std::min(state.entry_seed_ids.size(),
               std::clamp(requested_beam_width, std::size_t{1},
                          state.config.search_width));
  std::vector<std::uint32_t> page_ids;
  page_ids.reserve(warmup_window);
  for (std::size_t i = 0; i < warmup_window; ++i) {
    const auto entry = state.entry_seed_ids[i];
    if (!state.session.is_node_materialized(entry)) {
      page_ids.push_back(state.session.page_for_node(entry));
    }
  }
  return page_ids;
}

std::vector<std::uint32_t>
PackedDiskGraphIndex::collect_post_expand_lookahead_pages(
    const SearchState& state, std::size_t effective_beam_width) const {
  const std::size_t batch_width = std::max<std::size_t>(1, effective_beam_width);
  const std::size_t window_multiplier =
      std::max<std::size_t>(2, state.config.jit_window_multiplier);
  const std::size_t scan_width = batch_width * window_multiplier;

  auto frontier = state.candidates;
  std::vector<std::uint32_t> lookahead_pages;
  lookahead_pages.reserve(scan_width);

  while (!frontier.empty() && lookahead_pages.size() < scan_width) {
    const auto next = frontier.top();
    frontier.pop();
    if (state.session.is_node_materialized(next.id)) {
      continue;
    }

    lookahead_pages.push_back(state.session.page_for_node(next.id));
  }
  return lookahead_pages;
}

std::vector<std::uint32_t>
PackedDiskGraphIndex::collect_rerank_lookahead_pages(
    const SearchState& state) const {
  std::vector<std::uint32_t> page_ids;
  if (state.output.topk.empty() || state.config.rerank_topk == 0) {
    return page_ids;
  }

  const std::size_t rerank_limit =
      std::min(state.output.topk.size(), state.config.rerank_topk);
  page_ids.reserve(rerank_limit);
  for (std::size_t i = 0; i < rerank_limit; ++i) {
    const auto& candidate = state.output.topk[i];
    if (state.session.is_node_materialized(candidate.id)) {
      continue;
    }
    page_ids.push_back(state.session.page_for_node(candidate.id));
  }
  return page_ids;
}

void PackedDiskGraphIndex::maybe_issue_prefetch(SearchState& state) const {
  if (!post_expand_prefetch_enabled(state.config)) {
    return;
  }
  const std::size_t prefetch_top_k =
      state.config.prefetch_top_k == 0 ? state.config.prefetch_width
                                       : state.config.prefetch_top_k;
  if (pre_beam_prefetch_enabled(state.config) &&
      state.config.prefetch_max_inflight <= prefetch_top_k) {
    return;
  }
  if (state.session.async_prefetch_enabled()) {
    state.session.drain_ready_pages();
  }
  if (!state.session.candidate_prefetch_enabled() ||
      state.candidates.empty()) {
    return;
  }

  const std::size_t requested_beam_width =
      state.config.beam_width == 0 ? state.config.search_width
                                   : state.config.beam_width;
  const std::size_t effective_beam_width =
      std::clamp(requested_beam_width, std::size_t{1},
                 state.config.search_width);
  const auto page_ids =
      collect_post_expand_lookahead_pages(state, effective_beam_width);
  state.session.submit_candidate_prefetch(page_ids, "post_expand");
}

std::size_t PackedDiskGraphIndex::maybe_issue_entry_warmup(
    SearchState& state) const {
  if (!entry_warmup_prefetch_enabled(state.config) ||
      !state.session.async_prefetch_enabled() ||
      state.entry_seed_ids.empty()) {
    return 0;
  }

  const std::size_t max_jit_pages =
      state.config.jit_prefetch_max_pages_per_query;
  if (max_jit_pages > 0 &&
      state.jit_prefetch_submitted_pages >= max_jit_pages) {
    ++state.output.stats.jit_prefetch_budget_exhausted;
    return 0;
  }

  const auto page_ids = collect_entry_warmup_pages(state);
  if (page_ids.empty()) {
    return 0;
  }

  std::vector<std::uint32_t> capped_pages;
  if (max_jit_pages == 0) {
    capped_pages = page_ids;
  } else {
    const std::size_t remaining_budget =
        max_jit_pages - state.jit_prefetch_submitted_pages;
    capped_pages.reserve(std::min(remaining_budget, page_ids.size()));
    std::unordered_set<std::uint32_t> accepted_pages;
    accepted_pages.reserve(page_ids.size());
    for (const auto page_id : page_ids) {
      if (accepted_pages.find(page_id) == accepted_pages.end() &&
          accepted_pages.size() >= remaining_budget) {
        ++state.output.stats.jit_prefetch_candidates_capped;
        continue;
      }
      accepted_pages.insert(page_id);
      capped_pages.push_back(page_id);
    }
  }

  const std::size_t submitted =
      state.session.submit_jit_prefetch(capped_pages, "warmup_entry", 1);
  state.jit_prefetch_submitted_pages += submitted;
  return submitted;
}

std::size_t PackedDiskGraphIndex::maybe_issue_rerank_prefetch(
    SearchState& state) const {
  if (!rerank_prefetch_enabled(state.config) ||
      !state.session.async_prefetch_enabled() ||
      state.config.rerank_topk == 0 || state.output.topk.empty()) {
    return 0;
  }

  const auto page_ids = collect_rerank_lookahead_pages(state);
  if (page_ids.empty()) {
    return 0;
  }
  return state.session.submit_jit_prefetch(page_ids, "rerank", 1);
}

std::size_t PackedDiskGraphIndex::maybe_issue_jit_prefetch(
    SearchState& state, std::size_t effective_beam_width) const {
  if (!pre_beam_prefetch_enabled(state.config) ||
      !state.session.async_prefetch_enabled() || state.candidates.empty()) {
    return 0;
  }
  const std::size_t max_jit_pages =
      state.config.jit_prefetch_max_pages_per_query;
  if (max_jit_pages > 0 &&
      state.jit_prefetch_submitted_pages >= max_jit_pages) {
    ++state.output.stats.jit_prefetch_budget_exhausted;
    return 0;
  }
  const std::size_t interval =
      std::max<std::size_t>(1, state.config.jit_prefetch_interval_batches);
  if (state.output.stats.batch_count % interval != 0) {
    ++state.output.stats.jit_prefetch_skipped_by_interval;
    return 0;
  }
  const auto page_ids = collect_jit_frontier_pages(state, effective_beam_width);
  if (page_ids.empty()) {
    return 0;
  }
  const std::size_t remaining_budget =
      max_jit_pages == 0
          ? std::numeric_limits<std::size_t>::max()
          : max_jit_pages - state.jit_prefetch_submitted_pages;
  std::vector<std::uint32_t> capped_pages;
  capped_pages.reserve(std::min(remaining_budget, page_ids.size()));
  std::unordered_set<std::uint32_t> seen_pages;
  seen_pages.reserve(page_ids.size());
  for (std::size_t i = 0; i < page_ids.size(); ++i) {
    const std::uint32_t page_id = page_ids[i];
    if (!seen_pages.insert(page_id).second ||
        state.session.page_available_for_scheduling(page_id)) {
      continue;
    }
    capped_pages.push_back(page_id);
    if (capped_pages.size() >= remaining_budget) {
      if (i + 1 < page_ids.size()) {
        state.output.stats.jit_prefetch_candidates_capped +=
            page_ids.size() - i - 1;
      }
      break;
    }
  }
  if (capped_pages.empty()) {
    return 0;
  }
  ++state.output.stats.jit_prefetch_windows;
  const std::size_t submitted =
      state.session.submit_jit_prefetch(capped_pages, "pre_beam");
  state.jit_prefetch_submitted_pages += submitted;
  return submitted;
}

bool PackedDiskGraphIndex::update_frontier(SearchState& state) const {
  if (state.config.adaptive_early_stop &&
      state.output.stats.expanded >= state.config.min_expansions &&
      state.stagnant_expansions >= state.config.early_stop_patience) {
    return true;
  }
  return !state.candidates.empty() && state.should_stop(state.candidates.top());
}

std::vector<SearchResult> PackedDiskGraphIndex::select_candidate_batch(
    SearchState& state, std::size_t effective_beam_width) const {
  std::vector<SearchResult> window;
  const std::size_t remaining =
      state.config.search_width > state.output.stats.expanded
          ? state.config.search_width - state.output.stats.expanded
          : 0;
  const std::size_t batch_limit =
      std::min(effective_beam_width, remaining);
  if (batch_limit == 0) {
    return window;
  }

  window.reserve(batch_limit);
  while (!state.candidates.empty() && window.size() < batch_limit) {
    if (update_frontier(state)) {
      break;
    }
    window.push_back(state.candidates.top());
    state.candidates.pop();
  }

  std::unordered_set<std::uint32_t> batch_pages;
  std::unordered_set<std::uint32_t> available_pages;
  batch_pages.reserve(window.size());
  available_pages.reserve(window.size());
  for (const auto& candidate : window) {
    const std::uint32_t page_id = state.session.page_for_node(candidate.id);
    batch_pages.insert(page_id);
    if (state.session.page_available_for_scheduling(page_id)) {
      available_pages.insert(page_id);
    }
  }
  state.output.stats.beam_unique_pages += batch_pages.size();
  state.output.stats.beam_cached_or_pending_pages += available_pages.size();
  return window;
}

void PackedDiskGraphIndex::expand_candidate(SearchState& state,
                                            const SearchResult& current) {
  const DiskNode& node = state.load_node(current.id);
  (void)state.session.pin_page(state.session.page_for_node(current.id));
  ++state.output.stats.expanded;

  std::vector<std::uint32_t> new_neighbors;
  new_neighbors.reserve(node.neighbors.size());
  for (const auto neighbor_id : node.neighbors) {
    if (neighbor_id >= metadata_.vector_count ||
        !state.visited.insert(neighbor_id).second) {
      continue;
    }
    new_neighbors.push_back(neighbor_id);
  }

  std::vector<SearchResult> ranked_neighbors;
  ranked_neighbors = state.rank_neighbors_by_distance(new_neighbors);
  new_neighbors.clear();
  new_neighbors.reserve(ranked_neighbors.size());
  for (const auto& neighbor : ranked_neighbors) {
    new_neighbors.push_back(neighbor.id);
  }

  for (const auto& candidate : ranked_neighbors) {
    state.candidates.push(candidate);
    state.add_best(candidate);
  }
  state.update_adaptive_stop();
}

void PackedDiskGraphIndex::expand_candidate_batch(
    SearchState& state, const std::vector<SearchResult>& batch) {
  std::vector<std::vector<SearchResult>> ranked_neighbors_by_candidate;
  ranked_neighbors_by_candidate.reserve(batch.size());

  for (const auto& current : batch) {
    const DiskNode& node = state.load_node(current.id);
    (void)state.session.pin_page(state.session.page_for_node(current.id));
    ++state.output.stats.expanded;

    std::vector<std::uint32_t> new_neighbors;
    new_neighbors.reserve(node.neighbors.size());
    for (const auto neighbor_id : node.neighbors) {
      if (neighbor_id >= metadata_.vector_count ||
          !state.visited.insert(neighbor_id).second) {
        continue;
      }
      new_neighbors.push_back(neighbor_id);
    }
    std::vector<SearchResult> ranked_neighbors =
        state.rank_neighbors_by_distance(new_neighbors);
    new_neighbors.clear();
    new_neighbors.reserve(ranked_neighbors.size());
    for (const auto& neighbor : ranked_neighbors) {
      new_neighbors.push_back(neighbor.id);
    }
    ranked_neighbors_by_candidate.push_back(std::move(ranked_neighbors));
  }

  for (const auto& ranked_neighbors : ranked_neighbors_by_candidate) {
    for (const auto& candidate : ranked_neighbors) {
      state.candidates.push(candidate);
      state.add_best(candidate);
    }
    state.update_adaptive_stop();
  }
}

void PackedDiskGraphIndex::finalize_topk(SearchState& state) {
  state.output.stats.visited = state.visited.size();
  state.output.topk = sorted_results(state.best);
  if (!state.adc_table.empty() && state.config.rerank_topk > 0) {
    (void)maybe_issue_rerank_prefetch(state);
    const std::size_t reads_before_rerank = state.output.stats.node_reads;
    for (auto& result : state.output.topk) {
      const DecodedPage& page =
          state.session.get_or_load_page(state.session.page_for_node(result.id));
      const DiskNode& node = find_node_in_page(page, result.id);
      ++state.output.stats.distance_direct_calls;
      result.distance = compute_distance_direct(state.query, page, node);
    }
    state.output.stats.rerank_reads +=
        state.output.stats.node_reads - reads_before_rerank;
    std::sort(state.output.topk.begin(), state.output.topk.end(),
              [](const SearchResult& lhs, const SearchResult& rhs) {
                return lhs.distance == rhs.distance ? lhs.id < rhs.id
                                                    : lhs.distance < rhs.distance;
              });
  }
  if (state.output.topk.size() > state.effective_k) {
    state.output.topk.resize(state.effective_k);
  }
  state.session.finish_query();
}

DiskGraphSearchResult PackedDiskGraphIndex::search_one(
    const float* query, const DiskGraphSearchConfig& config) {
  const auto lock_start = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> search_lock(search_mutex_);
  const double search_mutex_wait_us =
      std::chrono::duration<double, std::micro>(
          std::chrono::steady_clock::now() - lock_start)
          .count();
  if (config.top_k == 0 || config.search_width == 0 ||
      config.entry_count == 0) {
    throw std::runtime_error("Packed graph search top_k, search_width, and entry_count must be positive");
  }
  const std::size_t effective_beam_width =
      std::clamp(config.beam_width == 0 ? config.search_width
                                         : config.beam_width,
                 std::size_t{1}, config.search_width);

  DiskGraphSearchResult output;
  output.stats.search_mutex_wait_us = search_mutex_wait_us;
  auto state = initialize_search_state(query, config, output);
  while (!state->candidates.empty() &&
         output.stats.expanded < config.search_width) {
    if (state->session.async_prefetch_enabled()) {
      state->session.drain_ready_pages();
    }
    const std::size_t pre_beam_submitted =
        maybe_issue_jit_prefetch(*state, effective_beam_width);
    std::vector<SearchResult> batch =
        select_candidate_batch(*state, effective_beam_width);
    if (batch.empty()) {
      break;
    }

    std::vector<std::uint32_t> page_ids;
    page_ids.reserve(batch.size());
    for (const auto& candidate : batch) {
      page_ids.push_back(state->session.page_for_node(candidate.id));
    }
    state->session.ensure_pages_loaded_batch(page_ids);

    ++output.stats.batch_count;
    output.stats.batch_expanded += batch.size();
    output.stats.max_batch_size =
        std::max(output.stats.max_batch_size, batch.size());

    expand_candidate_batch(*state, batch);
    if ((!pre_beam_prefetch_enabled(state->config) || pre_beam_submitted == 0) &&
        output.stats.expanded < config.search_width &&
        !state->candidates.empty() && !update_frontier(*state)) {
      maybe_issue_prefetch(*state);
    }
  }
  finalize_topk(*state);
  return output;
}


}  // namespace agent_aware
