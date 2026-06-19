#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace agent_aware {

class PrefetchPlanner {
 public:
  struct Config {
    std::string policy = "frontier-next-hop";
    std::size_t prefetch_width = 0;
    std::size_t prefetch_depth = 1;
    std::size_t fallback_width = 0;
    bool dedup_pages = true;
    bool coalesce_pages = true;
  };

  struct PageAvailability {
    bool cached = false;
    bool ready = false;
    bool pending = false;
  };

  struct PlanStats {
    std::size_t page_requests_before_dedup = 0;
    std::size_t dedup_requests = 0;
    std::size_t dedup_hits = 0;
  };

  struct Plan {
    std::vector<std::uint32_t> pages;
    PlanStats stats;
  };

  using PageForNode = std::function<std::uint32_t(std::uint32_t)>;
  using SkipNode = std::function<bool(std::uint32_t)>;
  using PageAvailabilityFn =
      std::function<PageAvailability(std::uint32_t)>;

  explicit PrefetchPlanner(Config config);

  bool frontier_enabled() const;
  bool next_hop_enabled() const;
  std::size_t width() const;
  std::size_t depth() const;

  Plan plan_next_hop(const std::vector<std::uint32_t>& node_ids,
                     std::size_t vector_count,
                     const PageForNode& page_for_node,
                     const SkipNode& skip_node,
                     const PageAvailabilityFn& availability);

  Plan plan_frontier(std::vector<std::uint32_t> page_ids,
                     const PageAvailabilityFn& availability);

 private:
  bool accept_page(std::uint32_t page_id,
                   const PageAvailabilityFn& availability,
                   PlanStats& stats);

  Config config_;
  std::unordered_set<std::uint32_t> seen_pages_;
};

}  // namespace agent_aware
