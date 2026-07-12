#include "core/reader_context.h"

#include <algorithm>
#include <limits>

#include "graph/disk_graph_index.h"

namespace agent_aware {
namespace {
constexpr std::size_t kCandidateBytes = 16;
constexpr std::size_t kPageMetadataReserve = 256 * 1024;
constexpr std::size_t kContainerReserve = 256 * 1024;

std::size_t saturating_multiply(std::size_t value, std::size_t multiplier) {
  if (value > std::numeric_limits<std::size_t>::max() / multiplier) {
    return std::numeric_limits<std::size_t>::max();
  }
  return value * multiplier;
}
}

ReaderContext::ReaderContext(std::size_t vector_count,
                             const DiskGraphSearchConfig& config,
                             DiskGraphSearchStats& stats) {
  limits_.search_width = config.search_width;
  limits_.beam_width = std::clamp(
      config.beam_width == 0 ? config.search_width : config.beam_width,
      std::size_t{1}, config.search_width);
  limits_.rerank_topk = config.rerank_topk;
  limits_.prefetch_top_k = config.prefetch_top_k;
  limits_.prefetch_max_inflight = config.prefetch_max_inflight;
  const std::size_t default_hot = saturating_multiply(limits_.beam_width, 4);
  limits_.search_list_size =
      config.search_list_size == 0
          ? std::max({config.top_k, config.rerank_topk, default_hot})
          : config.search_list_size;
  limits_.max_search_list_size =
      config.max_search_list_size == 0
          ? std::max(limits_.search_list_size,
                     saturating_multiply(config.search_width, 8))
          : config.max_search_list_size;
  limits_.max_search_list_size =
      std::max(limits_.max_search_list_size, limits_.search_list_size);
  limits_.candidate_hot_size =
      config.candidate_hot_size == 0 ? default_hot
                                     : config.candidate_hot_size;
  limits_.candidate_hot_size =
      std::clamp(limits_.candidate_hot_size, std::size_t{1},
                 limits_.search_list_size);
  limits_.candidate_capacity = limits_.max_search_list_size;

  stats.requested_search_list_size = config.search_list_size;
  stats.requested_max_search_list_size = config.max_search_list_size;
  stats.requested_candidate_hot_size = config.candidate_hot_size;

  auto fits = [&] {
    return estimate_bytes(vector_count, config, limits_) <= kHardLimitBytes;
  };
  if (!fits()) {
    limits_.prefetch_top_k = 0;
    limits_.prefetch_max_inflight = 0;
    ++stats.workspace_prefetch_degrade_count;
    stats.workspace_degraded = true;
  }
  while (!fits() && limits_.rerank_topk > 0) {
    const std::size_t next = limits_.rerank_topk / 2;
    limits_.rerank_topk = next < config.top_k ? config.top_k : next;
    ++stats.workspace_rerank_degrade_count;
    stats.workspace_degraded = true;
    if (limits_.rerank_topk == config.top_k) break;
  }
  const auto minimum_pool_size = [&] {
    return std::max({limits_.search_list_size, config.top_k,
                     limits_.rerank_topk});
  };
  while (!fits() && limits_.max_search_list_size > minimum_pool_size()) {
    limits_.max_search_list_size =
        std::max(minimum_pool_size(), limits_.max_search_list_size / 2);
    limits_.candidate_capacity = limits_.max_search_list_size;
    ++stats.workspace_candidate_pool_degrade_count;
    stats.workspace_degraded = true;
  }
  while (!fits() && limits_.search_width > config.top_k) {
    limits_.search_width =
        std::max(config.top_k, limits_.search_width / 2);
    limits_.beam_width = std::min(limits_.beam_width, limits_.search_width);
    if (config.search_list_size == 0) {
      limits_.search_list_size = std::max(
          {config.top_k, limits_.rerank_topk,
           saturating_multiply(limits_.beam_width, 4)});
    }
    if (config.max_search_list_size == 0) {
      limits_.max_search_list_size = std::max(
          limits_.search_list_size,
          saturating_multiply(limits_.search_width, 8));
    } else {
      limits_.max_search_list_size =
          std::max(limits_.max_search_list_size, limits_.search_list_size);
    }
    limits_.candidate_hot_size = std::min(
        limits_.candidate_hot_size, limits_.search_list_size);
    limits_.candidate_capacity = limits_.max_search_list_size;
    ++stats.workspace_search_degrade_count;
    stats.workspace_degraded = true;
  }
  peak_bytes_ = estimate_bytes(vector_count, config, limits_);
  if (peak_bytes_ > kHardLimitBytes) {
    stats.workspace_budget_exceeded = true;
    update_stats(stats);
    throw QueryWorkspaceBudgetExceeded();
  }
  visited_bits_.assign((vector_count + 63) / 64, 0);
  expanded_bits_.assign((vector_count + 63) / 64, 0);
  update_stats(stats);
}

std::size_t ReaderContext::estimate_bytes(
    std::size_t vector_count, const DiskGraphSearchConfig& config,
    const EffectiveLimits& limits) {
  const std::size_t bitmap =
      ((vector_count + 63) / 64) * sizeof(std::uint64_t) * 2;
  const std::size_t candidates = limits.candidate_capacity * kCandidateBytes * 2;
  const std::size_t adc = config.pq_model == nullptr
                              ? 64 * 256 * sizeof(float)
                              : config.pq_model->subspaces() *
                                    config.pq_model->centroids() * sizeof(float);
  const std::size_t rerank = limits.rerank_topk * kCandidateBytes;
  const std::size_t prefetch =
      (limits.prefetch_top_k + limits.prefetch_max_inflight) * 64;
  return bitmap + candidates + adc + rerank + prefetch +
         kPageMetadataReserve + kContainerReserve;
}

void ReaderContext::update_stats(DiskGraphSearchStats& stats) const {
  stats.query_workspace_budget_bytes = kHardLimitBytes;
  stats.query_workspace_peak_bytes = peak_bytes_;
  stats.effective_search_width = limits_.search_width;
  stats.effective_beam_width = limits_.beam_width;
  stats.effective_rerank_topk = limits_.rerank_topk;
  stats.effective_prefetch_top_k = limits_.prefetch_top_k;
  stats.effective_prefetch_max_inflight = limits_.prefetch_max_inflight;
  stats.effective_search_list_size = limits_.search_list_size;
  stats.effective_max_search_list_size = limits_.max_search_list_size;
  stats.effective_candidate_hot_size = limits_.candidate_hot_size;
}

bool ReaderContext::test_and_set(std::uint32_t id) {
  const std::size_t word = id >> 6;
  const std::uint64_t mask = std::uint64_t{1} << (id & 63);
  if ((visited_bits_[word] & mask) != 0) return true;
  visited_bits_[word] |= mask;
  ++visited_count_;
  return false;
}

bool ReaderContext::is_expanded(std::uint32_t id) const {
  const std::size_t word = id >> 6;
  const std::uint64_t mask = std::uint64_t{1} << (id & 63);
  return (expanded_bits_[word] & mask) != 0;
}

bool ReaderContext::mark_expanded(std::uint32_t id) {
  const std::size_t word = id >> 6;
  const std::uint64_t mask = std::uint64_t{1} << (id & 63);
  if ((expanded_bits_[word] & mask) != 0) return false;
  expanded_bits_[word] |= mask;
  return true;
}

}  // namespace agent_aware
