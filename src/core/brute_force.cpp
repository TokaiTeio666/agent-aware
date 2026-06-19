#include "agent_aware/core/brute_force.h"

#include <algorithm>
#include <queue>
#include <stdexcept>

#if defined(__AVX__) || defined(__SSE2__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <immintrin.h>
#endif

namespace agent_aware {
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
  if (lhs == nullptr || rhs == nullptr) {
    throw std::runtime_error("squared_l2 requires non-null vectors");
  }

  float distance = 0.0f;
  std::size_t i = 0;

#if defined(__AVX__)
  __m256 acc = _mm256_setzero_ps();
  for (; i + 8 <= dim; i += 8) {
    const __m256 left = _mm256_loadu_ps(lhs + i);
    const __m256 right = _mm256_loadu_ps(rhs + i);
    const __m256 diff = _mm256_sub_ps(left, right);
    acc = _mm256_add_ps(acc, _mm256_mul_ps(diff, diff));
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, acc);
  for (float lane : lanes) {
    distance += lane;
  }
#elif defined(__SSE2__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  __m128 acc = _mm_setzero_ps();
  for (; i + 4 <= dim; i += 4) {
    const __m128 left = _mm_loadu_ps(lhs + i);
    const __m128 right = _mm_loadu_ps(rhs + i);
    const __m128 diff = _mm_sub_ps(left, right);
    acc = _mm_add_ps(acc, _mm_mul_ps(diff, diff));
  }
  alignas(16) float lanes[4];
  _mm_store_ps(lanes, acc);
  for (float lane : lanes) {
    distance += lane;
  }
#endif

  for (; i < dim; ++i) {
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
  return search_memory_fast(base_, query, k);
}

std::vector<std::vector<SearchResult>> BruteForceIndex::search_batch(
    const VectorSet& queries, std::size_t k) const {
  return search_memory_fast_batch(base_, queries, k);
}

std::vector<SearchResult> search_memory_fast(const VectorSet& base,
                                             const float* query,
                                             std::size_t k) {
  if (base.empty()) {
    throw std::runtime_error("search_memory_fast requires non-empty base vectors");
  }
  if (query == nullptr) {
    throw std::runtime_error("search_memory_fast requires a non-null query");
  }
  if (k == 0) {
    return {};
  }

  const std::size_t effective_k = std::min(k, base.size());
  std::priority_queue<HeapItem, std::vector<HeapItem>, WorseFirst> heap;

  for (std::size_t i = 0; i < base.size(); ++i) {
    const float distance = squared_l2(query, base.row(i), base.dim);
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

std::vector<std::vector<SearchResult>> search_memory_fast_batch(
    const VectorSet& base, const VectorSet& queries, std::size_t k) {
  if (queries.dim != base.dim) {
    throw std::runtime_error("Query dimension does not match base dimension");
  }

  std::vector<std::vector<SearchResult>> all_results;
  all_results.reserve(queries.size());
  for (std::size_t i = 0; i < queries.size(); ++i) {
    all_results.push_back(search_memory_fast(base, queries.row(i), k));
  }
  return all_results;
}

}  // namespace agent_aware
