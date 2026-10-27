#include "graph/disk_graph_index.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// 候选的全精度距离比当前第 k 名远出这么多时才终止搜索。ADC 近似值可以被
// 当作"上限"来提前放弃，但 PQ 量化误差会让全精度距离在小范围内抖动；
// 留一点余量避免把差一点点进前 k 的点提前剪掉。
constexpr float kSearchStopSlack = 1e-3f;

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

  // 注意：PQ 的 ADC 距离**不能**当作可靠下界用于剪枝 —— 实测用
  // "第 k 名 + ADC 距离"判定会把召回率从 0.90 打到 0.28。
  // ADC 只用于在"还没有页里的全精度距离时"给候选排序。

  // 全精度距离：页已经在会话里（扩展时刚读过），向量就在 decoded.bytes 中，
  // 只算一次、不产生额外 I/O。ADC 是页不在手的近似值，不能代替它做决策。
  float exact_distance(std::uint32_t id) {
    ++output.stats.distance_direct_calls;
    return index.compute_distance_direct(query, page_holding(id), load_node(id));
  }

  const PackedDiskGraphIndex::DecodedPage& page_holding(std::uint32_t id) {
    return session.get_or_load_page(session.page_for_node(id));
  }

  // 锚点探测：几个起点分散在整个空间，页几乎不可能彼此同页，所以整体一次
  // 批量载入，把 N 次单页提交压成一趟往返；之后逐个算精确距离（零 I/O）。
  // anchor 阶段的计时与计数只覆盖这次载入。
  void prefetch_pages_batch(const std::vector<std::uint32_t>& ids) {
    std::vector<std::uint32_t> page_ids;
    page_ids.reserve(ids.size());
    for (const auto id : ids) {
      page_ids.push_back(session.page_for_node(id));
    }
    session.ensure_pages_loaded_batch(page_ids, DemandReadStage::kEntryAnchor);
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

std::size_t PackedDiskGraphIndex::resident_page_bytes(
    const DiskGraphMetadata& metadata) {
  // 解码页保留原始字节（bytes），同时为每个节点复制一份邻接 vector；
  // 邻接在解码时按 degree reserve，因此按满载容量估算。
  return static_cast<std::size_t>(metadata.page_size) +
         static_cast<std::size_t>(metadata.nodes_per_page) *
             static_cast<std::size_t>(metadata.degree) *
             sizeof(std::uint32_t);
}

std::size_t PackedDiskGraphIndex::cache_resident_bytes() const {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  return page_cache_.size() * resident_page_bytes(metadata_);
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
  // 先写到临时数组再发布，否则并发配置两个 index 时读者可能看到半张表。
  std::vector<std::uint32_t> degrees(
      static_cast<std::size_t>(metadata_.vector_count), 0);
  DiskGraphSearchStats ignored;
  for (std::uint32_t page_id = 0; page_id < metadata_.page_count; ++page_id) {
    DecodedPage page = read_page(page_id, ignored);
    for (const auto& node : page.nodes) {
      for (const auto neighbor : node.neighbors) {
        if (neighbor < degrees.size()) {
          ++degrees[neighbor];
        }
      }
    }
  }
  node_in_degrees_ = std::move(degrees);
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
  // 同一 index 上的多个会话共享 scratch_page_（无 cache 路径）与页缓冲池，
  // 因此会话期必须独占搜索锁。这里显式记账，让"并发检索"的代价可见，
  // 而不是依赖调用方恰好持有 search_mutex_。
  ++live_sessions_;
  if (live_sessions_ > 1) {
    --live_sessions_;
    throw std::runtime_error(
        "Packed graph index supports one concurrent query session per index; "
        "open one index per worker");
  }
  // 调用方传进来的 config 已经由 search_one 合并过索引自带的 PQ 模型，
  // 这里只做前置校验，不再自行改动配置。
  auto state = std::make_unique<SearchState>(*this, query, config, output);
  if (!config.adc_enable || config.pq_model == nullptr ||
      !config.pq_model->enabled()) {
    // 非 PQ 的候选生成路径已被移除，所以关闭 ADC 目前无法工作。
    // 这里明确报错而不是静默回落，否则对照实验会得出"关掉 ADC 性能相同"
    // 的错误结论 —— 实际是开关根本没生效。
    throw std::runtime_error(
        "Packed graph search requires PQ+ADC; non-PQ candidate generation was "
        "removed, so adc_enable cannot be turned off");
  }

  const std::vector<std::uint32_t> entries =
      config.seed_ids.empty() ? std::vector<std::uint32_t>{0}
                              : config.seed_ids;

  std::vector<std::uint32_t> all_entries = entries;
  // 均匀撒开起始点：见 DiskGraphSearchConfig::spread_start_count 的说明。
  const std::size_t spread_request = config.spread_start_count;
  const std::size_t spread_cap = std::min<std::size_t>(
      spread_request, static_cast<std::size_t>(metadata_.vector_count));
  if (spread_cap > 1) {
    const std::size_t stride =
        static_cast<std::size_t>(metadata_.vector_count) / spread_cap;
    for (std::size_t i = 0; i < spread_cap && stride > 0; ++i) {
      all_entries.push_back(static_cast<std::uint32_t>(i * stride));
    }
  }
  std::vector<std::uint32_t> accepted_entries;
  accepted_entries.reserve(all_entries.size());
  for (const auto entry : all_entries) {
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

  // 起始点的两阶段筛选：先用 PQ 码的 ADC 近似值把撒点排序，再只对最靠前的
  // 若干个读整页算全精度距离。撒开的起始点本身就是要覆盖整个空间（单入口
  // 在 1M 上只能召回 0.04），但每个起点都要读一个分散的页：2048 个起点让
  // 每查询物理读从 2.2 MB 涨到 9.4 MB。ADC 排序不读页，能把"读多少个页面"
  // 与"筛多少候选"解耦。
  constexpr std::size_t kMaxSeedAnchors = 4;
  // 每个保留的锚点，实际读页的候选数（按 ADC 从全部撒点里挑最近的这些）。
  constexpr std::size_t kExactAnchorsPerSlot = 1;
  constexpr std::size_t max_seed_anchors = kMaxSeedAnchors;
  std::vector<std::pair<float, std::uint32_t>> anchor_order;
  anchor_order.reserve(accepted_entries.size());
  for (const auto entry : accepted_entries) {
    anchor_order.emplace_back(state->candidate_distance(entry), entry);
  }
  std::sort(anchor_order.begin(), anchor_order.end());
  const std::size_t exact_probe_count = std::min<std::size_t>(
      anchor_order.size(), max_seed_anchors * kExactAnchorsPerSlot);
  anchor_order.resize(exact_probe_count);
  {
    std::vector<std::uint32_t> probe_ids;
    probe_ids.reserve(anchor_order.size());
    for (const auto& probe : anchor_order) probe_ids.push_back(probe.second);
    state->prefetch_pages_batch(probe_ids);
  }
  for (auto& [anchor_distance, entry] : anchor_order) {
    anchor_distance = state->exact_distance(entry);
  }
  std::sort(anchor_order.begin(), anchor_order.end());
  if (anchor_order.size() > max_seed_anchors) {
    anchor_order.resize(max_seed_anchors);
  }
  if (anchor_order.empty()) {
    // 任何情况下候选堆都不能为空：种子列表一旦被过滤光（全部越界或去重），
    // 搜索会零扩展返回空结果，而调用方只看到一个召回 0 的查询，看不出是
    // 配置错了。退回到 0 号节点，保证图搜索仍然执行。
    state->prefetch_pages_batch({0u});
    anchor_order.emplace_back(state->exact_distance(0), 0u);
  }
  std::vector<SearchResult> ranked_entries;
  ranked_entries.reserve(anchor_order.size());
  for (const auto& [anchor_distance, entry] : anchor_order) {
    ranked_entries.push_back(SearchResult{entry, anchor_distance, false});
  }

  // entry point 按 PQ 距离顺序入堆，避免第一批扩展被任意 id 顺序主导。
  // 注意不在这里加入结果集：结果集只接收全精度距离，近似值进去就再也
  // 改不回来了（best 堆无法原地更新）。
  for (const auto& result : ranked_entries) {
    state->candidates.push(result);
    ++output.stats.candidate_pushes;
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

void PackedDiskGraphIndex::expand_candidate_batch(
    SearchState& state, const std::vector<SearchResult>& batch,
    std::size_t effective_beam_width) {
  std::vector<std::vector<SearchResult>> ranked_neighbors_by_candidate;
  ranked_neighbors_by_candidate.reserve(batch.size());

  // 第一步：逐个候选打开节点、计入结果集、按原顺序收集新邻居 id，同时
  // 记录"这个邻居是否和候选同页"。这一步只碰已装在会话里的页（search_one
  // 已把本批候选所在页批量载入），不发起 I/O；visited 的插入顺序与逐候选
  // 扩展时完全一致。
  std::vector<std::vector<std::uint32_t>> new_neighbors_by_candidate;
  std::vector<std::vector<bool>> neighbor_crosses_page;
  new_neighbors_by_candidate.reserve(batch.size());
  neighbor_crosses_page.reserve(batch.size());
  for (const auto& current : batch) {
    const DiskNode& node = state.load_node(current.id);
    const std::uint32_t candidate_page =
        state.session.page_for_node(current.id);
    (void)state.session.pin_page(candidate_page);
    ++state.output.stats.expanded;
    // 节点刚被打开，向量就在手：此时才算全精度距离并入结果集。
    // 已扩展过的候选（current.approximate == false）用的是精确距离，直接沿用。
    const float exact =
        current.approximate
            ? state.index.compute_distance_direct(state.query,
                                                   state.page_holding(current.id),
                                                   node)
            : current.distance;
    state.add_best(SearchResult{current.id, exact, false});
    if (state.output.record_expanded_distances) {
      state.output.expanded_distances.push_back(exact);
    }

    std::vector<std::uint32_t> new_neighbors;
    std::vector<bool> crosses_page;
    new_neighbors.reserve(node.neighbors.size());
    crosses_page.reserve(node.neighbors.size());
    for (const auto neighbor_id : node.neighbors) {
      if (neighbor_id >= metadata_.vector_count ||
          !state.visited.insert(neighbor_id).second) {
        continue;
      }
      new_neighbors.push_back(neighbor_id);
      crosses_page.push_back(state.session.page_for_node(neighbor_id) !=
                             candidate_page);
    }
    new_neighbors_by_candidate.push_back(std::move(new_neighbors));
    neighbor_crosses_page.push_back(std::move(crosses_page));
  }

  // 第二步：把跨页邻居所在页一次性批量载入。此前每个邻居单独算全精度距离，
  // 页不在会话里就落进 read_demand_page 的单页提交——每页一次
  // io_uring_submit + 一次 wait_cqe 的串行往返，这是 Phase 0 §12 定位的
  // I/O 粒度问题。同页邻居本来就在手，不参与预读。
  std::vector<std::uint32_t> neighbor_page_ids;
  for (std::size_t i = 0; i < new_neighbors_by_candidate.size(); ++i) {
    for (std::size_t j = 0; j < new_neighbors_by_candidate[i].size(); ++j) {
      if (neighbor_crosses_page[i][j]) {
        neighbor_page_ids.push_back(
            state.session.page_for_node(new_neighbors_by_candidate[i][j]));
      }
    }
  }
  state.session.ensure_pages_loaded_batch(neighbor_page_ids,
                                          DemandReadStage::kNeighborRank);

  // 第三步：邻居页此时都在会话里，算全精度距离不产生 I/O。候选堆永远按
  // 真实距离排序，也不会为了"改正近似值"而弹出后又塞回去。
  // 置位只是为了让预读之后仍漏网的按需读（页在预读与打分之间被移走、
  // 或预取竞争时缓冲池不足）归因到邻居阶段而不是锚点探测。
  state.session.set_reading_neighbor_pages(true);
  for (const auto& new_neighbors : new_neighbors_by_candidate) {
    std::vector<SearchResult> ranked_neighbors;
    ranked_neighbors.reserve(new_neighbors.size());
    for (const auto neighbor_id : new_neighbors) {
      ranked_neighbors.push_back(
          SearchResult{neighbor_id, state.exact_distance(neighbor_id), false});
    }
    std::sort(ranked_neighbors.begin(), ranked_neighbors.end(),
              [](const SearchResult& lhs, const SearchResult& rhs) {
                return lhs.distance == rhs.distance ? lhs.id < rhs.id
                                                    : lhs.distance < rhs.distance;
              });

    ranked_neighbors_by_candidate.push_back(std::move(ranked_neighbors));
  }
  state.session.set_reading_neighbor_pages(false);

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

  // 邻居在 expand_candidate_batch 里已经用全精度距离排过序（页刚读过，
  // 算距离不产生额外 I/O），所以推进候选堆的每个 SearchResult 都带
  // approximate=false 的真实距离。这里不需要再用 ADC 近似值占位。
  for (const auto& ranked_neighbors : ranked_neighbors_by_candidate) {
    for (const auto& candidate : ranked_neighbors) {
      state.candidates.push(candidate);
      ++state.output.stats.candidate_pushes;
    }
  }
}

void PackedDiskGraphIndex::finalize_topk(SearchState& state) {
  state.output.stats.visited = state.visited.size();
  state.output.topk = sorted_results(state.best);
  if (!state.adc_table.empty() && state.config.rerank_topk > 0) {
    const auto rerank_pages = collect_rerank_pages(state);
    // rerank 的候选数就是 topk（默认 10），一次批量提交即可。原先按固定
    // 2 页一块切分，等于把一趟往返拆成 5 趟，且与新加的邻居预批口径不一致。
    constexpr std::size_t kRerankChunkPages = 32;
    const std::size_t reads_before_rerank = state.output.stats.node_reads;
    for (std::size_t begin = 0; begin < rerank_pages.size();
         begin += kRerankChunkPages) {
      const std::size_t end =
          std::min(begin + kRerankChunkPages, rerank_pages.size());
      const std::vector<std::uint32_t> current_pages(
          rerank_pages.begin() + begin, rerank_pages.begin() + end);
      state.session.ensure_pages_loaded_batch(current_pages,
                                              DemandReadStage::kRerank);

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
  // 把调用方的配置与索引自带的 PQ 模型合并成一份 effective_config，后续
  // 校验和 SearchState 都只看这一份，避免"校验用一份、执行用另一份"的分叉。
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
  // 这里**不再**无条件置 adc_enable = true。此前那一行会静默覆盖调用方的
  // 设置，使"关掉 ADC"的对照实验实际仍走 ADC，而报告里的开关看起来是关的。
  // 关闭 ADC 现在会在 initialize_search_state 里明确报错（非 PQ 候选生成
  // 路径已被移除），而不是被悄悄改回 true。
  // 实验开关：记录结果集中每个位置的入批顺序（用来看第 k 名是否早期就被
  // 远离查询的节点占住，从而让 relaxed 收敛条件提前终止搜索）。
  const bool trace_results = std::getenv("AGENT_AWARE_TRACE_RESULTS") != nullptr;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> result_trace;
  if (trace_results) {
    result_trace.reserve(512);
  }
  const std::size_t effective_beam_width =
      std::clamp(config.beam_width == 0 ? config.search_width
                                         : config.beam_width,
                 std::size_t{1}, config.search_width);

  DiskGraphSearchResult output;
  output.record_expanded_distances = config.record_expanded_distances;
  // 只在真正拿到锁之后计时：此前写在该字段里的值恒为 0（锁前从未打点），
  // 无法用于论证锁竞争。
  output.stats.search_mutex_wait_us = search_mutex_wait_us;
  // initialize_search_state 成功后才持有会话；它自身的失败（已并发、非 PQ 路径、
  // ADC 表构建）不会在这里减计数，否则 size_t 下溢会让该 index 永久拒绝搜索。
  struct SessionRelease {
    std::atomic<std::size_t>& live;
    ~SessionRelease() { live.fetch_sub(1, std::memory_order_relaxed); }
  } release{live_sessions_};
  std::unique_ptr<SearchState> state =
      initialize_search_state(query, effective_config, output);
  // beam search 的三个不变量（缺一不可，与 Vamana / DiskANN 的实现一致）：
  //   1. 候选堆按全精度距离排序（入口点在读页之前用 ADC 估，读到页后立即改正）；
  //   2. 候选比当前最差结果还远就不再扩展 —— 这是"visited"与"beacon"之外的收敛条件；
  //   3. 结果集（best）只由全精度距离决定，ADC 只用于挑选"下一个要打开的页"。
  // 旧实现缺少第 1、3 条：结果全程按 ADC 近似值排序且永不重排，于是返回的
  // distance 与 id 对不上，召回率在 1M 规模上塌到 0.0002。
  const std::size_t expansion_limit =
      std::max<std::size_t>(config.search_width, effective_beam_width);
  const std::size_t beam = std::max<std::size_t>(1, effective_beam_width);
  // 近似值改正必须计入预算，否则它们可以无限自循环（见下方改正分支）。
  constexpr std::size_t kApproximateCorrectionLimit = 1000;
  // 上限取固定的 1000：一次查询里需要改正的点数不该超过搜索宽度的量级，
  // 超过就说明存在持续抖动，此时宁可放弃本次搜索也不要空转。
  std::size_t approximate_corrections = 0;
  std::vector<SearchResult> batch;
  while (!state->candidates.empty() &&
         output.stats.expanded < expansion_limit) {
    batch.clear();
    std::size_t emitted = 0;
    while (!state->candidates.empty() && emitted < beam) {
      const SearchResult front = state->candidates.top();
      // 收敛：最前面的候选已经比当前第 k 名更远（Vamana 的 relaxed 条件），
      //     继续扩展只可能走得更远，整条搜索即可结束。这个判断只看"最前面的
      //     候选"，与 beam 宽度无关：若对整批都判断，beam 宽时批次还没凑满，
      //     front 早就不比 worst 近（worst 尚未被替换），搜索会一路扩展到上限。
      //     批内不再单独丢弃候选：ADC 下界太松，据此剪枝把召回率从 0.90 打到
      //     0.28（见 docs/problem/phase0-measurements-2026-10-26.md）。
      if (emitted == 0 && state->best.size() >= state->effective_k &&
          front.distance > state->best.top().distance) {
        if (trace_results) {
          std::fprintf(stderr,
                       "trace convergence: expanded=%zu best_size=%zu "
                       "front=%.3f worst_w=%.3f topk=",
                       output.stats.expanded, state->best.size(),
                       static_cast<double>(front.distance),
                       static_cast<double>(state->best.top().distance));
          for (const auto& result : sorted_results(state->best)) {
            std::fprintf(stderr, "%u/%.3f ", result.id,
                         static_cast<double>(result.distance));
          }
          std::fprintf(stderr, "\n");
        }
        state->candidates = {};
        break;
      }
      // 先在堆顶做判断，确认要扩展才 pop：beam 名额用完时堆顶必须留在堆里，
      // 它是最近的那个候选 —— 若误弹出就会把它丢掉，下一轮却去扩展更远的点。
      // （实测 beam=64 时每次弹出的第一个候选被丢弃，召回率从 0.86 掉到 0.05。）
      if (front.approximate) {
        state->candidates.pop();
        // 兜底：候选堆里理论上只有精确距离（入口与邻居都在读页时就算了），
        // 走到这里说明有路径漏了精确化。就地改正后重新入堆，而不是带着
        // 近似值参与扩展决策。
        ++approximate_corrections;
        ++output.stats.candidate_corrections;
        state->candidates.push(
            SearchResult{front.id, state->exact_distance(front.id), false});
        ++output.stats.candidate_pushes;
        if (approximate_corrections > kApproximateCorrectionLimit) {
          // 改正步不计入扩展预算，但必须有上界，否则一个持续的近似/精确
          // 距离抖动就会让内层循环空转：状态停在 expanded=0、batch=0，
          // 整个搜索返回空结果。日志留痕，便于把真正的漏精确化找出来。
          std::fprintf(stderr,
                       "warning: abandoned packed graph search after %zu "
                       "approximate-distance corrections\n",
                       approximate_corrections);
          state->candidates = {};
          break;
        }
        continue;
      }
      state->candidates.pop();
      ++output.stats.candidate_pops;
      batch.push_back(front);
      ++emitted;
    }
    if (batch.empty()) {
      break;
    }

    std::vector<std::uint32_t> page_ids;
    page_ids.reserve(batch.size());
    for (const auto& candidate : batch) {
      page_ids.push_back(state->session.page_for_node(candidate.id));
    }
    state->session.ensure_pages_loaded_batch(
        page_ids, DemandReadStage::kCandidateExpand);

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
