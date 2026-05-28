#include "agentmem/brute_force.h"

#include <algorithm>
#include <queue>
#include <stdexcept>

namespace agentmem {
namespace {

struct HeapItem {
  std::uint32_t id = 0;
  float distance = 0.0f;
};

struct WorseFirst {
  bool operator()(const HeapItem& lhs, const HeapItem& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id < rhs.id;
    }
    return lhs.distance < rhs.distance;
  }
};

}  // namespace

float squared_l2(const float* lhs, const float* rhs, std::size_t dim) {
  float distance = 0.0f;
  for (std::size_t i = 0; i < dim; ++i) {
    const float diff = lhs[i] - rhs[i];
    distance += diff * diff;
  }
  return distance;
}

BruteForceIndex::BruteForceIndex(const VectorSet& base) : base_(base) {
  if (base_.empty()) {
    throw std::runtime_error("BruteForceIndex requires non-empty base vectors");
  }
}

std::vector<SearchResult> BruteForceIndex::search_one(const float* query,
                                                      std::size_t k) const {
  if (k == 0) {
    return {};
  }

  const std::size_t effective_k = std::min(k, base_.size());
  std::priority_queue<HeapItem, std::vector<HeapItem>, WorseFirst> heap;

  for (std::size_t i = 0; i < base_.size(); ++i) {
    const float distance = squared_l2(query, base_.row(i), base_.dim);
    const HeapItem item{static_cast<std::uint32_t>(i), distance};
    if (heap.size() < effective_k) {
      heap.push(item);
    } else {
      const HeapItem& worst = heap.top();
      if (distance < worst.distance ||
          (distance == worst.distance && item.id < worst.id)) {
        heap.pop();
        heap.push(item);
      }
    }
  }

  std::vector<SearchResult> results;
  results.reserve(effective_k);
  while (!heap.empty()) {
    results.push_back(SearchResult{heap.top().id, heap.top().distance});
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

std::vector<std::vector<SearchResult>> BruteForceIndex::search_batch(
    const VectorSet& queries, std::size_t k) const {
  if (queries.dim != base_.dim) {
    throw std::runtime_error("Query dimension does not match base dimension");
  }

  std::vector<std::vector<SearchResult>> all_results;
  all_results.reserve(queries.size());
  for (std::size_t i = 0; i < queries.size(); ++i) {
    all_results.push_back(search_one(queries.row(i), k));
  }
  return all_results;
}

}  // namespace agentmem

