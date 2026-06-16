#pragma once

#include <cstddef>
#include <vector>

#include "agentmem/core/types.h"

namespace agentmem {

class BruteForceIndex {
 public:
  explicit BruteForceIndex(const VectorSet& base);

  std::vector<SearchResult> search_one(const float* query, std::size_t k) const;

  std::vector<std::vector<SearchResult>> search_batch(const VectorSet& queries,
                                                      std::size_t k) const;

 private:
  const VectorSet& base_;
};

float squared_l2(const float* lhs, const float* rhs, std::size_t dim);

std::vector<SearchResult> search_memory_fast(const VectorSet& base,
                                             const float* query,
                                             std::size_t k);

std::vector<std::vector<SearchResult>> search_memory_fast_batch(
    const VectorSet& base, const VectorSet& queries, std::size_t k);

}  // namespace agentmem
