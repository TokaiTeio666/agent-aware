#include "agentmem/storage/lsm_tree.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "agentmem/core/brute_force.h"

namespace agentmem {
namespace {

constexpr char kWalMagic[8] = {'A', 'M', 'F', 'W', 'A', 'L', 'V', '5'};  // 文件签名。
constexpr std::uint32_t kWalVersion = 5;       // WAL 二进制格式版本。
constexpr std::uint32_t kWalInsertRecord = 1;  // 记录包含完整向量。
constexpr std::uint32_t kWalDeleteRecord = 2;  // 记录仅包含待删除 id。

struct WorseFirst {
  bool operator()(const SearchResult& lhs, const SearchResult& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id < rhs.id;
    }
    return lhs.distance < rhs.distance;
  }
};

template <typename T>
void write_value(std::ofstream& output, const T& value,
                 const std::string& path) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(T));
  if (!output) {
    throw std::runtime_error("Failed to write WAL: " + path);
  }
}

template <typename T>
T read_value(std::ifstream& input, const std::string& path) {
  T value{};
  input.read(reinterpret_cast<char*>(&value), sizeof(T));
  if (!input) {
    throw std::runtime_error("Failed to read WAL: " + path);
  }
  return value;
}

void write_wal_header(std::ofstream& output, std::size_t dim,
                      const std::string& path, WalStats& stats) {
  output.write(kWalMagic, sizeof(kWalMagic));  // header: magic + version + dim。
  write_value(output, kWalVersion, path);
  write_value(output, static_cast<std::uint32_t>(dim), path);
  stats.bytes +=
      sizeof(kWalMagic) + sizeof(kWalVersion) + sizeof(std::uint32_t);
}

std::vector<SearchResult> sorted_heap(
    std::priority_queue<SearchResult, std::vector<SearchResult>, WorseFirst>&
        heap) {
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

WalWriter::WalWriter(const std::string& path, std::size_t dim)
    : WalWriter(path, dim, false, WalStats{}) {}

WalWriter::WalWriter(const std::string& path, std::size_t dim, bool append,
                     const WalStats& initial_stats)
    : path_(path), dim_(dim), stats_(initial_stats) {
  if (dim_ == 0) {
    throw std::runtime_error("WAL dimension must be positive");
  }
  const auto mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
  output_.open(path_, mode);
  if (!output_) {
    throw std::runtime_error("Cannot create WAL file: " + path_);
  }

  if (!append || stats_.bytes == 0) {
    stats_ = {};
    write_wal_header(output_, dim_, path_, stats_);
  }
}

void WalWriter::append_insert(std::uint32_t id, const float* vector) {
  const std::uint32_t bytes =
      static_cast<std::uint32_t>(dim_ * sizeof(float));
  write_value(output_, kWalInsertRecord, path_);  // record: type + id + dim + bytes + vector。
  write_value(output_, id, path_);
  write_value(output_, static_cast<std::uint32_t>(dim_), path_);
  write_value(output_, bytes, path_);
  output_.write(reinterpret_cast<const char*>(vector), bytes);
  if (!output_) {
    throw std::runtime_error("Failed to append insert record to WAL: " +
                             path_);
  }
  ++stats_.records;
  stats_.bytes += sizeof(std::uint32_t) * 4 + bytes;
}

void WalWriter::append_delete(std::uint32_t id) {
  write_value(output_, kWalDeleteRecord, path_);  // delete record 的 bytes 固定为 0。
  write_value(output_, id, path_);
  write_value(output_, static_cast<std::uint32_t>(dim_), path_);
  write_value(output_, static_cast<std::uint32_t>(0), path_);
  ++stats_.records;
  stats_.bytes += sizeof(std::uint32_t) * 4;
}

void WalWriter::flush() {
  output_.flush();
  if (!output_) {
    throw std::runtime_error("Failed to flush WAL: " + path_);
  }
}

WalReplayStats replay_wal_inserts(
    const std::string& path, std::size_t expected_dim,
    const std::function<void(std::uint32_t, const float*)>& on_insert) {
  return replay_wal_records(path, expected_dim, on_insert,
                            [](std::uint32_t) {});
}

WalReplayStats replay_wal_records(
    const std::string& path, std::size_t expected_dim,
    const std::function<void(std::uint32_t, const float*)>& on_insert,
    const std::function<void(std::uint32_t)>& on_delete) {
  if (expected_dim == 0) {
    throw std::runtime_error("WAL replay dimension must be positive");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open WAL file for replay: " + path);
  }

  char magic[sizeof(kWalMagic)]{};
  input.read(magic, sizeof(magic));
  if (!input || std::memcmp(magic, kWalMagic, sizeof(kWalMagic)) != 0) {
    throw std::runtime_error("Invalid WAL magic: " + path);
  }
  const auto version = read_value<std::uint32_t>(input, path);
  if (version != kWalVersion) {
    throw std::runtime_error("Unsupported WAL version in " + path);
  }
  const auto dim = read_value<std::uint32_t>(input, path);
  if (dim != expected_dim) {
    throw std::runtime_error("WAL dimension does not match current dataset");
  }

  WalReplayStats stats;
  stats.dim = dim;
  stats.bytes =
      sizeof(kWalMagic) + sizeof(kWalVersion) + sizeof(std::uint32_t);

  std::vector<float> vector(dim, 0.0f);
  while (input.peek() != EOF) {
    const auto record_type = read_value<std::uint32_t>(input, path);
    const auto id = read_value<std::uint32_t>(input, path);
    const auto record_dim = read_value<std::uint32_t>(input, path);
    const auto bytes = read_value<std::uint32_t>(input, path);
    stats.bytes += sizeof(std::uint32_t) * 4;

    if (record_type != kWalInsertRecord && record_type != kWalDeleteRecord) {
      throw std::runtime_error("Unsupported WAL record type in " + path);
    }
    if (record_dim != dim) {
      throw std::runtime_error("Invalid WAL record dimension in " + path);
    }
    if (record_type == kWalDeleteRecord) {
      if (bytes != 0) {
        throw std::runtime_error("Invalid WAL delete record shape in " + path);
      }
      ++stats.records;
      ++stats.deletes;
      stats.has_records = true;
      stats.max_id = stats.records == 1 ? id : std::max(stats.max_id, id);
      on_delete(id);  // replay 只负责解析，恢复策略由调用方注入。
      continue;
    }
    if (bytes != dim * sizeof(float)) {
      throw std::runtime_error("Invalid WAL insert record shape in " + path);
    }

    input.read(reinterpret_cast<char*>(vector.data()), bytes);
    if (!input) {
      throw std::runtime_error("Truncated WAL insert vector in " + path);
    }
    stats.bytes += bytes;
    ++stats.records;
    ++stats.inserts;
    stats.has_records = true;
    stats.max_id = stats.records == 1 ? id : std::max(stats.max_id, id);
    on_insert(id, vector.data());  // 回调必须在下一次读取覆盖缓冲区前完成拷贝。
  }

  return stats;
}

DeltaFlatIndex::DeltaFlatIndex(std::size_t dim) : dim_(dim) {
  if (dim_ == 0) {
    throw std::runtime_error("DeltaFlatIndex dimension must be positive");
  }
}

void DeltaFlatIndex::insert(std::uint32_t id, const float* vector) {
  active_ids_.push_back(id);  // 新写入先进入 active delta。
  const std::size_t old_size = active_values_.size();
  active_values_.resize(old_size + dim_);
  std::memcpy(active_values_.data() + old_size, vector, dim_ * sizeof(float));
}

std::size_t DeltaFlatIndex::compact_active_to_sealed(
    std::size_t max_vectors) {
  const std::size_t moved = std::min(max_vectors, active_ids_.size());
  if (moved == 0) {
    return 0;
  }

  sealed_ids_.insert(sealed_ids_.end(), active_ids_.begin(),  // 批量转入只读 sealed 区。
                     active_ids_.begin() + static_cast<std::ptrdiff_t>(moved));
  sealed_values_.insert(sealed_values_.end(), active_values_.begin(),
                        active_values_.begin() +
                            static_cast<std::ptrdiff_t>(moved * dim_));

  active_ids_.erase(active_ids_.begin(),
                    active_ids_.begin() + static_cast<std::ptrdiff_t>(moved));
  active_values_.erase(
      active_values_.begin(),
      active_values_.begin() + static_cast<std::ptrdiff_t>(moved * dim_));
  return moved;
}

std::vector<SearchResult> DeltaFlatIndex::search_block(
    const std::vector<std::uint32_t>& ids, const std::vector<float>& values,
    const float* query, std::size_t k) const {
  if (k == 0 || ids.empty()) {
    return {};
  }

  const std::size_t effective_k = std::min(k, ids.size());
  std::priority_queue<SearchResult, std::vector<SearchResult>, WorseFirst> heap;

  for (std::size_t i = 0; i < ids.size(); ++i) {
    const float* row = values.data() + i * dim_;
    const SearchResult item{ids[i], squared_l2(query, row, dim_)};
    if (heap.size() < effective_k) {
      heap.push(item);
      continue;
    }
    const auto& worst = heap.top();
    if (item.distance < worst.distance ||
        (item.distance == worst.distance && item.id < worst.id)) {
      heap.pop();
      heap.push(item);
    }
  }

  return sorted_heap(heap);
}

std::vector<SearchResult> DeltaFlatIndex::search_one(const float* query,
                                                     std::size_t k) const {
  const auto active = search_block(active_ids_, active_values_, query, k);
  const auto sealed = search_block(sealed_ids_, sealed_values_, query, k);
  return merge_topk(active, sealed, k);
}

DeltaIvfFlatIndex::DeltaIvfFlatIndex(std::size_t dim,
                                     std::size_t centroid_count,
                                     std::size_t probe_count,
                                     std::size_t train_iterations,
                                     std::size_t rebuild_interval)
    : dim_(dim),
      centroid_count_(centroid_count),
      probe_count_(probe_count),
      train_iterations_(train_iterations),
      rebuild_interval_(rebuild_interval),
      centroids_(centroid_count * dim, 0.0f),
      bucket_ids_(centroid_count),
      bucket_values_(centroid_count) {
  if (dim_ == 0) {
    throw std::runtime_error("DeltaIvfFlatIndex dimension must be positive");
  }
  if (centroid_count_ == 0) {
    throw std::runtime_error("Delta IVF centroid count must be positive");
  }
  if (probe_count_ == 0) {
    throw std::runtime_error("Delta IVF probe count must be positive");
  }
  if (train_iterations_ == 0) {
    throw std::runtime_error("Delta IVF train iterations must be positive");
  }
  if (rebuild_interval_ == 0) {
    throw std::runtime_error("Delta IVF rebuild interval must be positive");
  }
}

void DeltaIvfFlatIndex::insert(std::uint32_t id, const float* vector) {
  all_ids_.push_back(id);  // 保留全量副本，周期性 rebuild 时重新训练。
  const std::size_t old_size = all_values_.size();
  all_values_.resize(old_size + dim_);
  std::memcpy(all_values_.data() + old_size, vector, dim_ * sizeof(float));
  ++size_;
  ++dirty_since_rebuild_;

  if (size_ <= centroid_count_ || dirty_since_rebuild_ >= rebuild_interval_) {
    rebuild();
    return;
  }

  const std::size_t bucket = nearest_centroid(vector);
  bucket_ids_[bucket].push_back(id);
  auto& values = bucket_values_[bucket];
  const std::size_t bucket_old_size = values.size();
  values.resize(bucket_old_size + dim_);
  std::memcpy(values.data() + bucket_old_size, vector, dim_ * sizeof(float));
}

std::size_t DeltaIvfFlatIndex::nearest_centroid(const float* vector) const {
  if (initialized_centroids_ == 0) {
    return 0;
  }
  return nearest_centroid_in_range(vector, initialized_centroids_);
}

std::size_t DeltaIvfFlatIndex::nearest_centroid_in_range(
    const float* vector, std::size_t centroid_count) const {
  std::size_t best = 0;
  float best_distance = std::numeric_limits<float>::max();
  for (std::size_t c = 0; c < centroid_count; ++c) {
    const float distance =
        squared_l2(vector, centroids_.data() + c * dim_, dim_);
    if (distance < best_distance) {
      best_distance = distance;
      best = c;
    }
  }
  return best;
}

const float* DeltaIvfFlatIndex::stored_vector(std::size_t index) const {
  return all_values_.data() + index * dim_;
}

void DeltaIvfFlatIndex::initialize_centroids(std::size_t effective_centroids) {
  if (effective_centroids == 0) {
    return;
  }

  std::memcpy(centroids_.data(), stored_vector(0), dim_ * sizeof(float));  // 最远点初始化。
  std::vector<float> nearest_distances(size_, std::numeric_limits<float>::max());

  for (std::size_t c = 1; c < effective_centroids; ++c) {
    std::size_t farthest = 0;
    float farthest_distance = -1.0f;
    for (std::size_t i = 0; i < size_; ++i) {
      const float distance =
          squared_l2(stored_vector(i), centroids_.data() + (c - 1) * dim_, dim_);
      nearest_distances[i] = std::min(nearest_distances[i], distance);
      if (nearest_distances[i] > farthest_distance) {
        farthest_distance = nearest_distances[i];
        farthest = i;
      }
    }
    std::memcpy(centroids_.data() + c * dim_, stored_vector(farthest),
                dim_ * sizeof(float));
  }
}

void DeltaIvfFlatIndex::rebuild() {
  if (size_ == 0) {
    return;
  }

  initialized_centroids_ = std::min(centroid_count_, size_);
  initialize_centroids(initialized_centroids_);

  std::vector<std::size_t> assignments(size_, 0);
  std::vector<float> sums(initialized_centroids_ * dim_, 0.0f);
  std::vector<std::size_t> counts(initialized_centroids_, 0);

  for (std::size_t iteration = 0; iteration < train_iterations_; ++iteration) {
    std::fill(sums.begin(), sums.end(), 0.0f);
    std::fill(counts.begin(), counts.end(), 0);

    for (std::size_t i = 0; i < size_; ++i) {
      const std::size_t bucket =
          nearest_centroid_in_range(stored_vector(i), initialized_centroids_);
      assignments[i] = bucket;
      ++counts[bucket];
      float* sum = sums.data() + bucket * dim_;
      const float* vector = stored_vector(i);
      for (std::size_t d = 0; d < dim_; ++d) {
        sum[d] += vector[d];
      }
    }

    for (std::size_t c = 0; c < initialized_centroids_; ++c) {
      float* centroid = centroids_.data() + c * dim_;
      if (counts[c] == 0) {
        std::size_t farthest = 0;
        float farthest_distance = -1.0f;
        for (std::size_t i = 0; i < size_; ++i) {
          const float distance =
              squared_l2(stored_vector(i),
                         centroids_.data() + assignments[i] * dim_, dim_);
          if (distance > farthest_distance) {
            farthest_distance = distance;
            farthest = i;
          }
        }
        std::memcpy(centroid, stored_vector(farthest), dim_ * sizeof(float));
        continue;
      }
      const float scale = 1.0f / static_cast<float>(counts[c]);
      const float* sum = sums.data() + c * dim_;
      for (std::size_t d = 0; d < dim_; ++d) {
        centroid[d] = sum[d] * scale;
      }
    }
  }

  for (auto& ids : bucket_ids_) {
    ids.clear();
  }
  for (auto& values : bucket_values_) {
    values.clear();
  }
  for (std::size_t i = 0; i < size_; ++i) {
    const std::size_t bucket =
        nearest_centroid_in_range(stored_vector(i), initialized_centroids_);
    bucket_ids_[bucket].push_back(all_ids_[i]);
    auto& values = bucket_values_[bucket];
    const std::size_t old_size = values.size();
    values.resize(old_size + dim_);
    std::memcpy(values.data() + old_size, stored_vector(i),
                dim_ * sizeof(float));
  }
  dirty_since_rebuild_ = 0;
}

std::vector<std::size_t> DeltaIvfFlatIndex::closest_centroids(
    const float* query) const {
  std::vector<SearchResult> scored;
  scored.reserve(initialized_centroids_);
  for (std::size_t c = 0; c < initialized_centroids_; ++c) {
    scored.push_back(SearchResult{
        static_cast<std::uint32_t>(c),
        squared_l2(query, centroids_.data() + c * dim_, dim_)});
  }
  std::sort(scored.begin(), scored.end(),
            [](const SearchResult& lhs, const SearchResult& rhs) {
              if (lhs.distance == rhs.distance) {
                return lhs.id < rhs.id;
              }
              return lhs.distance < rhs.distance;
            });

  const std::size_t probes =
      std::min({probe_count_, initialized_centroids_, scored.size()});
  std::vector<std::size_t> buckets;
  buckets.reserve(probes);
  for (std::size_t i = 0; i < probes; ++i) {
    buckets.push_back(scored[i].id);
  }
  return buckets;
}

std::vector<SearchResult> DeltaIvfFlatIndex::search_one(
    const float* query, std::size_t k) const {
  if (k == 0 || size_ == 0 || initialized_centroids_ == 0) {
    return {};
  }

  std::priority_queue<SearchResult, std::vector<SearchResult>, WorseFirst> heap;
  const auto buckets = closest_centroids(query);  // IVF 仅扫描最接近的 probe 桶。
  for (const auto bucket : buckets) {
    const auto& ids = bucket_ids_[bucket];
    const auto& values = bucket_values_[bucket];
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const float* row = values.data() + i * dim_;
      const SearchResult item{ids[i], squared_l2(query, row, dim_)};
      if (heap.size() < k) {
        heap.push(item);
        continue;
      }
      const auto& worst = heap.top();
      if (item.distance < worst.distance ||
          (item.distance == worst.distance && item.id < worst.id)) {
        heap.pop();
        heap.push(item);
      }
    }
  }

  return sorted_heap(heap);
}

std::vector<SearchResult> merge_topk(const std::vector<SearchResult>& lhs,
                                     const std::vector<SearchResult>& rhs,
                                     std::size_t k) {
  if (k == 0) {
    return {};
  }

  std::unordered_map<std::uint32_t, float> best_by_id;  // 合并 Main/Delta 时按 id 去重。
  best_by_id.reserve(lhs.size() + rhs.size());

  auto add = [&](const std::vector<SearchResult>& values) {
    for (const auto& value : values) {
      auto found = best_by_id.find(value.id);
      if (found == best_by_id.end() || value.distance < found->second) {
        best_by_id[value.id] = value.distance;
      }
    }
  };

  add(lhs);
  add(rhs);

  std::vector<SearchResult> merged;
  merged.reserve(best_by_id.size());
  for (const auto& item : best_by_id) {
    merged.push_back(SearchResult{item.first, item.second});
  }

  std::sort(merged.begin(), merged.end(),
            [](const SearchResult& lhs_item, const SearchResult& rhs_item) {
              if (lhs_item.distance == rhs_item.distance) {
                return lhs_item.id < rhs_item.id;
              }
              return lhs_item.distance < rhs_item.distance;
            });
  if (merged.size() > k) {
    merged.resize(k);
  }
  return merged;
}

}  // namespace agentmem
