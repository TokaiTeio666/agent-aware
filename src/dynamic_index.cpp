#include "agentmem/dynamic_index.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "agentmem/brute_force.h"

namespace agentmem {
namespace {

constexpr char kWalMagic[8] = {'A', 'M', 'F', 'W', 'A', 'L', 'V', '5'};
constexpr std::uint32_t kWalVersion = 5;
constexpr std::uint32_t kWalInsertRecord = 1;

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
    : path_(path), dim_(dim), output_(path, std::ios::binary | std::ios::trunc) {
  if (dim_ == 0) {
    throw std::runtime_error("WAL dimension must be positive");
  }
  if (!output_) {
    throw std::runtime_error("Cannot create WAL file: " + path_);
  }

  output_.write(kWalMagic, sizeof(kWalMagic));
  write_value(output_, kWalVersion, path_);
  write_value(output_, static_cast<std::uint32_t>(dim_), path_);
  stats_.bytes = sizeof(kWalMagic) + sizeof(kWalVersion) + sizeof(std::uint32_t);
}

void WalWriter::append_insert(std::uint32_t id, const float* vector) {
  const std::uint32_t bytes =
      static_cast<std::uint32_t>(dim_ * sizeof(float));
  write_value(output_, kWalInsertRecord, path_);
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

void WalWriter::flush() {
  output_.flush();
  if (!output_) {
    throw std::runtime_error("Failed to flush WAL: " + path_);
  }
}

DeltaFlatIndex::DeltaFlatIndex(std::size_t dim) : dim_(dim) {
  if (dim_ == 0) {
    throw std::runtime_error("DeltaFlatIndex dimension must be positive");
  }
}

void DeltaFlatIndex::insert(std::uint32_t id, const float* vector) {
  active_ids_.push_back(id);
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

  sealed_ids_.insert(sealed_ids_.end(), active_ids_.begin(),
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

std::vector<SearchResult> merge_topk(const std::vector<SearchResult>& lhs,
                                     const std::vector<SearchResult>& rhs,
                                     std::size_t k) {
  if (k == 0) {
    return {};
  }

  std::unordered_map<std::uint32_t, float> best_by_id;
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
