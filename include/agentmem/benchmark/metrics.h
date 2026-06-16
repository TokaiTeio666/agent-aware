#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "agentmem/core/types.h"

namespace agentmem {

struct LatencyStats {
  double avg_ms = 0.0;
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
};

double recall_at_k(const std::vector<std::vector<SearchResult>>& results,
                   const std::vector<std::vector<std::uint32_t>>& truth,
                   std::size_t k);

LatencyStats summarize_latency(const std::vector<double>& latencies_ms);

double queries_per_second(std::size_t query_count, double elapsed_seconds);

}  // namespace agentmem
