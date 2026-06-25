#include "agent_aware/core/prefetch_planner.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace agent_aware {
namespace {

constexpr std::size_t kTopCandidateCutoff = 4;

double finite_or_zero(double value) {
  return std::isfinite(value) ? value : 0.0;
}

double finite_or_negative_infinity(double value) {
  return std::isfinite(value) ? value
                              : -std::numeric_limits<double>::infinity();
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

int parse_int_after(const std::string& text, const std::string& key) {
  const auto start = text.find(key);
  if (start == std::string::npos) {
    throw std::runtime_error("Malformed XGBoost model node: missing " + key);
  }
  const std::size_t value_start = start + key.size();
  std::size_t value_end = value_start;
  while (value_end < text.size() &&
         (std::isdigit(static_cast<unsigned char>(text[value_end])) ||
          text[value_end] == '-')) {
    ++value_end;
  }
  return std::stoi(text.substr(value_start, value_end - value_start));
}

double parse_double_after(const std::string& text, const std::string& key) {
  const auto start = text.find(key);
  if (start == std::string::npos) {
    throw std::runtime_error("Malformed XGBoost model node: missing " + key);
  }
  const std::size_t value_start = start + key.size();
  std::size_t value_end = value_start;
  while (value_end < text.size()) {
    const char ch = text[value_end];
    if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' ||
          ch == '+' || ch == '.' || ch == 'e' || ch == 'E')) {
      break;
    }
    ++value_end;
  }
  const double value = std::stod(text.substr(value_start, value_end - value_start));
  if (!std::isfinite(value)) {
    throw std::runtime_error("XGBoost model contains a non-finite value");
  }
  return value;
}

double feature_value(const PrefetchPlanner::PrefetchFeature& feature,
                     const std::string& name) {
  if (name == "num_candidates_on_page") {
    return static_cast<double>(feature.num_candidates_on_page);
  }
  if (name == "min_pq_rank_on_page") {
    return static_cast<double>(feature.min_pq_rank_on_page);
  }
  if (name == "avg_pq_rank_on_page") {
    return feature.avg_pq_rank_on_page;
  }
  if (name == "candidate_rank_span") {
    return static_cast<double>(feature.candidate_rank_span);
  }
  if (name == "contains_top1_candidate") {
    return feature.contains_top1_candidate ? 1.0 : 0.0;
  }
  if (name == "contains_topk_candidate") {
    return feature.contains_topk_candidate ? 1.0 : 0.0;
  }
  if (name == "is_cached") {
    return feature.is_cached ? 1.0 : 0.0;
  }
  if (name == "is_inflight") {
    return feature.is_inflight ? 1.0 : 0.0;
  }
  if (name == "is_ready") {
    return feature.is_ready ? 1.0 : 0.0;
  }
  if (name == "is_materialized") {
    return feature.is_materialized ? 1.0 : 0.0;
  }
  if (name == "search_step") {
    return static_cast<double>(feature.search_step);
  }
  if (name == "visited_count") {
    return static_cast<double>(feature.visited_count);
  }
  if (name == "frontier_size") {
    return static_cast<double>(feature.frontier_size);
  }
  if (name == "result_size") {
    return static_cast<double>(feature.result_size);
  }
  if (name == "current_candidate_distance") {
    return finite_or_zero(feature.current_candidate_distance);
  }
  if (name == "worst_result_distance") {
    return finite_or_zero(feature.worst_result_distance);
  }
  if (name == "ef_search") {
    return static_cast<double>(feature.ef_search);
  }
  if (name == "beam_width") {
    return static_cast<double>(feature.beam_width);
  }
  if (name == "io_queue_depth") {
    return static_cast<double>(feature.io_queue_depth);
  }
  if (name == "max_prefetch_inflight") {
    return static_cast<double>(feature.max_prefetch_inflight);
  }
  if (name == "cache_pressure") {
    return feature.cache_pressure;
  }
  return 0.0;
}

}  // namespace

struct PrefetchPlanner::PageCandidate {
  PrefetchFeature feature;
  bool eligible = true;
};

PrefetchPlanner::PrefetchPlanner(Config config)
    : config_(std::move(config)) {
  if (config_.policy == "xgboost") {
    if (config_.model_path.empty()) {
      throw std::runtime_error(
          "XGBoost prefetch policy requires --prefetch-model");
    }
    load_xgboost_model(config_.model_path);
  }
}

bool PrefetchPlanner::frontier_enabled() const {
  return config_.policy == "xgboost";
}

std::size_t PrefetchPlanner::width() const {
  return config_.prefetch_width;
}

std::size_t PrefetchPlanner::depth() const {
  return config_.prefetch_depth;
}

std::size_t PrefetchPlanner::top_k() const {
  return config_.prefetch_top_k == 0 ? width() : config_.prefetch_top_k;
}

PrefetchPlanner::Plan PrefetchPlanner::plan_frontier(
    std::vector<std::uint32_t> page_ids,
    const PageAvailabilityFn& availability, PlanningContext context) {
  if (!frontier_enabled() || top_k() == 0 || depth() == 0) {
    return Plan{};
  }
  return plan_candidates(std::move(page_ids), availability, 0, true,
                         context);
}

PrefetchPlanner::Plan PrefetchPlanner::plan_candidates(
    std::vector<std::uint32_t> page_ids,
    const PageAvailabilityFn& availability, std::size_t max_pages,
    bool allow_low_reuse_fallback, PlanningContext context) {
  Plan plan;
  const std::size_t limit = max_pages == 0 ? top_k() : max_pages;
  if (limit == 0 || depth() == 0) {
    return plan;
  }

  return plan_coalesced_candidates(std::move(page_ids), availability, limit,
                                   allow_low_reuse_fallback, context);
}

PrefetchPlanner::Plan PrefetchPlanner::plan_coalesced_candidates(
    std::vector<std::uint32_t> page_ids,
    const PageAvailabilityFn& availability, std::size_t max_pages,
    bool allow_low_reuse_fallback, PlanningContext context) {
  PlanStats stats;

  struct Aggregate {
    PrefetchFeature feature;
    std::size_t rank_sum = 0;
    std::size_t last_rank = 0;
    bool state_loaded = false;
    bool eligible = true;
  };

  std::unordered_map<std::uint32_t, Aggregate> aggregates;
  aggregates.reserve(page_ids.size());
  for (std::size_t rank = 0; rank < page_ids.size(); ++rank) {
    const std::uint32_t page_id = page_ids[rank];
    ++stats.page_requests_before_dedup;
    ++stats.dedup_requests;

    auto [it, inserted] = aggregates.emplace(page_id, Aggregate{});
    Aggregate& aggregate = it->second;
    if (inserted) {
      aggregate.feature.page_id = page_id;
      aggregate.feature.min_pq_rank_on_page = rank;
      aggregate.last_rank = rank;
      aggregate.feature.search_step = context.search_step;
      aggregate.feature.visited_count = context.visited_count;
      aggregate.feature.frontier_size = context.frontier_size;
      aggregate.feature.result_size = context.result_size;
      aggregate.feature.current_candidate_distance =
          context.current_candidate_distance;
      aggregate.feature.worst_result_distance =
          context.worst_result_distance;
      aggregate.feature.ef_search = context.ef_search;
      aggregate.feature.beam_width = context.beam_width;
      aggregate.feature.io_queue_depth = context.io_queue_depth;
      aggregate.feature.max_prefetch_inflight =
          context.max_prefetch_inflight == 0
              ? config_.max_prefetch_inflight
              : context.max_prefetch_inflight;
      aggregate.feature.cache_pressure = context.cache_pressure;

      const PageAvailability state = availability(page_id);
      aggregate.state_loaded = true;
      aggregate.feature.is_cached = state.cached;
      aggregate.feature.is_inflight = state.pending;
      aggregate.feature.is_ready = state.ready;
      aggregate.feature.is_materialized = state.materialized;
      if (state.cached || state.pending || state.ready ||
          state.materialized) {
        aggregate.eligible = false;
        ++stats.dedup_hits;
        if (state.cached) {
          ++stats.skip_cached;
          aggregate.feature.skip_reason = "cached";
        } else if (state.pending) {
          ++stats.skip_pending;
          aggregate.feature.skip_reason = "pending";
        } else {
          ++stats.skip_materialized;
          aggregate.feature.skip_reason = "materialized";
        }
      } else if (seen_pages_.find(page_id) != seen_pages_.end()) {
        aggregate.eligible = false;
        ++stats.dedup_hits;
        ++stats.skip_seen_before;
        aggregate.feature.skip_reason = "seen_before";
      }
    }

    ++aggregate.feature.num_candidates_on_page;
    aggregate.rank_sum += rank;
    aggregate.feature.min_pq_rank_on_page =
        std::min(aggregate.feature.min_pq_rank_on_page, rank);
    aggregate.last_rank = std::max(aggregate.last_rank, rank);
    aggregate.feature.contains_top1_candidate =
        aggregate.feature.contains_top1_candidate || rank == 0;
    aggregate.feature.contains_topk_candidate =
        aggregate.feature.contains_topk_candidate ||
        rank < kTopCandidateCutoff;
  }

  std::vector<PageCandidate> candidates;
  candidates.reserve(aggregates.size());
  for (auto& entry : aggregates) {
    Aggregate& aggregate = entry.second;
    if (aggregate.feature.num_candidates_on_page > 0) {
      aggregate.feature.avg_pq_rank_on_page =
          static_cast<double>(aggregate.rank_sum) /
          static_cast<double>(aggregate.feature.num_candidates_on_page);
    }
    aggregate.feature.candidate_rank_span =
        aggregate.last_rank - aggregate.feature.min_pq_rank_on_page;
    candidates.push_back(PageCandidate{aggregate.feature,
                                       aggregate.eligible});
  }

  return rank_and_throttle(std::move(candidates), max_pages,
                           allow_low_reuse_fallback, context, stats);
}

PrefetchPlanner::Plan PrefetchPlanner::rank_and_throttle(
    std::vector<PageCandidate> candidates, std::size_t max_pages,
    bool allow_low_reuse_fallback, PlanningContext context,
    PlanStats stats) {
  Plan plan;
  if (candidates.empty()) {
    plan.stats = stats;
    return plan;
  }

  const std::size_t limit = max_pages == 0 ? top_k() : max_pages;
  const std::size_t min_reuse =
      context.min_candidates_per_page == 0
          ? std::max<std::size_t>(1, config_.min_candidates_per_page)
          : std::max<std::size_t>(1, context.min_candidates_per_page);
  const std::size_t inflight_limit =
      context.max_prefetch_inflight == 0
          ? config_.max_prefetch_inflight
          : (config_.max_prefetch_inflight == 0
                 ? context.max_prefetch_inflight
                 : std::min(context.max_prefetch_inflight,
                            config_.max_prefetch_inflight));
  const std::size_t base_inflight = context.io_queue_depth;

  for (auto& candidate : candidates) {
    candidate.feature.score =
        finite_or_negative_infinity(score_candidate(candidate.feature));
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const PageCandidate& lhs, const PageCandidate& rhs) {
              if (lhs.feature.score != rhs.feature.score) {
                return lhs.feature.score > rhs.feature.score;
              }
              if (lhs.feature.num_candidates_on_page !=
                  rhs.feature.num_candidates_on_page) {
                return lhs.feature.num_candidates_on_page >
                       rhs.feature.num_candidates_on_page;
              }
              if (lhs.feature.min_pq_rank_on_page !=
                  rhs.feature.min_pq_rank_on_page) {
                return lhs.feature.min_pq_rank_on_page <
                       rhs.feature.min_pq_rank_on_page;
              }
              return lhs.feature.page_id < rhs.feature.page_id;
            });

  plan.candidates.reserve(candidates.size());
  plan.pages.reserve(std::min(limit, candidates.size()));

  for (std::size_t i = 0; i < candidates.size(); ++i) {
    PageCandidate& candidate = candidates[i];
    candidate.feature.rank = i + 1;

    if (!candidate.eligible) {
      plan.candidates.push_back(candidate.feature);
      continue;
    }
    if (candidate.feature.num_candidates_on_page < min_reuse) {
      candidate.feature.skip_reason = "low_page_reuse";
      ++stats.skip_low_page_reuse;
      plan.candidates.push_back(candidate.feature);
      continue;
    }
    if (candidate.feature.score < config_.score_threshold) {
      candidate.feature.skip_reason = "score_threshold";
      ++stats.skip_score_threshold;
      plan.candidates.push_back(candidate.feature);
      continue;
    }
    if (plan.pages.size() >= limit) {
      candidate.feature.skip_reason = "budget_full";
      ++stats.skip_budget_full;
      plan.candidates.push_back(candidate.feature);
      continue;
    }
    if (inflight_limit != 0 &&
        base_inflight + plan.pages.size() >= inflight_limit) {
      candidate.feature.skip_reason = "inflight_full";
      ++stats.skip_inflight_full;
      plan.candidates.push_back(candidate.feature);
      continue;
    }

    candidate.feature.selected = true;
    plan.pages.push_back(candidate.feature.page_id);
    seen_pages_.insert(candidate.feature.page_id);
    plan.candidates.push_back(candidate.feature);
  }

  if (plan.pages.empty() && allow_low_reuse_fallback) {
    for (auto& feature : plan.candidates) {
      if (feature.skip_reason != "low_page_reuse" ||
          feature.score < config_.score_threshold) {
        continue;
      }
      if (inflight_limit != 0 && base_inflight >= inflight_limit) {
        ++stats.skip_inflight_full;
        break;
      }
      feature.skip_reason.clear();
      feature.selected = true;
      plan.pages.push_back(feature.page_id);
      if (stats.skip_low_page_reuse > 0) {
        --stats.skip_low_page_reuse;
      }
      seen_pages_.insert(feature.page_id);
      break;
    }
  }

  plan.stats = stats;
  return plan;
}

double PrefetchPlanner::score_candidate(
    const PrefetchFeature& feature) const {
  return score_xgboost_model(feature);
}

void PrefetchPlanner::load_xgboost_model(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open prefetch model: " + path);
  }

  std::string line;
  XGBoostTree* current_tree = nullptr;
  while (std::getline(input, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    if (line.rfind("base_score=", 0) == 0) {
      xgboost_base_score_ =
          parse_double_after(line, std::string("base_score="));
      continue;
    }

    if (line.rfind("booster[", 0) == 0 || line.rfind("tree[", 0) == 0) {
      xgboost_trees_.push_back(XGBoostTree{});
      current_tree = &xgboost_trees_.back();
      continue;
    }

    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    if (current_tree == nullptr) {
      xgboost_trees_.push_back(XGBoostTree{});
      current_tree = &xgboost_trees_.back();
    }

    const int node_id = std::stoi(trim(line.substr(0, colon)));
    const std::string body = trim(line.substr(colon + 1));
    XGBoostNode node;
    if (body.rfind("leaf=", 0) == 0) {
      node.leaf = true;
      node.leaf_value = parse_double_after(body, std::string("leaf="));
    } else {
      const auto open = body.find('[');
      const auto split = body.find('<', open == std::string::npos ? 0 : open);
      const auto close = body.find(']', split == std::string::npos ? 0 : split);
      if (open == std::string::npos || split == std::string::npos ||
          close == std::string::npos || split <= open + 1 || close <= split + 1) {
        throw std::runtime_error("Malformed XGBoost split node in " + path);
      }
      node.feature = trim(body.substr(open + 1, split - open - 1));
      node.threshold = std::stod(body.substr(split + 1, close - split - 1));
      if (!std::isfinite(node.threshold)) {
        throw std::runtime_error("XGBoost split threshold must be finite");
      }
      node.yes = parse_int_after(body, "yes=");
      node.no = parse_int_after(body, "no=");
      node.missing = parse_int_after(body, "missing=");
    }
    (*current_tree)[node_id] = std::move(node);
  }

  if (xgboost_trees_.empty()) {
    throw std::runtime_error("Prefetch XGBoost model has no trees: " + path);
  }
}

double PrefetchPlanner::score_xgboost_model(
    const PrefetchFeature& feature) const {
  double score = xgboost_base_score_;
  for (const auto& tree : xgboost_trees_) {
    int node_id = 0;
    for (;;) {
      const auto found = tree.find(node_id);
      if (found == tree.end()) {
        throw std::runtime_error("XGBoost prefetch model references missing node");
      }
      const XGBoostNode& node = found->second;
      if (node.leaf) {
        score += node.leaf_value;
        break;
      }
      const double value = feature_value(feature, node.feature);
      if (!std::isfinite(value)) {
        node_id = node.missing;
      } else {
        node_id = value < node.threshold ? node.yes : node.no;
      }
    }
  }
  return score;
}

}  // namespace agent_aware
