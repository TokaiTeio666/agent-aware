#include "graph/disk_graph_index.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/async_page_reader.h"
#include "core/brute_force.h"
#include "core/query_page_session.h"
#include "graph/disk_page_codec.h"
#include "graph/vamana_builder.h"

namespace agent_aware {
namespace {

// PackedDiskGraphIndex 查询 V2 页布局，组合 PQ/ADC 候选打分、页 cache 复用和单 query 异步预取会话。
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

}  // namespace

std::vector<TopologyPageCandidate> rank_topology_pages(
    const std::vector<std::uint32_t>& ranked_page_ids,
    std::size_t candidate_window) {
  const std::size_t limit = std::min(ranked_page_ids.size(), candidate_window);
  std::unordered_map<std::uint32_t, TopologyPageCandidate> pages;
  pages.reserve(limit);
  for (std::size_t rank = 0; rank < limit; ++rank) {
    const auto page_id = ranked_page_ids[rank];
    auto [it, inserted] = pages.emplace(
        page_id, TopologyPageCandidate{page_id, rank, 0});
    ++it->second.candidate_count;
    if (!inserted) it->second.min_pq_rank = std::min(it->second.min_pq_rank, rank);
  }
  std::vector<TopologyPageCandidate> ranked;
  ranked.reserve(pages.size());
  for (const auto& entry : pages) ranked.push_back(entry.second);
  std::sort(ranked.begin(), ranked.end(),
            [](const TopologyPageCandidate& lhs,
               const TopologyPageCandidate& rhs) {
              if (lhs.min_pq_rank != rhs.min_pq_rank) {
                return lhs.min_pq_rank < rhs.min_pq_rank;
              }
              if (lhs.candidate_count != rhs.candidate_count) {
                return lhs.candidate_count > rhs.candidate_count;
              }
              return lhs.page_id < rhs.page_id;
            });
  return ranked;
}

struct PackedDiskGraphIndex::SearchState {
  SearchState(PackedDiskGraphIndex& index, const float* query,
              const DiskGraphSearchConfig& config,
              DiskGraphSearchResult& output)
      : index(index),
        query(query),
        config(config),
        output(output),
        session(index, config, output.stats) {}

  // SearchState 保存单 query 可变遍历状态；共享的 index 对象只负责索引元数据和 cache。
  const DiskNode& load_node(std::uint32_t id) {
    return session.get_node(id, true);
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
  std::vector<float> adc_table;
  std::size_t effective_k = 0;
  std::size_t result_capacity = 0;
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
    constexpr char kV2Magic[8] = {'A', 'M', 'F', 'P', 'V', '2', '\0', '\0'};
    if (std::memcmp(magic, kV2Magic, sizeof(kV2Magic)) == 0) {
      throw std::runtime_error(
          "V2 packed index is obsolete; rebuild with --rebuild-index 1");
    }
    throw std::runtime_error("Invalid packed graph index magic: " + path);
  }

  const auto version = read_value<std::uint32_t>(input_);
  if (version != kPackedGraphVersion) {
    throw std::runtime_error(
        "Unsupported packed graph index version; rebuild with agent-aware-build");
  }
  metadata_.format_version = version;

  metadata_.vector_count = read_value<std::uint64_t>(input_);
  metadata_.dim = read_value<std::uint32_t>(input_);
  metadata_.degree = read_value<std::uint32_t>(input_);
  metadata_.page_size = read_value<std::uint32_t>(input_);
  metadata_.records_offset = read_value<std::uint64_t>(input_);
  metadata_.directory_offset = read_value<std::uint64_t>(input_);
  metadata_.page_count = read_value<std::uint64_t>(input_);
  metadata_.nodes_per_page = read_value<std::uint32_t>(input_);
  metadata_.pq_subspaces = read_value<std::uint32_t>(input_);
  metadata_.pq_centroids = read_value<std::uint32_t>(input_);
  metadata_.pq_dim = read_value<std::uint32_t>(input_);
  metadata_.pq_offset = read_value<std::uint64_t>(input_);
  metadata_.pq_bytes = read_value<std::uint64_t>(input_);
  metadata_.pq_offsets_offset = read_value<std::uint64_t>(input_);
  metadata_.pq_offsets_bytes = read_value<std::uint64_t>(input_);
  metadata_.pq_codebook_offset = read_value<std::uint64_t>(input_);
  metadata_.pq_codebook_bytes = read_value<std::uint64_t>(input_);
  metadata_.pq_codes_offset = read_value<std::uint64_t>(input_);
  metadata_.pq_codes_bytes = read_value<std::uint64_t>(input_);
  metadata_.index_bytes = std::filesystem::file_size(path_);

  if (metadata_.vector_count == 0 || metadata_.dim == 0 ||
      metadata_.degree == 0 || metadata_.page_size == 0 ||
      metadata_.page_count == 0 || metadata_.nodes_per_page == 0) {
    throw std::runtime_error("Packed graph index metadata is invalid");
  }
  if (graph_record_bytes(metadata_.dim, metadata_.degree) > metadata_.page_size) {
    throw std::runtime_error("Packed graph node payload exceeds page size");
  }
  const std::uint64_t directory_bytes =
      metadata_.vector_count * sizeof(std::uint32_t);
  const std::uint64_t records_bytes = metadata_.page_count * metadata_.page_size;
  if (metadata_.directory_offset > metadata_.index_bytes ||
      directory_bytes > metadata_.index_bytes - metadata_.directory_offset ||
      metadata_.records_offset > metadata_.index_bytes ||
      records_bytes > metadata_.index_bytes - metadata_.records_offset) {
    throw std::runtime_error("Packed graph index sections exceed file bounds");
  }
  if (metadata_.pq_bytes == 0 || metadata_.pq_subspaces == 0 ||
      metadata_.pq_centroids == 0 || metadata_.pq_dim != metadata_.dim ||
      metadata_.pq_offset > metadata_.index_bytes ||
      metadata_.pq_bytes > metadata_.index_bytes - metadata_.pq_offset ||
      metadata_.pq_offsets_offset < metadata_.pq_offset ||
      metadata_.pq_codebook_offset !=
          metadata_.pq_offsets_offset + metadata_.pq_offsets_bytes ||
      metadata_.pq_codes_offset !=
          metadata_.pq_codebook_offset + metadata_.pq_codebook_bytes ||
      metadata_.pq_codes_offset + metadata_.pq_codes_bytes !=
          metadata_.pq_offset + metadata_.pq_bytes) {
    throw std::runtime_error("Packed graph PQ section metadata is invalid");
  }
  input_.clear();
  input_.seekg(static_cast<std::streamoff>(metadata_.pq_offset), std::ios::beg);
  pq_model_ = PqAdcModel::read(input_);
  if (pq_model_.dim() != metadata_.pq_dim ||
      pq_model_.subspaces() != metadata_.pq_subspaces ||
      pq_model_.centroids() != metadata_.pq_centroids ||
      pq_model_.vector_count() != metadata_.vector_count ||
      metadata_.pq_offsets_bytes !=
          (pq_model_.subspaces() + 1) * sizeof(std::uint64_t) ||
      pq_model_.codebook_bytes() != metadata_.pq_codebook_bytes ||
      pq_model_.code_bytes() != metadata_.pq_codes_bytes) {
    throw std::runtime_error("Packed graph PQ model does not match its header");
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
    // hub-aware 策略需要从序列化图中统计入度，只在策略真正会使用时才懒加载。
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

std::size_t PackedDiskGraphIndex::cache_capacity_pages() const {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  return cache_capacity_pages_;
}

std::size_t PackedDiskGraphIndex::cache_resident_pages() const {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  return page_cache_.size();
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

  // 这是 cache 策略使用的离线统计，刻意走 read_page，校验正常查询也会读取的同一份字节。
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
    // graph-aware 2Q 保留 recency/protected-queue 信号，并加入 hub bonus 和频次上限来保护高复用页。
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
      config.seed_ids.empty() ? std::vector<std::uint32_t>{0}
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

  // entry point 按 PQ 距离顺序入堆，避免第一批扩展被任意 id 顺序主导。
  for (const auto& result : ranked_entries) {
    state->candidates.push(result);
    state->add_best(result);
  }
  return state;
}

std::vector<std::uint32_t> PackedDiskGraphIndex::collect_rerank_pages(
    const SearchState& state) const {
  std::vector<std::uint32_t> page_ids;
  if (state.output.topk.empty() || state.config.rerank_topk == 0) {
    return page_ids;
  }
  page_ids.reserve(state.output.topk.size());
  for (const auto& candidate : state.output.topk) {
    const std::uint32_t page_id = state.session.page_for_node(candidate.id);
    if (std::find(page_ids.begin(), page_ids.end(), page_id) ==
        page_ids.end()) {
      page_ids.push_back(page_id);
    }
  }
  return page_ids;
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
    if (state.session.page_available(page_id)) {
      available_pages.insert(page_id);
    }
  }
  state.output.stats.beam_unique_pages += batch_pages.size();
  state.output.stats.beam_cached_pages += available_pages.size();
  return window;
}

void PackedDiskGraphIndex::expand_candidate_batch(
    SearchState& state, const std::vector<SearchResult>& batch,
    std::size_t effective_beam_width) {
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

    ranked_neighbors_by_candidate.push_back(std::move(ranked_neighbors));
  }

  if (state.config.prefetch_policy == "topology") {
    const auto planner_start = std::chrono::steady_clock::now();
    const std::size_t candidate_limit =
        std::max<std::size_t>(1, effective_beam_width) * 2;
    std::priority_queue<SearchResult, std::vector<SearchResult>, CloserFirst>
        projected_frontier = state.candidates;
    for (const auto& ranked : ranked_neighbors_by_candidate) {
      for (const auto& candidate : ranked) projected_frontier.push(candidate);
    }
    const std::size_t scan_limit =
        std::min(projected_frontier.size(), candidate_limit);
    state.output.stats.prefetch_candidate_nodes += scan_limit;
    std::vector<std::uint32_t> ranked_page_ids;
    ranked_page_ids.reserve(scan_limit);
    for (std::size_t rank = 0; rank < scan_limit; ++rank) {
      const auto candidate = projected_frontier.top();
      projected_frontier.pop();
      if (state.session.is_node_materialized(candidate.id)) continue;
      const std::uint32_t page_id = state.session.page_for_node(candidate.id);
      ranked_page_ids.push_back(page_id);
    }
    const auto ranked_pages = rank_topology_pages(ranked_page_ids, scan_limit);
    state.session.record_topology_candidates(ranked_pages);
    std::vector<std::uint32_t> page_ids;
    page_ids.reserve(ranked_pages.size());
    for (const auto& page : ranked_pages) page_ids.push_back(page.page_id);
    state.output.stats.prefetch_candidate_pages += page_ids.size();
    (void)state.session.submit_topology_prefetch(page_ids);
    state.output.stats.prefetch_planner_cpu_us +=
        std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - planner_start).count();
  }

  // 将 PQ 排序后的邻居推入候选堆（CPU 计算已在 rank_neighbors_by_distance 中完成）
  for (const auto& ranked_neighbors : ranked_neighbors_by_candidate) {
    for (const auto& candidate : ranked_neighbors) {
      state.candidates.push(candidate);
      state.add_best(candidate);
    }
  }
}

void PackedDiskGraphIndex::finalize_topk(SearchState& state) {
  state.output.stats.visited = state.visited.size();
  state.output.topk = sorted_results(state.best);
  if (!state.adc_table.empty() && state.config.rerank_topk > 0) {
    const auto rerank_pages = collect_rerank_pages(state);
    constexpr std::size_t kRerankChunkPages = 2;
    const std::size_t reads_before_rerank = state.output.stats.node_reads;
    for (std::size_t begin = 0; begin < rerank_pages.size();
         begin += kRerankChunkPages) {
      const std::size_t end =
          std::min(begin + kRerankChunkPages, rerank_pages.size());
      const std::vector<std::uint32_t> current_pages(
          rerank_pages.begin() + begin, rerank_pages.begin() + end);
      state.session.ensure_pages_loaded_batch(current_pages);

      const std::unordered_set<std::uint32_t> current_page_set(
          current_pages.begin(), current_pages.end());
      for (auto& result : state.output.topk) {
        if (current_page_set.find(state.session.page_for_node(result.id)) ==
            current_page_set.end()) {
          continue;
        }
        const DiskNode& node = state.session.get_node(result.id, true);
        ++state.output.stats.distance_direct_calls;
        result.distance = squared_l2(state.query, node.vector.data(),
                                     metadata_.dim);
      }
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
  if (config.prefetch_policy != "none" &&
      config.prefetch_policy != "topology") {
    throw std::runtime_error(
        "Packed graph prefetch policy must be none or topology");
  }
  DiskGraphSearchConfig effective_config = config;
  if (effective_config.pq_model != nullptr) {
    const auto& external = *effective_config.pq_model;
    if (!external.enabled() || external.dim() != pq_model_.dim() ||
        external.subspaces() != pq_model_.subspaces() ||
        external.centroids() != pq_model_.centroids() ||
        external.vector_count() != pq_model_.vector_count()) {
      throw std::runtime_error(
          "External PQ model does not match the packed V3 index");
    }
  } else {
    effective_config.pq_model = &pq_model_;
  }
  effective_config.adc_enable = true;
  const std::size_t effective_beam_width =
      std::clamp(config.beam_width == 0 ? config.search_width
                                         : config.beam_width,
                 std::size_t{1}, config.search_width);

  DiskGraphSearchResult output;
  output.stats.search_mutex_wait_us = search_mutex_wait_us;
  auto state = initialize_search_state(query, effective_config, output);
  while (!state->candidates.empty() &&
         output.stats.expanded < config.search_width) {
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

    expand_candidate_batch(*state, batch, effective_beam_width);
  }
  finalize_topk(*state);
  return output;
}


}  // namespace agent_aware
