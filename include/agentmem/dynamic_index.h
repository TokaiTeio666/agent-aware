#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "agentmem/types.h"

namespace agentmem {

struct WalStats {
  std::size_t records = 0;
  std::size_t bytes = 0;
};

class WalWriter {
 public:
  WalWriter(const std::string& path, std::size_t dim);

  void append_insert(std::uint32_t id, const float* vector);
  void flush();

  const WalStats& stats() const {
    return stats_;
  }

 private:
  std::string path_;
  std::size_t dim_ = 0;
  std::ofstream output_;
  WalStats stats_;
};

class DeltaFlatIndex {
 public:
  explicit DeltaFlatIndex(std::size_t dim);

  void insert(std::uint32_t id, const float* vector);
  std::size_t compact_active_to_sealed(std::size_t max_vectors);

  std::vector<SearchResult> search_one(const float* query,
                                       std::size_t k) const;

  std::size_t active_size() const {
    return active_ids_.size();
  }

  std::size_t sealed_size() const {
    return sealed_ids_.size();
  }

  std::size_t total_size() const {
    return active_size() + sealed_size();
  }

 private:
  std::vector<SearchResult> search_block(const std::vector<std::uint32_t>& ids,
                                         const std::vector<float>& values,
                                         const float* query,
                                         std::size_t k) const;

  std::size_t dim_ = 0;
  std::vector<std::uint32_t> active_ids_;
  std::vector<float> active_values_;
  std::vector<std::uint32_t> sealed_ids_;
  std::vector<float> sealed_values_;
};

std::vector<SearchResult> merge_topk(const std::vector<SearchResult>& lhs,
                                     const std::vector<SearchResult>& rhs,
                                     std::size_t k);

}  // namespace agentmem
