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
#include "core/reader_context.h"
#include "core/top_l_candidate_pool.h"
#include "graph/disk_page_codec.h"
#include "graph/vamana_builder.h"

namespace agent_aware {
namespace {

constexpr std::uint32_t kNoFrame = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint8_t kNoQueue = 0;
constexpr std::uint8_t kA1Cold = 1;
constexpr std::uint8_t kA1Hot = 2;
constexpr std::uint8_t kAmCold = 3;
constexpr std::uint8_t kAmHot = 4;

std::mutex shared_pq_registry_mutex;
std::unordered_map<std::string, std::weak_ptr<const PqAdcModel>>
    shared_pq_registry;
std::atomic<std::uint64_t> shared_pq_disk_load_count{0};

std::string pq_registry_key(const std::string& path,
                            std::uint64_t index_bytes) {
  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  const auto modified = std::filesystem::last_write_time(path)
                            .time_since_epoch()
                            .count();
  return absolute.string() + ":" + std::to_string(index_bytes) + ":" +
         std::to_string(modified);
}

std::shared_ptr<const PqAdcModel> load_shared_pq_model(
    const std::string& path, std::uint64_t index_bytes,
    std::uint64_t pq_offset, std::ifstream& input) {
  const std::string key = pq_registry_key(path, index_bytes);
  std::lock_guard<std::mutex> lock(shared_pq_registry_mutex);
  if (const auto found = shared_pq_registry.find(key);
      found != shared_pq_registry.end()) {
    if (auto model = found->second.lock()) {
      return model;
    }
    shared_pq_registry.erase(found);
  }

  input.clear();
  input.seekg(static_cast<std::streamoff>(pq_offset), std::ios::beg);
  auto model = std::make_shared<const PqAdcModel>(PqAdcModel::read(input));
  shared_pq_disk_load_count.fetch_add(1, std::memory_order_relaxed);
  shared_pq_registry.emplace(key, model);
  return model;
}

// PackedDiskGraphIndex 查询 V2 页布局，组合 PQ/ADC 候选打分、页 cache 复用和单 query 异步预取会话。
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

std::uint64_t estimate_page_cache_bytes(std::size_t page_size,
                                        std::size_t capacity_pages) {
  // Frame metadata, page->frame hash node/bucket, free-list slot and the
  // bounded A1out membership structure. Payload remains the dominant cost.
  constexpr std::size_t kManagementBytesPerFrame = 96;
  return static_cast<std::uint64_t>(capacity_pages) *
         (page_size + kManagementBytesPerFrame);
}

struct PackedDiskGraphIndex::SearchState {
  SearchState(PackedDiskGraphIndex& index, const float* query,
              const DiskGraphSearchConfig& config,
              DiskGraphSearchResult& output)
      : index(index),
        query(query),
        config(config),
        output(output),
        context(static_cast<std::size_t>(index.metadata_.vector_count), config,
                output.stats),
        session(index, config, output.stats),
        pool(context.limits().max_search_list_size),
        logical_l(context.limits().search_list_size) {
    session.apply_workspace_limits(context.limits().prefetch_top_k,
                                   context.limits().prefetch_max_inflight);
    staging.reserve(context.limits().beam_width * index.metadata_.degree);
  }

  // SearchState 保存单 query 可变遍历状态；共享的 index 对象只负责索引元数据和 cache。
  const QueryPageSession::Node& load_node(std::uint32_t id) {
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

  void merge_staging() {
    output.stats.candidate_staging_peak_size = std::max(
        output.stats.candidate_staging_peak_size, staging.size());
    const CandidatePoolMergeStats merged = pool.merge(staging);
    output.stats.candidate_admitted_count += merged.admitted;
    output.stats.candidate_replacement_count += merged.replacements;
    output.stats.candidate_rejected_count += merged.rejected;
    output.stats.candidate_pool_peak_size =
        std::max(output.stats.candidate_pool_peak_size, pool.size());
  }

  std::size_t logical_limit() const {
    return std::min(logical_l, pool.size());
  }

  std::size_t unexpanded_in_logical_prefix() const {
    std::size_t count = 0;
    const auto& values = pool.values();
    for (std::size_t i = 0; i < logical_limit(); ++i) {
      if (!context.is_expanded(values[i].id)) ++count;
    }
    return count;
  }

  bool grow_logical_l() {
    if (logical_l >= context.limits().max_search_list_size) return false;
    const std::size_t doubled =
        logical_l > context.limits().max_search_list_size / 2
            ? context.limits().max_search_list_size
            : logical_l * 2;
    logical_l = std::min(context.limits().max_search_list_size,
                         std::max(logical_l + 1, doubled));
    ++output.stats.candidate_logical_l_growth_count;
    return true;
  }

  bool converged() const {
    if (pool.size() < result_capacity) return false;
    if (output.stats.expanded < logical_limit()) return false;
    const auto& values = pool.values();
    for (std::size_t i = 0; i < logical_limit(); ++i) {
      if (!context.is_expanded(values[i].id)) {
        return !candidate_is_better(values[i], values[result_capacity - 1]);
      }
    }
    return true;
  }

  PackedDiskGraphIndex& index;
  const float* query = nullptr;
  const DiskGraphSearchConfig& config;
  DiskGraphSearchResult& output;
  ReaderContext context;
  QueryPageSession session;
  TopLCandidatePool pool;
  std::vector<SearchResult> staging;
  std::vector<float> adc_table;
  std::size_t logical_l = 0;
  std::size_t effective_k = 0;
  std::size_t result_capacity = 0;
};

PackedDiskGraphIndex::PackedDiskGraphIndex(
    const std::string& path,
    std::shared_ptr<const PqAdcModel> shared_pq_model)
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
  if (shared_pq_model) {
    pq_model_ = std::move(shared_pq_model);
  } else {
    pq_model_ = load_shared_pq_model(path_, metadata_.index_bytes,
                                     metadata_.pq_offset, input_);
  }
  if (pq_model_->dim() != metadata_.pq_dim ||
      pq_model_->subspaces() != metadata_.pq_subspaces ||
      pq_model_->centroids() != metadata_.pq_centroids ||
      pq_model_->vector_count() != metadata_.vector_count ||
      metadata_.pq_offsets_bytes !=
          (pq_model_->subspaces() + 1) * sizeof(std::uint64_t) ||
      pq_model_->codebook_bytes() != metadata_.pq_codebook_bytes ||
      pq_model_->code_bytes() != metadata_.pq_codes_bytes) {
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

std::uint64_t PackedDiskGraphIndex::pq_disk_load_count() {
  return shared_pq_disk_load_count.load(std::memory_order_relaxed);
}

void PackedDiskGraphIndex::configure_cache(const std::string& policy,
                                           std::size_t capacity_pages,
                                           bool protect_hot_pages,
                                           std::size_t hot_degree_threshold) {
  std::unique_lock<std::shared_mutex> runtime_lock(runtime_mutex_);
  if (policy != "none" && policy != "lru" && policy != "2q" &&
      policy != "graph-aware-2q" && policy != "agent") {
    throw std::runtime_error(
        "Cache policy must be none, lru, 2q, or graph-aware-2q");
  }
  const bool threshold_changed =
      hot_degree_threshold != cache_hot_degree_threshold_;
  if (threshold_changed) page_hot_classes_.clear();
  cache_hot_degree_threshold_ = hot_degree_threshold;
  if (policy == "graph-aware-2q" || policy == "agent" ||
      protect_hot_pages) {
    // V3 没有持久化页热度，首次启用时聚合一次，随后释放节点级入度。
    ensure_page_hot_scores();
  }
  std::lock_guard<std::mutex> lock(cache_mutex_);
  const bool changed =
      policy != cache_policy_ || capacity_pages != cache_capacity_pages_ ||
      protect_hot_pages != cache_protect_hot_pages_ ||
      threshold_changed;
  cache_policy_ = policy;
  cache_capacity_pages_ = capacity_pages;
  cache_protect_hot_pages_ = protect_hot_pages;
  if (changed || cache_frames_.size() != capacity_pages) reset_cache_locked();
}

void PackedDiskGraphIndex::reset_cache_locked() {
  page_cache_.clear();
  page_cache_.reserve(cache_capacity_pages_);
  cache_frames_.clear();
  cache_frames_.resize(cache_capacity_pages_);
  cache_free_frames_.clear();
  cache_free_frames_.reserve(cache_capacity_pages_);
  for (std::size_t i = cache_capacity_pages_; i > 0; --i) {
    cache_free_frames_.push_back(static_cast<std::uint32_t>(i - 1));
  }
  for (auto& queue : cache_queues_) queue = CacheQueue{};
  const std::size_t ghost_capacity = cache_capacity_pages_ / 4;
  ghost_ring_.assign(ghost_capacity, kNoFrame);
  ghost_pages_.clear();
  ghost_pages_.reserve(ghost_capacity);
  ghost_cursor_ = 0;
  probationary_target_ = std::max<std::size_t>(1, cache_capacity_pages_ / 5);
  probationary_target_ = std::min(probationary_target_, cache_capacity_pages_);
  hub_target_ = cache_capacity_pages_ * 3 / 10;
}

void PackedDiskGraphIndex::queue_push_front_locked(std::uint8_t queue_id,
                                                    std::uint32_t frame) {
  if (queue_id == kNoQueue || frame >= cache_frames_.size()) return;
  CacheQueue& queue = cache_queues_[queue_id];
  CacheEntry& entry = cache_frames_[frame];
  entry.queue = queue_id;
  entry.prev = kNoFrame;
  entry.next = queue.head;
  if (queue.head != kNoFrame) cache_frames_[queue.head].prev = frame;
  else queue.tail = frame;
  queue.head = frame;
  ++queue.size;
}

void PackedDiskGraphIndex::queue_remove_locked(std::uint32_t frame) {
  CacheEntry& entry = cache_frames_[frame];
  if (entry.queue == kNoQueue) return;
  CacheQueue& queue = cache_queues_[entry.queue];
  if (entry.prev != kNoFrame) cache_frames_[entry.prev].next = entry.next;
  else queue.head = entry.next;
  if (entry.next != kNoFrame) cache_frames_[entry.next].prev = entry.prev;
  else queue.tail = entry.prev;
  --queue.size;
  entry.prev = entry.next = kNoFrame;
  entry.queue = kNoQueue;
}

void PackedDiskGraphIndex::queue_touch_locked(std::uint32_t frame) {
  const std::uint8_t queue = cache_frames_[frame].queue;
  queue_remove_locked(frame);
  queue_push_front_locked(queue, frame);
}

std::uint32_t PackedDiskGraphIndex::queue_victim_locked(
    std::uint8_t queue) const {
  return cache_queues_[queue].tail;
}

std::uint8_t PackedDiskGraphIndex::admission_queue_locked(
    bool hot, bool ghost_hit) const {
  if (cache_policy_ == "lru") return kAmCold;
  if (ghost_hit && is_two_queue_policy()) return hot ? kAmHot : kAmCold;
  return hot && is_graph_aware_cache_policy() ? kA1Hot : kA1Cold;
}

void PackedDiskGraphIndex::remember_ghost_locked(std::uint32_t page_id) {
  if (ghost_ring_.empty()) return;
  const std::uint32_t old = ghost_ring_[ghost_cursor_];
  if (old != kNoFrame) {
    const auto found = ghost_pages_.find(old);
    if (found != ghost_pages_.end() && found->second == ghost_cursor_) {
      ghost_pages_.erase(found);
    }
  }
  ghost_ring_[ghost_cursor_] = page_id;
  ghost_pages_.insert_or_assign(page_id, ghost_cursor_);
  ghost_cursor_ = (ghost_cursor_ + 1) % ghost_ring_.size();
}

bool PackedDiskGraphIndex::consume_ghost_locked(std::uint32_t page_id) {
  return ghost_pages_.erase(page_id) != 0;
}

bool PackedDiskGraphIndex::pin_if_cached(std::uint32_t page_id) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto found = page_cache_.find(GraphPageIdentity{0, 0, page_id});
  if (found == page_cache_.end()) {
    return false;
  }
  CacheEntry& entry = cache_frames_[found->second];
  if (entry.pin_count == std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("Packed graph page pin overflow");
  }
  ++entry.pin_count;
  entry.referenced = true;
  return true;
}

void PackedDiskGraphIndex::unpin_if_cached(std::uint32_t page_id) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto found = page_cache_.find(GraphPageIdentity{0, 0, page_id});
  if (found == page_cache_.end()) {
    return;
  }
  CacheEntry& entry = cache_frames_[found->second];
  if (entry.pin_count == 0) {
    throw std::runtime_error("Packed graph page pin underflow");
  }
  --entry.pin_count;
}

void PackedDiskGraphIndex::configure_io(const std::string& mode,
                                        std::size_t batch_size,
                                        std::size_t io_depth) {
  std::unique_lock<std::shared_mutex> runtime_lock(runtime_mutex_);
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
    for (std::uint32_t slot = 0; slot < page.node_count; ++slot) {
      const DiskNode node = DiskPageCodec::node_at(metadata_, page, slot);
      const std::uint32_t* neighbors = neighbor_data(page, node);
      for (std::uint32_t i = 0; i < node.neighbor_count; ++i) {
        const std::uint32_t neighbor = neighbors[i];
        if (neighbor < node_in_degrees_.size()) {
          ++node_in_degrees_[neighbor];
        }
      }
    }
  }
  node_in_degrees_ready_ = true;
}

void PackedDiskGraphIndex::ensure_page_hot_scores() {
  if (!page_hot_classes_.empty()) return;
  ensure_node_in_degrees();
  std::vector<std::uint32_t> page_scores(
      static_cast<std::size_t>(metadata_.page_count), 0);
  for (std::size_t node_id = 0; node_id < node_to_page_.size(); ++node_id) {
    const std::uint32_t page_id = node_to_page_[node_id];
    if (page_id >= page_scores.size()) continue;
    const std::uint32_t degree = node_in_degrees_[node_id];
    if (cache_hot_degree_threshold_ == 0) {
      page_scores[page_id] += degree;
    } else if (degree >= cache_hot_degree_threshold_) {
      ++page_scores[page_id];
    }
  }
  std::uint32_t cutoff = 1;
  if (cache_hot_degree_threshold_ == 0 && !page_scores.empty()) {
    const std::uint64_t total = std::accumulate(
        page_scores.begin(), page_scores.end(), std::uint64_t{0});
    cutoff = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, (total + page_scores.size() - 1) /
                                       page_scores.size()));
  }
  page_hot_classes_.resize(page_scores.size());
  for (std::size_t page = 0; page < page_scores.size(); ++page) {
    page_hot_classes_[page] = page_scores[page] >= cutoff ? 1 : 0;
  }
  node_in_degrees_.clear();
  node_in_degrees_.shrink_to_fit();
  node_in_degrees_ready_ = false;
}

PackedDiskGraphIndex::DiskNode PackedDiskGraphIndex::find_node_in_page(
    const DecodedPage& page, std::uint32_t node_id) const {
  return DiskPageCodec::find_node(metadata_, page, node_id);
}

const float* PackedDiskGraphIndex::vector_data(const DecodedPage& page,
                                               const DiskNode& node) const {
  return DiskPageCodec::vector_data(metadata_, page, node);
}

const std::uint32_t* PackedDiskGraphIndex::neighbor_data(
    const DecodedPage& page, const DiskNode& node) const {
  const std::size_t bytes = static_cast<std::size_t>(node.neighbor_count) *
                            sizeof(std::uint32_t);
  if (node.neighbor_offset + bytes > page.size()) {
    throw std::runtime_error("Packed node neighbors point outside page");
  }
  return reinterpret_cast<const std::uint32_t*>(
      page.data() + node.neighbor_offset);
}

float PackedDiskGraphIndex::compute_distance_direct(
    const float* query, const DecodedPage& page, const DiskNode& node) const {
  return squared_l2(query, vector_data(page, node), metadata_.dim);
}

std::shared_ptr<const PackedDiskGraphIndex::DecodedPage>
PackedDiskGraphIndex::lookup_cached_page(std::uint32_t page_id,
                                         DiskGraphSearchStats& stats) {
  if (!cache_enabled()) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto found = page_cache_.find(GraphPageIdentity{0, 0, page_id});
  if (found == page_cache_.end()) {
    return nullptr;
  }

  CacheEntry& entry = cache_frames_[found->second];
  ++stats.page_cache_hits;
  ++stats.cache_hits;
  entry.referenced = true;
  return entry.page;
}

std::shared_ptr<const PackedDiskGraphIndex::DecodedPage>
PackedDiskGraphIndex::store_cached_page(DecodedPage page,
                                        DiskGraphSearchStats& stats) {
  if (!cache_enabled()) {
    return std::make_shared<DecodedPage>(std::move(page));
  }

  const std::uint32_t page_id = page.page_id;
  std::lock_guard<std::mutex> lock(cache_mutex_);
  const GraphPageIdentity key{0, 0, page_id};
  auto found = page_cache_.find(key);
  if (found != page_cache_.end()) {
    CacheEntry& entry = cache_frames_[found->second];
    if (page.bytes.size() != metadata_.page_size) {
      throw std::runtime_error("Packed cache page has unexpected size");
    }
    entry.page = std::make_shared<DecodedPage>(std::move(page));
    entry.referenced = true;
    return entry.page;
  }

  if (cache_free_frames_.empty()) {
    if (!evict_one_page_locked(&stats)) {
      return std::make_shared<DecodedPage>(std::move(page));
    }
  }

  const bool ghost_hit = consume_ghost_locked(page_id);
  const std::uint32_t frame = cache_free_frames_.back();
  cache_free_frames_.pop_back();
  CacheEntry& entry = cache_frames_[frame];
  entry.page = std::make_shared<DecodedPage>(std::move(page));
  if (entry.page->bytes.size() != metadata_.page_size) {
    throw std::runtime_error("Packed cache page has unexpected size");
  }
  entry.occupied = true;
  entry.hot = page_hot_score(*entry.page) > 0;
  entry.pin_count = 0;
  entry.saved_queue = kNoQueue;
  entry.referenced = true;
  page_cache_.emplace(key, frame);
  queue_push_front_locked(admission_queue_locked(entry.hot, ghost_hit), frame);
  return entry.page;
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
  return page.page_id < page_hot_classes_.size()
             ? page_hot_classes_[page.page_id]
             : 0;
}

bool PackedDiskGraphIndex::evict_one_page(DiskGraphSearchStats* stats) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  return evict_one_page_locked(stats);
}

bool PackedDiskGraphIndex::evict_one_page_locked(DiskGraphSearchStats* stats) {
  if (page_cache_.empty()) {
    return false;
  }

  const std::size_t probationary = cache_queues_[kA1Cold].size +
                                   cache_queues_[kA1Hot].size;
  const std::size_t hot = cache_queues_[kA1Hot].size +
                          cache_queues_[kAmHot].size;
  std::uint32_t victim = kNoFrame;
  if (is_graph_aware_cache_policy() && hot > hub_target_) {
    victim = queue_victim_locked(kA1Hot);
    if (victim == kNoFrame) victim = queue_victim_locked(kAmHot);
  }
  if (victim == kNoFrame && probationary > probationary_target_) {
    victim = queue_victim_locked(kA1Cold);
    if (victim == kNoFrame) victim = queue_victim_locked(kA1Hot);
  }
  if (victim == kNoFrame) victim = queue_victim_locked(kA1Cold);
  if (victim == kNoFrame) victim = queue_victim_locked(kA1Hot);
  if (victim == kNoFrame) victim = queue_victim_locked(kAmCold);
  if (victim == kNoFrame) victim = queue_victim_locked(kAmHot);
  if (victim == kNoFrame) return false;

  // Foreground hits only set referenced. Eviction owns queue movement and
  // grants one second chance without touching a strict LRU chain per hit.
  std::size_t attempts = cache_frames_.size();
  while (attempts-- > 0 && victim != kNoFrame) {
    CacheEntry& candidate = cache_frames_[victim];
    if (candidate.pin_count == 0 && !candidate.referenced) break;
    if (candidate.pin_count == 0) candidate.referenced = false;
    const std::uint8_t queue = candidate.queue;
    queue_touch_locked(victim);
    victim = queue_victim_locked(queue);
  }
  if (victim == kNoFrame || cache_frames_[victim].pin_count != 0 ||
      cache_frames_[victim].referenced) {
    return false;
  }

  CacheEntry& entry = cache_frames_[victim];
  const std::uint32_t page_id = entry.page->page_id;
  const bool probationary_victim =
      entry.queue == kA1Cold || entry.queue == kA1Hot;
  queue_remove_locked(victim);
  page_cache_.erase(GraphPageIdentity{0, 0, page_id});
  if (probationary_victim && is_two_queue_policy()) remember_ghost_locked(page_id);
  entry = CacheEntry{};
  cache_free_frames_.push_back(victim);
  if (stats != nullptr) {
    ++stats->page_cache_evictions;
  }
  return true;
}

std::shared_ptr<const PackedDiskGraphIndex::DecodedPage>
PackedDiskGraphIndex::load_page(
    std::uint32_t page_id, DiskGraphSearchStats& stats) {
  ++stats.page_requests;
  ++stats.page_requests_before_dedup;

  if (auto cached = lookup_cached_page(page_id, stats)) {
    return cached;
  }

  ++stats.page_cache_misses;
  ++stats.demand_reads;
  ++stats.node_reads;
  ++stats.page_requests_after_dedup;
  DecodedPage page = read_page(page_id, stats);
  return store_cached_page(std::move(page), stats);
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
    if (state->context.test_and_set(entry)) {
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
      std::min<std::size_t>(
          std::max(state->effective_k, state->context.limits().rerank_topk),
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
    state->staging.push_back(result);
  }
  state->merge_staging();
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
      state.context.limits().search_width > state.output.stats.expanded
          ? state.context.limits().search_width - state.output.stats.expanded
          : 0;
  const std::size_t batch_limit =
      std::min(effective_beam_width, remaining);
  if (batch_limit == 0) {
    return window;
  }

  const auto& pool = state.pool.values();
  const std::size_t logical_limit = state.logical_limit();
  const std::size_t hot_limit = state.context.limits().candidate_hot_size;
  std::vector<SearchResult> hot;
  hot.reserve(std::min(hot_limit, logical_limit));
  for (std::size_t i = 0; i < logical_limit && hot.size() < hot_limit; ++i) {
    if (!state.context.is_expanded(pool[i].id)) hot.push_back(pool[i]);
  }
  if (hot.empty()) return window;

  std::vector<std::uint32_t> ranked_page_ids;
  ranked_page_ids.reserve(hot.size());
  for (const auto& candidate : hot) {
    ranked_page_ids.push_back(state.session.page_for_node(candidate.id));
  }
  const auto ranked_pages =
      rank_topology_pages(ranked_page_ids, ranked_page_ids.size());
  window.reserve(batch_limit);
  for (const auto& page : ranked_pages) {
    for (const auto& candidate : hot) {
      if (state.session.page_for_node(candidate.id) == page.page_id) {
        window.push_back(candidate);
        if (window.size() == batch_limit) break;
      }
    }
    if (window.size() == batch_limit) break;
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
  for (const auto& current : batch) {
    if (!state.context.mark_expanded(current.id)) continue;
    const QueryPageSession::Node& node = state.load_node(current.id);
    const std::uint32_t current_page = state.session.page_for_node(current.id);
    (void)state.session.pin_page(current_page);
    ++state.output.stats.expanded;

    std::vector<std::uint32_t> new_neighbors;
    new_neighbors.reserve(node.neighbors.size());
    for (const auto neighbor_id : node.neighbors) {
      if (neighbor_id >= metadata_.vector_count ||
          state.context.test_and_set(neighbor_id)) {
        continue;
      }
      new_neighbors.push_back(neighbor_id);
    }

    for (const auto neighbor_id : new_neighbors) {
      const SearchResult candidate{neighbor_id,
                                   state.candidate_distance(neighbor_id)};
      if (state.pool.size() < state.pool.capacity() ||
          candidate_is_better(candidate, state.pool.values().back())) {
        state.staging.push_back(candidate);
      } else {
        ++state.output.stats.candidate_rejected_count;
      }
    }
    state.session.unpin_page(current_page);
  }

  state.merge_staging();

  if (state.config.prefetch_policy == "topology") {
    const auto planner_start = std::chrono::steady_clock::now();
    const std::size_t candidate_limit =
        std::max<std::size_t>(1, effective_beam_width) * 2;
    std::vector<std::uint32_t> ranked_page_ids;
    ranked_page_ids.reserve(candidate_limit);
    for (const auto& candidate : state.pool.values()) {
      if (ranked_page_ids.size() == candidate_limit) break;
      if (state.context.is_expanded(candidate.id)) continue;
      if (state.session.is_node_materialized(candidate.id)) continue;
      const std::uint32_t page_id = state.session.page_for_node(candidate.id);
      ranked_page_ids.push_back(page_id);
    }
    state.output.stats.prefetch_candidate_nodes += ranked_page_ids.size();
    const auto ranked_pages =
        rank_topology_pages(ranked_page_ids, ranked_page_ids.size());
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

}

void PackedDiskGraphIndex::finalize_topk(SearchState& state) {
  state.output.stats.visited = state.context.visited_count();
  const auto& pool = state.pool.values();
  const std::size_t result_count = std::min(state.result_capacity, pool.size());
  state.output.topk.assign(pool.begin(), pool.begin() + result_count);
  if (!state.adc_table.empty() && state.context.limits().rerank_topk > 0) {
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
        const QueryPageSession::Node& node =
            state.session.get_node(result.id, true, true);
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
  state.output.stats.candidate_final_logical_l = state.logical_l;
}

DiskGraphSearchResult PackedDiskGraphIndex::search_one(
    const float* query, const DiskGraphSearchConfig& config) {
  std::shared_lock<std::shared_mutex> runtime_lock(runtime_mutex_);
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
    if (!external.enabled() || external.dim() != pq_model_->dim() ||
        external.subspaces() != pq_model_->subspaces() ||
        external.centroids() != pq_model_->centroids() ||
        external.vector_count() != pq_model_->vector_count()) {
      throw std::runtime_error(
          "External PQ model does not match the packed V3 index");
    }
  } else {
    effective_config.pq_model = pq_model_.get();
  }
  effective_config.adc_enable = true;
  const std::size_t effective_beam_width =
      std::clamp(config.beam_width == 0 ? config.search_width
                                         : config.beam_width,
                 std::size_t{1}, config.search_width);

  DiskGraphSearchResult output;
  auto state = initialize_search_state(query, effective_config, output);
  while (output.stats.expanded < state->context.limits().search_width) {
    const std::size_t effective_context_beam =
        state->context.limits().beam_width;
    std::vector<SearchResult> batch =
        select_candidate_batch(*state, effective_context_beam);
    if (batch.empty()) {
      if (state->grow_logical_l()) continue;
      output.stats.candidate_terminated_by_exhaustion = 1;
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

    expand_candidate_batch(*state, batch, effective_context_beam);
    if (state->converged()) {
      output.stats.candidate_terminated_by_convergence = 1;
      break;
    }
    if (state->unexpanded_in_logical_prefix() < effective_context_beam) {
      (void)state->grow_logical_l();
    }
  }
  if (output.stats.expanded >= state->context.limits().search_width) {
    output.stats.candidate_terminated_by_budget = 1;
  }
  finalize_topk(*state);
  return output;
}


}  // namespace agent_aware
