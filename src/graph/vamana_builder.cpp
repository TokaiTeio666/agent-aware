#include "agent_aware/graph/vamana_builder.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "agent_aware/core/simd_distance.h"

namespace agent_aware {
namespace {

struct Candidate {
  std::uint32_t id = 0;
  float distance = 0.0f;
};

struct CloserFirst {
  bool operator()(const Candidate& lhs, const Candidate& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id > rhs.id;
    }
    return lhs.distance > rhs.distance;
  }
};

bool closer_candidate(const Candidate& lhs, const Candidate& rhs) {
  if (lhs.distance == rhs.distance) {
    return lhs.id < rhs.id;
  }
  return lhs.distance < rhs.distance;
}

std::uint32_t mix_u32(std::uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

bool contains_id(const std::vector<std::uint32_t>& ids, std::uint32_t target) {
  return std::find(ids.begin(), ids.end(), target) != ids.end();
}

bool is_deleted(const std::vector<char>& deleted, std::uint32_t id) {
  return !deleted.empty() &&
         (id >= deleted.size() || deleted[static_cast<std::size_t>(id)]);
}

bool is_deleted(const std::vector<char>& deleted, std::size_t id) {
  return !deleted.empty() && (id >= deleted.size() || deleted[id]);
}

std::vector<std::uint32_t> unique_ids(std::vector<std::uint32_t> ids,
                                      std::uint32_t excluded,
                                      const std::vector<char>& deleted) {
  std::vector<std::uint32_t> output;
  output.reserve(ids.size());

  std::unordered_set<std::uint32_t> seen;
  seen.reserve(ids.size());
  for (const auto id : ids) {
    if (id == excluded || is_deleted(deleted, id)) {
      continue;
    }
    if (seen.insert(id).second) {
      output.push_back(id);
    }
  }
  return output;
}

}  // namespace

VamanaBuilder::VamanaBuilder(VamanaBuildConfig config)
    : config_(std::move(config)) {
  if (config_.max_degree == 0) {
    throw std::runtime_error("Vamana max_degree must be positive");
  }
  if (config_.search_width == 0) {
    throw std::runtime_error("Vamana search_width must be positive");
  }
  if (config_.alpha < 1.0) {
    throw std::runtime_error("Vamana alpha must be at least 1.0");
  }
}

VamanaGraph VamanaBuilder::build(const VectorSet& base) const {
  if (base.empty() || base.dim == 0) {
    throw std::runtime_error("VamanaBuilder requires non-empty vectors");
  }
  if (base.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("VamanaBuilder supports at most uint32_t ids");
  }

  const auto deleted = deleted_mask(base.size());
  VamanaGraph output;
  output.medoid = find_medoid(base);
  output.adjacency = initial_graph(base, deleted);
  output.stats.medoid = output.medoid;

  const auto order = build_order(base.size());
  for (const auto node_id : order) {
    if (is_deleted(deleted, node_id)) {
      output.adjacency[node_id].clear();
      continue;
    }
    insert(base, output.adjacency, node_id, output.medoid, &output.stats);
  }

  finalize_stats(output.adjacency, output.stats);
  return output;
}

std::uint32_t VamanaBuilder::find_medoid(const VectorSet& base) const {
  if (base.empty() || base.dim == 0) {
    throw std::runtime_error("Cannot compute medoid for empty vectors");
  }

  const auto deleted = deleted_mask(base.size());
  std::vector<float> centroid(base.dim, 0.0f);
  std::size_t valid_count = 0;
  for (std::size_t i = 0; i < base.size(); ++i) {
    if (is_deleted(deleted, i)) {
      continue;
    }
    const float* row = base.row(i);
    for (std::size_t d = 0; d < base.dim; ++d) {
      centroid[d] += row[d];
    }
    ++valid_count;
  }
  if (valid_count == 0) {
    throw std::runtime_error("Cannot compute Vamana medoid: all nodes are deleted");
  }
  const float scale = 1.0f / static_cast<float>(valid_count);
  for (float& value : centroid) {
    value *= scale;
  }

  std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
  float best_distance = std::numeric_limits<float>::max();
  for (std::size_t i = 0; i < base.size(); ++i) {
    if (is_deleted(deleted, i)) {
      continue;
    }
    const float distance =
        l2_distance_sq_simd(base.row(i), centroid.data(), base.dim);
    if (distance < best_distance ||
        (distance == best_distance &&
         static_cast<std::uint32_t>(i) < best)) {
      best_distance = distance;
      best = static_cast<std::uint32_t>(i);
    }
  }
  if (best == std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("Cannot compute Vamana medoid: no valid nodes");
  }
  return best;
}

std::vector<std::uint32_t> VamanaBuilder::search_for_construction(
    const VectorSet& base,
    const std::vector<std::vector<std::uint32_t>>& graph,
    const float* query, std::uint32_t entry_id,
    std::uint32_t excluded_id) const {
  if (graph.size() != base.size()) {
    throw std::runtime_error("Vamana construction graph size mismatch");
  }
  const auto deleted = deleted_mask(base.size());
  if (entry_id >= graph.size() || is_deleted(deleted, entry_id)) {
    entry_id = find_medoid(base);
  }

  thread_local std::vector<std::uint32_t> visited_epoch;
  thread_local std::uint32_t current_epoch = 0;
  if (visited_epoch.size() < graph.size()) {
    visited_epoch.assign(graph.size(), 0);
  }
  if (current_epoch == std::numeric_limits<std::uint32_t>::max()) {
    std::fill(visited_epoch.begin(), visited_epoch.end(), 0);
    current_epoch = 0;
  }
  ++current_epoch;
  std::priority_queue<Candidate, std::vector<Candidate>, CloserFirst> frontier;
  std::vector<Candidate> explored;
  explored.reserve(std::min(config_.search_width, graph.size()));

  auto push = [&](std::uint32_t id) {
    if (id >= graph.size() || id == excluded_id || is_deleted(deleted, id)) {
      return;
    }
    if (visited_epoch[id] == current_epoch) {
      return;
    }
    visited_epoch[id] = current_epoch;
    frontier.push(
        Candidate{id, l2_distance_sq_simd(query, base.row(id), base.dim)});
  };

  push(entry_id);
  if (frontier.empty()) {
    for (std::size_t i = 0; i < graph.size(); ++i) {
      push(static_cast<std::uint32_t>(i));
      if (!frontier.empty()) {
        break;
      }
    }
  }

  while (!frontier.empty() && explored.size() < config_.search_width) {
    const Candidate current = frontier.top();
    frontier.pop();
    explored.push_back(current);
    for (const auto neighbor : graph[current.id]) {
      push(neighbor);
    }
  }

  std::sort(explored.begin(), explored.end(), closer_candidate);

  std::vector<std::uint32_t> ids;
  ids.reserve(explored.size());
  for (const auto& item : explored) {
    ids.push_back(item.id);
  }
  return ids;
}

std::vector<std::uint32_t> VamanaBuilder::robust_prune(
    const VectorSet& base, std::uint32_t node_id,
    const std::vector<std::uint32_t>& candidates) const {
  if (node_id >= base.size()) {
    throw std::runtime_error("Vamana robust_prune node id out of range");
  }
  const auto deleted = deleted_mask(base.size());
  if (is_deleted(deleted, node_id)) {
    return {};
  }

  const std::size_t effective_degree =
      std::min<std::size_t>(config_.max_degree, base.size() - 1);
  if (effective_degree == 0) {
    return {};
  }

  std::vector<Candidate> pool;
  const auto unique = unique_ids(candidates, node_id, deleted);
  pool.reserve(unique.size());
  const float* source = base.row(node_id);
  for (const auto id : unique) {
    pool.push_back(
        Candidate{id, l2_distance_sq_simd(source, base.row(id), base.dim)});
  }

  std::sort(pool.begin(), pool.end(), closer_candidate);

  std::vector<std::uint32_t> output;
  output.reserve(effective_degree);
  const double alpha_sq = config_.alpha * config_.alpha;
  std::vector<Candidate> remaining;
  remaining.reserve(pool.size());
  while (!pool.empty() && output.size() < effective_degree) {
    const Candidate picked = pool.front();
    output.push_back(picked.id);

    remaining.clear();
    for (std::size_t i = 1; i < pool.size(); ++i) {
      const Candidate& candidate = pool[i];
      const float between =
          l2_distance_sq_simd(base.row(picked.id), base.row(candidate.id),
                              base.dim);
      if (alpha_sq * static_cast<double>(between) >
          static_cast<double>(candidate.distance)) {
        remaining.push_back(candidate);
      }
    }
    pool.swap(remaining);
  }
  return output;
}

void VamanaBuilder::insert(const VectorSet& base,
                           std::vector<std::vector<std::uint32_t>>& graph,
                           std::uint32_t node_id, std::uint32_t entry_id,
                           VamanaBuildStats* stats) const {
  if (graph.size() != base.size()) {
    throw std::runtime_error("Vamana insert graph size mismatch");
  }
  if (node_id >= graph.size()) {
    throw std::runtime_error("Vamana insert node id out of range");
  }
  const auto deleted = deleted_mask(base.size());
  if (is_deleted(deleted, node_id)) {
    graph[node_id].clear();
    return;
  }

  std::vector<std::uint32_t> candidates = search_for_construction(
      base, graph, base.row(node_id), entry_id, node_id);
  candidates.insert(candidates.end(), graph[node_id].begin(),
                    graph[node_id].end());
  candidates = unique_ids(std::move(candidates), node_id, deleted);
  if (stats != nullptr) {
    stats->candidate_counts.push_back(candidates.size());
    ++stats->prune_count;
  }

  graph[node_id] = robust_prune(base, node_id, candidates);
  if (!config_.add_reverse_edges) {
    return;
  }
  const std::vector<std::uint32_t> neighbors = graph[node_id];
  for (const auto target : neighbors) {
    add_reverse_edge(base, graph, node_id, target, stats);
  }
}

std::vector<char> VamanaBuilder::deleted_mask(std::size_t count) const {
  if (config_.deleted_ids.empty()) {
    return {};
  }
  std::vector<char> deleted(count, 0);
  for (const auto id : config_.deleted_ids) {
    if (id < count) {
      deleted[id] = 1;
    }
  }
  return deleted;
}

std::vector<std::vector<std::uint32_t>> VamanaBuilder::initial_graph(
    const VectorSet& base, const std::vector<char>& deleted) const {
  std::vector<std::vector<std::uint32_t>> graph(base.size());
  if (base.size() <= 1) {
    return graph;
  }

  const std::size_t ring_degree =
      std::min<std::size_t>(config_.max_degree, base.size() - 1);
  const std::size_t random_degree = std::max<std::size_t>(
      ring_degree, std::min<std::size_t>(config_.search_width, base.size() - 1));

  for (std::size_t i = 0; i < base.size(); ++i) {
    if (is_deleted(deleted, i)) {
      continue;
    }
    std::vector<std::uint32_t> candidates;
    candidates.reserve(ring_degree * 2 + random_degree);
    for (std::size_t offset = 1; offset <= ring_degree; ++offset) {
      candidates.push_back(static_cast<std::uint32_t>((i + offset) % base.size()));
      candidates.push_back(static_cast<std::uint32_t>(
          (i + base.size() - offset) % base.size()));
    }
    const std::uint32_t seed =
        mix_u32(config_.seed ^ static_cast<std::uint32_t>(i));
    for (std::size_t r = 0; r < random_degree; ++r) {
      const std::uint32_t mixed =
          mix_u32(seed + static_cast<std::uint32_t>(r * 0x9e3779b9u));
      candidates.push_back(static_cast<std::uint32_t>(mixed % base.size()));
    }
    graph[i] = unique_ids(std::move(candidates),
                          static_cast<std::uint32_t>(i), deleted);
    if (graph[i].size() > config_.max_degree) {
      graph[i].resize(config_.max_degree);
    }
  }
  return graph;
}

std::vector<std::uint32_t> VamanaBuilder::build_order(std::size_t count) const {
  std::vector<std::uint32_t> order;
  order.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    order.push_back(static_cast<std::uint32_t>(i));
  }
  std::mt19937 rng(config_.seed ^ 0x9e3779b9u);
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

void VamanaBuilder::add_reverse_edge(
    const VectorSet& base, std::vector<std::vector<std::uint32_t>>& graph,
    std::uint32_t source, std::uint32_t target,
    VamanaBuildStats* stats) const {
  if (target >= graph.size() || source >= graph.size() || source == target) {
    return;
  }
  const auto deleted = deleted_mask(base.size());
  if (is_deleted(deleted, source) || is_deleted(deleted, target) ||
      contains_id(graph[target], source)) {
    return;
  }

  if (graph[target].size() < config_.max_degree) {
    graph[target].push_back(source);
    if (stats != nullptr) {
      ++stats->reverse_edges_added;
    }
    return;
  }

  std::vector<std::uint32_t> candidates = graph[target];
  candidates.push_back(source);
  if (stats != nullptr) {
    ++stats->reverse_prune_count;
    ++stats->prune_count;
  }
  const auto pruned = robust_prune(base, target, candidates);
  if (contains_id(pruned, source) && stats != nullptr) {
    ++stats->reverse_edges_added;
  }
  graph[target] = pruned;
}

void VamanaBuilder::finalize_stats(
    const std::vector<std::vector<std::uint32_t>>& graph,
    VamanaBuildStats& stats) const {
  if (graph.empty()) {
    return;
  }
  std::size_t degree_sum = 0;
  std::size_t max_degree = 0;
  for (const auto& neighbors : graph) {
    degree_sum += neighbors.size();
    max_degree = std::max(max_degree, neighbors.size());
  }
  stats.avg_degree =
      static_cast<double>(degree_sum) / static_cast<double>(graph.size());
  stats.max_degree = max_degree;

  if (stats.candidate_counts.empty()) {
    return;
  }
  std::vector<std::size_t> sorted = stats.candidate_counts;
  std::sort(sorted.begin(), sorted.end());
  const auto percentile = [&](double p) {
    const double pos = (static_cast<double>(sorted.size()) - 1.0) * p;
    const auto lower = static_cast<std::size_t>(pos);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    const double fraction = pos - static_cast<double>(lower);
    return static_cast<double>(sorted[lower]) * (1.0 - fraction) +
           static_cast<double>(sorted[upper]) * fraction;
  };
  const double sum = static_cast<double>(std::accumulate(
      sorted.begin(), sorted.end(), static_cast<std::size_t>(0)));
  stats.candidate_avg = sum / static_cast<double>(sorted.size());
  stats.candidate_p95 = percentile(0.95);
  stats.candidate_p99 = percentile(0.99);
}

}  // namespace agent_aware
