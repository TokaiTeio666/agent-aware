#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agent_aware {

class PrefetchPlanner {
 public:
  struct Config {
    std::string policy = "none";
    std::size_t prefetch_width = 0;
    std::size_t prefetch_depth = 1;
    std::size_t min_candidates_per_page = 2;
    std::string model_path;
    std::size_t prefetch_top_k = 0;
    double score_threshold = -std::numeric_limits<double>::infinity();
    std::size_t max_prefetch_inflight = 0;
  };

  struct PageAvailability {
    bool cached = false;
    bool ready = false;
    bool pending = false;
    bool materialized = false;
  };

  struct PlanStats {
    std::size_t page_requests_before_dedup = 0;
    std::size_t dedup_requests = 0;
    std::size_t dedup_hits = 0;
    std::size_t skip_seen_before = 0;
    std::size_t skip_cached = 0;
    std::size_t skip_pending = 0;
    std::size_t skip_materialized = 0;
    std::size_t skip_budget_full = 0;
    std::size_t skip_low_page_reuse = 0;
    std::size_t skip_score_threshold = 0;
    std::size_t skip_inflight_full = 0;
  };

  struct PlanningContext {
    std::size_t search_step = 0;
    std::size_t visited_count = 0;
    std::size_t frontier_size = 0;
    std::size_t result_size = 0;
    double current_candidate_distance =
        std::numeric_limits<double>::infinity();
    double worst_result_distance = std::numeric_limits<double>::infinity();
    std::size_t ef_search = 0;
    std::size_t beam_width = 0;
    std::size_t io_queue_depth = 0;
    std::size_t max_prefetch_inflight = 0;
    std::size_t min_candidates_per_page = 0;
    double cache_pressure = 0.0;
  };

  struct PrefetchFeature {
    std::uint32_t page_id = 0;
    std::size_t num_candidates_on_page = 0;
    std::size_t min_pq_rank_on_page = 0;
    double avg_pq_rank_on_page = 0.0;
    std::size_t candidate_rank_span = 0;
    bool contains_top1_candidate = false;
    bool contains_topk_candidate = false;
    bool is_cached = false;
    bool is_inflight = false;
    bool is_ready = false;
    bool is_materialized = false;
    std::size_t search_step = 0;
    std::size_t visited_count = 0;
    std::size_t frontier_size = 0;
    std::size_t result_size = 0;
    double current_candidate_distance =
        std::numeric_limits<double>::infinity();
    double worst_result_distance = std::numeric_limits<double>::infinity();
    std::size_t ef_search = 0;
    std::size_t beam_width = 0;
    std::size_t io_queue_depth = 0;
    std::size_t max_prefetch_inflight = 0;
    double cache_pressure = 0.0;
    double score = 0.0;
    std::size_t rank = 0;
    bool selected = false;
    std::string skip_reason;
  };

  struct Plan {
    std::vector<std::uint32_t> pages;
    std::vector<PrefetchFeature> candidates;
    PlanStats stats;
  };

  using PageAvailabilityFn =
      std::function<PageAvailability(std::uint32_t)>;

  explicit PrefetchPlanner(Config config);

  bool frontier_enabled() const;
  bool has_model() const { return !xgboost_trees_.empty(); }
  std::size_t width() const;
  std::size_t depth() const;
  std::size_t top_k() const;

  Plan plan_candidates(std::vector<std::uint32_t> page_ids,
                       const PageAvailabilityFn& availability,
                       std::size_t max_pages,
                       bool allow_low_reuse_fallback,
                       PlanningContext context);

  Plan plan_frontier(std::vector<std::uint32_t> page_ids,
                     const PageAvailabilityFn& availability,
                     PlanningContext context);

 private:
  struct PageCandidate;

  Plan plan_coalesced_candidates(std::vector<std::uint32_t> page_ids,
                                 const PageAvailabilityFn& availability,
                                 std::size_t max_pages,
                                 bool allow_low_reuse_fallback,
                                 PlanningContext context);
  Plan rank_and_throttle(std::vector<PageCandidate> candidates,
                         std::size_t max_pages,
                         bool allow_low_reuse_fallback,
                         PlanningContext context,
                         PlanStats stats);
  double score_candidate(const PrefetchFeature& feature) const;
  void load_xgboost_model(const std::string& path);
  double score_xgboost_model(const PrefetchFeature& feature) const;

  Config config_;
  struct XGBoostNode {
    bool leaf = false;
    std::string feature;
    double threshold = 0.0;
    int yes = -1;
    int no = -1;
    int missing = -1;
    double leaf_value = 0.0;
  };
  using XGBoostTree = std::unordered_map<int, XGBoostNode>;

  std::vector<XGBoostTree> xgboost_trees_;
  double xgboost_base_score_ = 0.0;
  std::unordered_set<std::uint32_t> seen_pages_;
};

}  // namespace agent_aware
