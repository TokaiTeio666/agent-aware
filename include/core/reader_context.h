#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace agent_aware {

struct DiskGraphSearchConfig;
struct DiskGraphSearchStats;

class QueryWorkspaceBudgetExceeded : public std::runtime_error {
 public:
  QueryWorkspaceBudgetExceeded()
      : std::runtime_error("QueryWorkspaceBudgetExceeded") {}
};

class ReaderContext {
 public:
  static constexpr std::size_t kTargetBytes = 3 * 1024 * 1024;
  static constexpr std::size_t kSoftLimitBytes = 4 * 1024 * 1024;
  static constexpr std::size_t kHardLimitBytes = 5 * 1024 * 1024;

  struct EffectiveLimits {
    std::size_t search_width = 0;
    std::size_t beam_width = 0;
    std::size_t rerank_topk = 0;
    std::size_t prefetch_top_k = 0;
    std::size_t prefetch_max_inflight = 0;
    std::size_t candidate_capacity = 0;
    std::size_t search_list_size = 0;
    std::size_t max_search_list_size = 0;
    std::size_t candidate_hot_size = 0;
  };

  ReaderContext(std::size_t vector_count, const DiskGraphSearchConfig& config,
                DiskGraphSearchStats& stats);

  bool test_and_set(std::uint32_t id);
  bool is_expanded(std::uint32_t id) const;
  bool mark_expanded(std::uint32_t id);
  std::size_t visited_count() const { return visited_count_; }
  const EffectiveLimits& limits() const { return limits_; }
  std::size_t peak_bytes() const { return peak_bytes_; }

 private:
  static std::size_t estimate_bytes(std::size_t vector_count,
                                    const DiskGraphSearchConfig& config,
                                    const EffectiveLimits& limits);
  void update_stats(DiskGraphSearchStats& stats) const;

  std::vector<std::uint64_t> visited_bits_;
  std::vector<std::uint64_t> expanded_bits_;
  std::size_t visited_count_ = 0;
  std::size_t peak_bytes_ = 0;
  EffectiveLimits limits_;
};

}  // namespace agent_aware
