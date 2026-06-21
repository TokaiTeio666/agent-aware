#include "agent_aware/core/prefetch_planner.h"

#include <algorithm>
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
  if (config_.prefetch_width == 0) {
    return config_.fallback_width;
  }
  return std::max<std::size_t>(1, config_.prefetch_width);
}

std::size_t PrefetchPlanner::depth() const {
  return std::max<std::size_t>(1, config_.prefetch_depth);
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
  Plan plan;
  if (!frontier_enabled() || width() == 0 || depth() == 0) {
    return plan;
  }

  if (config_.coalesce_pages) {
    std::sort(page_ids.begin(), page_ids.end());
    page_ids.erase(std::unique(page_ids.begin(), page_ids.end()),
                   page_ids.end());
  }

  plan.pages.reserve(std::min(width(), page_ids.size()));
  for (const auto page_id : page_ids) {
    if (plan.pages.size() >= width()) {
      break;
    }
    ++plan.stats.page_requests_before_dedup;
    if (accept_page(page_id, availability, plan.stats)) {
      plan.pages.push_back(page_id);
    }
  }
  return plan;
}

bool PrefetchPlanner::accept_page(std::uint32_t page_id,
                                  const PageAvailabilityFn& availability,
                                  PlanStats& stats) {
  const PageAvailability state = availability(page_id);
  const bool already_available = state.cached || state.ready || state.pending;
  if (!config_.dedup_pages) {
    return !already_available;
  }

  ++stats.dedup_requests;
  const bool seen_before = !seen_pages_.insert(page_id).second;
  if (seen_before || already_available) {
    ++stats.dedup_hits;
    return false;
  }
  return true;
}

}  // namespace agent_aware
