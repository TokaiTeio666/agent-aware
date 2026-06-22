#include "agent_aware/core/prefetch_planner.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace agent_aware {

PrefetchPlanner::PrefetchPlanner(Config config)
    : config_(std::move(config)) {}

bool PrefetchPlanner::frontier_enabled() const {
  return config_.policy == "frontier" ||
         config_.policy == "frontier-next-hop";
}

bool PrefetchPlanner::next_hop_enabled() const {
  return config_.policy == "next-hop" ||
         config_.policy == "frontier-next-hop";
}

std::size_t PrefetchPlanner::width() const {
  return config_.prefetch_width;
}

std::size_t PrefetchPlanner::depth() const {
  return config_.prefetch_depth;
}

std::size_t PrefetchPlanner::fallback_width() const {
  return config_.fallback_width;
}

PrefetchPlanner::Plan PrefetchPlanner::plan_next_hop(
    const std::vector<std::uint32_t>& node_ids, std::size_t vector_count,
    const PageForNode& page_for_node, const SkipNode& skip_node,
    const PageAvailabilityFn& availability) {
  Plan plan;
  if (!next_hop_enabled() || width() == 0 || depth() == 0) {
    return plan;
  }

  plan.pages.reserve(width());
  for (const auto node_id : node_ids) {
    if (plan.pages.size() >= width()) {
      break;
    }
    if (node_id >= vector_count || skip_node(node_id)) {
      continue;
    }

    const std::uint32_t page_id = page_for_node(node_id);
    ++plan.stats.page_requests_before_dedup;
    if (accept_page(page_id, availability, plan.stats)) {
      plan.pages.push_back(page_id);
    }
  }
  return plan;
}

PrefetchPlanner::Plan PrefetchPlanner::plan_frontier(
    std::vector<std::uint32_t> page_ids,
    const PageAvailabilityFn& availability) {
  if (!frontier_enabled() || width() == 0 || depth() == 0) {
    return Plan{};
  }
  return plan_candidates(std::move(page_ids), availability);
}

PrefetchPlanner::Plan PrefetchPlanner::plan_candidates(
    std::vector<std::uint32_t> page_ids,
    const PageAvailabilityFn& availability, std::size_t max_pages) {
  Plan plan;
  const std::size_t limit = max_pages == 0 ? width() : max_pages;
  if (limit == 0 || depth() == 0) {
    return plan;
  }

  if (!config_.coalesce_pages) {
    return plan_ordered_candidates(std::move(page_ids), availability, limit);
  }
  return plan_coalesced_candidates(std::move(page_ids), availability, limit);
}

PrefetchPlanner::Plan PrefetchPlanner::plan_ordered_candidates(
    std::vector<std::uint32_t> page_ids,
    const PageAvailabilityFn& availability, std::size_t max_pages) {
  Plan plan;
  plan.pages.reserve(std::min(max_pages, page_ids.size()));
  std::unordered_set<std::uint32_t> emitted;
  emitted.reserve(std::min(max_pages, page_ids.size()));

  for (const auto page_id : page_ids) {
    ++plan.stats.page_requests_before_dedup;
    ++plan.stats.dedup_requests;

    const PageAvailability state = availability(page_id);
    if (state.cached) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_cached;
      continue;
    }
    if (state.pending) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_pending;
      continue;
    }
    if (state.ready || state.materialized) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_materialized;
      continue;
    }
    if (!emitted.insert(page_id).second) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_seen_before;
      continue;
    }
    if (config_.dedup_pages &&
        seen_pages_.find(page_id) != seen_pages_.end()) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_seen_before;
      continue;
    }
    if (plan.pages.size() >= max_pages) {
      ++plan.stats.skip_budget_full;
      continue;
    }
    plan.pages.push_back(page_id);
    if (config_.dedup_pages) {
      seen_pages_.insert(page_id);
    }
  }
  return plan;
}

PrefetchPlanner::Plan PrefetchPlanner::plan_coalesced_candidates(
    std::vector<std::uint32_t> page_ids,
    const PageAvailabilityFn& availability, std::size_t max_pages) {
  Plan plan;

  struct PageCandidate {
    std::size_t count = 0;
    std::size_t first_rank = 0;
  };

  std::unordered_map<std::uint32_t, PageCandidate> candidates;
  candidates.reserve(page_ids.size());
  for (std::size_t rank = 0; rank < page_ids.size(); ++rank) {
    const std::uint32_t page_id = page_ids[rank];
    ++plan.stats.page_requests_before_dedup;
    ++plan.stats.dedup_requests;

    const PageAvailability state = availability(page_id);
    if (state.cached) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_cached;
      continue;
    }
    if (state.pending) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_pending;
      continue;
    }
    if (state.ready || state.materialized) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_materialized;
      continue;
    }
    if (config_.dedup_pages &&
        seen_pages_.find(page_id) != seen_pages_.end()) {
      ++plan.stats.dedup_hits;
      ++plan.stats.skip_seen_before;
      continue;
    }

    auto [it, inserted] = candidates.emplace(page_id, PageCandidate{0, rank});
    if (!inserted) {
      it->second.first_rank = std::min(it->second.first_rank, rank);
    }
    ++it->second.count;
  }

  std::vector<std::pair<std::uint32_t, PageCandidate>> ranked;
  ranked.reserve(candidates.size());
  for (const auto& entry : candidates) {
    ranked.push_back(entry);
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const auto& lhs, const auto& rhs) {
              if (lhs.second.count != rhs.second.count) {
                return lhs.second.count > rhs.second.count;
              }
              return lhs.second.first_rank < rhs.second.first_rank;
            });

  const std::size_t min_reuse =
      std::max<std::size_t>(1, config_.min_candidates_per_page);
  plan.pages.reserve(std::min(max_pages, ranked.size()));
  for (const auto& entry : ranked) {
    if (entry.second.count < min_reuse) {
      continue;
    }
    if (plan.pages.size() >= max_pages) {
      ++plan.stats.skip_budget_full;
      continue;
    }
    plan.pages.push_back(entry.first);
    if (config_.dedup_pages) {
      seen_pages_.insert(entry.first);
    }
  }

  if (plan.pages.empty() && !ranked.empty()) {
    const auto best = std::min_element(
        ranked.begin(), ranked.end(),
        [](const auto& lhs, const auto& rhs) {
          return lhs.second.first_rank < rhs.second.first_rank;
        });
    plan.pages.push_back(best->first);
    if (config_.dedup_pages) {
      seen_pages_.insert(best->first);
    }
    for (const auto& entry : ranked) {
      if (entry.first != best->first) {
        ++plan.stats.skip_low_page_reuse;
      }
    }
  } else {
    for (const auto& entry : ranked) {
      if (entry.second.count < min_reuse) {
        ++plan.stats.skip_low_page_reuse;
      }
    }
  }
  return plan;
}

bool PrefetchPlanner::accept_page(std::uint32_t page_id,
                                  const PageAvailabilityFn& availability,
                                  PlanStats& stats) {
  const PageAvailability state = availability(page_id);
  const bool already_available =
      state.cached || state.ready || state.pending || state.materialized;
  if (!config_.dedup_pages) {
    return !already_available;
  }

  ++stats.dedup_requests;
  const bool seen_before = !seen_pages_.insert(page_id).second;
  if (seen_before || already_available) {
    ++stats.dedup_hits;
    if (seen_before) {
      ++stats.skip_seen_before;
    } else if (state.cached) {
      ++stats.skip_cached;
    } else if (state.pending) {
      ++stats.skip_pending;
    } else {
      ++stats.skip_materialized;
    }
    return false;
  }
  return true;
}

}  // namespace agent_aware
