#include "agentmem/metrics.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace agentmem {
namespace {

double percentile_sorted(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double pos = (static_cast<double>(sorted.size()) - 1.0) * p;  // 线性插值。
  const auto lower = static_cast<std::size_t>(pos);
  const auto upper = std::min(lower + 1, sorted.size() - 1);
  const double fraction = pos - static_cast<double>(lower);
  return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

}  // namespace

double recall_at_k(const std::vector<std::vector<SearchResult>>& results,
                   const std::vector<std::vector<std::uint32_t>>& truth,
                   std::size_t k) {
  if (results.size() != truth.size()) {
    throw std::runtime_error("Result and ground-truth query counts differ");
  }
  if (results.empty() || k == 0) {
    return 0.0;
  }

  double total = 0.0;
  for (std::size_t i = 0; i < results.size(); ++i) {
    std::unordered_set<std::uint32_t> truth_ids;  // 每个 query 单独计算命中率。
    const std::size_t truth_k = std::min(k, truth[i].size());
    for (std::size_t j = 0; j < truth_k; ++j) {
      truth_ids.insert(truth[i][j]);
    }

    std::size_t hits = 0;
    const std::size_t result_k = std::min(k, results[i].size());
    for (std::size_t j = 0; j < result_k; ++j) {
      if (truth_ids.find(results[i][j].id) != truth_ids.end()) {
        ++hits;
      }
    }
    total += truth_k == 0 ? 0.0 : static_cast<double>(hits) / truth_k;  // macro 平均。
  }

  return total / static_cast<double>(results.size());
}

LatencyStats summarize_latency(const std::vector<double>& latencies_ms) {
  LatencyStats stats;
  if (latencies_ms.empty()) {
    return stats;
  }

  std::vector<double> sorted = latencies_ms;  // 不改变调用方保留的原始采样顺序。
  std::sort(sorted.begin(), sorted.end());
  stats.avg_ms =
      std::accumulate(sorted.begin(), sorted.end(), 0.0) /
      static_cast<double>(sorted.size());
  stats.p50_ms = percentile_sorted(sorted, 0.50);
  stats.p95_ms = percentile_sorted(sorted, 0.95);
  stats.p99_ms = percentile_sorted(sorted, 0.99);
  return stats;
}

double queries_per_second(std::size_t query_count, double elapsed_seconds) {
  if (elapsed_seconds <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(query_count) / elapsed_seconds;
}

}  // namespace agentmem
