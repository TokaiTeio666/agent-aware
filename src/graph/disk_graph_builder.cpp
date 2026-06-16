#include "agentmem/graph/disk_graph_builder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "agentmem/core/brute_force.h"
#include "agentmem/graph/disk_page_codec.h"
#include "agentmem/graph/vamana_builder.h"

namespace agentmem {
namespace {

constexpr char kMagic[8] = {'A', 'M', 'F', 'G', 'V', '1', '\0', '\0'};  // 单节点页格式。
constexpr char kPackedMagic[8] = {'A', 'M', 'F', 'P', 'V', '2', '\0', '\0'};  // 紧凑页格式。
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kPackedVersion = 2;
constexpr std::uint64_t kRecordsOffset = 4096;  // 预留首个 4 KB 页存放 header。

struct NeighborCandidate {
  std::uint32_t id = 0;
  float distance = 0.0f;
};

struct ProjectionItem {
  float score = 0.0f;
  std::uint32_t id = 0;
};

struct ProjectionIndex {
  std::vector<std::vector<std::uint32_t>> orders;     // 每个投影上的排序结果。
  std::vector<std::vector<std::uint32_t>> positions;  // node id 到排序位置的反查表。
};

struct LshTable {
  std::vector<std::uint32_t> signatures;    // 每个向量的 SimHash。
  std::vector<std::uint32_t> ids_by_bucket; // CSR 风格的桶内 id 连续区。
  std::vector<std::uint32_t> offsets;       // offsets[b]..offsets[b+1] 为桶 b。
};

struct LshIndex {
  std::size_t bits = 0;
  std::vector<LshTable> tables;
};

struct WorseNeighborFirst {
  bool operator()(const NeighborCandidate& lhs,
                  const NeighborCandidate& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id < rhs.id;
    }
    return lhs.distance < rhs.distance;
  }
};

struct BetterNeighborFirst {
  bool operator()(const NeighborCandidate& lhs,
                  const NeighborCandidate& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id < rhs.id;
    }
    return lhs.distance < rhs.distance;
  }
};

struct CloserFirst {
  bool operator()(const SearchResult& lhs, const SearchResult& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id > rhs.id;
    }
    return lhs.distance > rhs.distance;
  }
};

struct WorseResultFirst {
  bool operator()(const SearchResult& lhs, const SearchResult& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id < rhs.id;
    }
    return lhs.distance < rhs.distance;
  }
};

std::size_t record_bytes(std::size_t dim, std::size_t degree) {
  return sizeof(std::uint32_t) * 2 + dim * sizeof(float) +  // id、degree、向量、邻居。
         degree * sizeof(std::uint32_t);
}

std::uint64_t pair_key(std::uint32_t lhs, std::uint32_t rhs) {
  const std::uint32_t a = std::min(lhs, rhs);
  const std::uint32_t b = std::max(lhs, rhs);
  return (static_cast<std::uint64_t>(a) << 32) | b;
}

bool contains_id(const std::vector<std::uint32_t>& ids, std::uint32_t target) {
  return std::find(ids.begin(), ids.end(), target) != ids.end();
}

std::uint32_t mix_u32(std::uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

bool is_candidate_build_policy(const DiskGraphBuildConfig& config) {
  return config.build_policy == "approx-rp" ||
         config.build_policy == "lsh-rp" ||
         config.build_policy == "lsh-vamana" ||
         config.build_policy == "vamana";
}

void validate_build_policy(const DiskGraphBuildConfig& config) {
  if (config.build_policy != "exact" && !is_candidate_build_policy(config)) {
    throw std::runtime_error(
        "--graph-build-policy must be exact, approx-rp, lsh-rp, "
        "lsh-vamana, or vamana");
  }
  if (is_candidate_build_policy(config) && config.build_policy != "vamana") {
    if (config.approx_projections == 0) {
      throw std::runtime_error("--approx-projections must be positive");
    }
    if (config.approx_window == 0) {
      throw std::runtime_error("--approx-window must be positive");
    }
    if (config.approx_candidate_limit != 0 &&
        config.approx_candidate_limit < config.degree) {
      throw std::runtime_error(
          "--approx-candidate-limit must be 0 or at least --graph-degree");
    }
  }
  if (config.build_policy == "lsh-rp" ||
      config.build_policy == "lsh-vamana") {
    if (config.lsh_tables == 0) {
      throw std::runtime_error("--lsh-tables must be positive");
    }
    if (config.lsh_bits == 0 || config.lsh_bits > 24) {
      throw std::runtime_error("--lsh-bits must be between 1 and 24");
    }
    if (config.lsh_probe_radius > 1) {
      throw std::runtime_error(
          "--lsh-probe-radius currently supports only 0 or 1");
    }
    if (config.lsh_bucket_limit == 0) {
      throw std::runtime_error("--lsh-bucket-limit must be positive");
    }
  }
  if (config.robust_prune_alpha < 1.0) {
    throw std::runtime_error("--robust-prune-alpha must be at least 1.0");
  }
}

std::vector<char> deleted_mask(std::size_t count,
                               const std::vector<std::uint32_t>& deleted_ids) {
  std::vector<char> deleted(count, 0);
  for (const auto id : deleted_ids) {
    if (id < count) {
      deleted[id] = 1;
    }
  }
  return deleted;
}

std::vector<std::uint32_t> exact_neighbors(const VectorSet& base,
                                           std::size_t node_id,
                                           std::size_t degree,
                                           const std::vector<char>& deleted) {
  const std::size_t effective_degree =
      base.size() == 0 ? 0 : std::min(degree, base.size() - 1);
  if (node_id >= deleted.size() || deleted[node_id]) {
    return {};
  }
  std::priority_queue<NeighborCandidate, std::vector<NeighborCandidate>,
                      WorseNeighborFirst>
      heap;
  const float* source = base.row(node_id);

  for (std::size_t other = 0; other < base.size(); ++other) {
    if (other == node_id || (other < deleted.size() && deleted[other])) {
      continue;
    }
    const float distance = squared_l2(source, base.row(other), base.dim);
    const NeighborCandidate item{static_cast<std::uint32_t>(other), distance};
    if (heap.size() < effective_degree) {
      heap.push(item);
    } else {
      const auto& worst = heap.top();
      if (distance < worst.distance ||
          (distance == worst.distance && item.id < worst.id)) {
        heap.pop();
        heap.push(item);
      }
    }
  }

  std::vector<NeighborCandidate> candidates;
  candidates.reserve(heap.size());
  while (!heap.empty()) {
    candidates.push_back(heap.top());
    heap.pop();
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const NeighborCandidate& lhs, const NeighborCandidate& rhs) {
              if (lhs.distance == rhs.distance) {
                return lhs.id < rhs.id;
              }
              return lhs.distance < rhs.distance;
            });

  std::vector<std::uint32_t> ids;
  ids.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    ids.push_back(candidate.id);
  }
  return ids;
}

std::vector<std::vector<std::uint32_t>> build_exact_graph(
    const VectorSet& base, std::size_t degree,
    const std::vector<char>& deleted) {
  // 仅用于小规模正确性基线：每个节点都会与全量 base 比较，复杂度接近 O(N^2)。
  std::vector<std::vector<std::uint32_t>> graph;
  graph.reserve(base.size());
  for (std::size_t i = 0; i < base.size(); ++i) {
    graph.push_back(exact_neighbors(base, i, degree, deleted));
  }
  return graph;
}

float projection_score(const float* row, const std::vector<float>& plane,
                       std::size_t dim) {
  float score = 0.0f;
  for (std::size_t d = 0; d < dim; ++d) {
    score += row[d] * plane[d];
  }
  return score;
}

ProjectionIndex build_projection_index(const VectorSet& base,
                                       const DiskGraphBuildConfig& config) {
  // approx-rp 用多个随机超平面排序，排名相近的向量会进入候选集。
  ProjectionIndex index;
  index.orders.resize(config.approx_projections);
  index.positions.resize(config.approx_projections);

  std::mt19937 rng(config.random_seed);
  std::uniform_int_distribution<int> sign_pick(0, 1);
  std::vector<float> plane(base.dim, 0.0f);
  std::vector<ProjectionItem> items(base.size());

  for (std::size_t p = 0; p < config.approx_projections; ++p) {
    for (float& value : plane) {
      value = sign_pick(rng) == 0 ? -1.0f : 1.0f;
    }

    for (std::size_t i = 0; i < base.size(); ++i) {
      items[i] = ProjectionItem{
          projection_score(base.row(i), plane, base.dim),
          static_cast<std::uint32_t>(i)};
    }

    std::sort(items.begin(), items.end(),
              [](const ProjectionItem& lhs, const ProjectionItem& rhs) {
                if (lhs.score == rhs.score) {
                  return lhs.id < rhs.id;
                }
                return lhs.score < rhs.score;
              });

    index.orders[p].resize(base.size());
    index.positions[p].resize(base.size());
    for (std::size_t rank = 0; rank < items.size(); ++rank) {
      const auto id = items[rank].id;
      index.orders[p][rank] = id;
      index.positions[p][id] = static_cast<std::uint32_t>(rank);
    }
  }

  return index;
}

void add_candidate(std::vector<std::uint32_t>& candidates, std::uint32_t self,
                   std::uint32_t candidate, std::size_t count) {
  if (candidate != self && candidate < count) {
    candidates.push_back(candidate);
  }
}

void add_projection_candidates(std::vector<std::uint32_t>& candidates,
                               std::uint32_t self,
                               const ProjectionIndex& projection_index,
                               const DiskGraphBuildConfig& config,
                               std::size_t count) {
  for (std::size_t p = 0; p < projection_index.orders.size(); ++p) {
    const std::size_t rank = projection_index.positions[p][self];
    const std::size_t begin =
        rank > config.approx_window ? rank - config.approx_window : 0;
    const std::size_t end =
        std::min(count, rank + config.approx_window + 1);
    for (std::size_t i = begin; i < end; ++i) {
      add_candidate(candidates, self, projection_index.orders[p][i], count);
    }
  }
}

void add_deterministic_samples(std::vector<std::uint32_t>& candidates,
                               std::uint32_t self,
                               const DiskGraphBuildConfig& config,
                               std::size_t count) {
  if (count <= 1) {
    return;
  }
  const std::uint32_t seed = mix_u32(config.random_seed ^ self);  // 可复现实验的伪随机采样。
  for (std::size_t i = 0; i < config.approx_random_samples; ++i) {
    const std::uint32_t mixed =
        mix_u32(seed + static_cast<std::uint32_t>(i * 0x9e3779b9u));
    add_candidate(candidates, self,
                  static_cast<std::uint32_t>(mixed % count), count);
  }

  const std::size_t ring = std::min<std::size_t>(config.approx_window, count - 1);
  for (std::size_t offset = 1; offset <= ring; ++offset) {
    add_candidate(candidates, self,
                  static_cast<std::uint32_t>((self + offset) % count), count);
    add_candidate(candidates, self,
                  static_cast<std::uint32_t>((self + count - offset) % count),
                  count);
  }
}

void deduplicate_and_limit(std::vector<std::uint32_t>& candidates,
                           std::uint32_t self,
                           const DiskGraphBuildConfig& config,
                           std::size_t count,
                           const std::vector<char>& deleted) {
  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(),
                     [&](std::uint32_t id) {
                       return id == self || id >= count ||
                              (id < deleted.size() && deleted[id]);
                     }),
      candidates.end());
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  if (config.approx_candidate_limit != 0 &&
      candidates.size() > config.approx_candidate_limit) {
    // 候选上限限制建图成本；固定 seed 保证同参数下结果稳定。
    std::mt19937 rng(mix_u32(config.random_seed ^ self ^ 0xa77c0deu));
    std::shuffle(candidates.begin(), candidates.end(), rng);
    candidates.resize(config.approx_candidate_limit);
  }
}

std::vector<std::uint32_t> nearest_neighbors_from_candidates(
    const VectorSet& base, std::uint32_t self,
    const std::vector<std::uint32_t>& candidates, std::size_t degree,
    const std::vector<char>& deleted) {
  const std::size_t effective_degree =
      std::min<std::size_t>(degree, base.size() - 1);
  if (effective_degree == 0 || self >= deleted.size() || deleted[self]) {
    return {};
  }

  std::vector<NeighborCandidate> pool;
  pool.reserve(candidates.size());
  const float* source = base.row(self);
  for (const auto candidate : candidates) {
    if (candidate == self || candidate >= base.size() ||
        candidate >= deleted.size() || deleted[candidate]) {
      continue;
    }
    pool.push_back(NeighborCandidate{
        candidate, squared_l2(source, base.row(candidate), base.dim)});
  }

  if (pool.size() > effective_degree) {
    std::nth_element(pool.begin(), pool.begin() + effective_degree,
                     pool.end(), BetterNeighborFirst{});
    pool.resize(effective_degree);
  }
  std::sort(pool.begin(), pool.end(), BetterNeighborFirst{});

  std::vector<std::uint32_t> output;
  output.reserve(pool.size());
  for (const auto& candidate : pool) {
    output.push_back(candidate.id);
  }
  return output;
}

std::vector<std::uint32_t> robust_prune_neighbors(
    const VectorSet& base, std::uint32_t self,
    const std::vector<std::uint32_t>& candidates, std::size_t degree,
    double alpha, const std::vector<char>& deleted) {
  // FreshVamana 风格裁剪：优先保留近邻，并移除可被已选邻居替代的冗余边。
  const std::size_t effective_degree =
      std::min<std::size_t>(degree, base.size() - 1);
  if (effective_degree == 0 || self >= deleted.size() || deleted[self]) {
    return {};
  }

  std::vector<NeighborCandidate> pool;
  pool.reserve(candidates.size());
  const float* source = base.row(self);
  for (const auto candidate : candidates) {
    if (candidate == self || candidate >= base.size() ||
        candidate >= deleted.size() || deleted[candidate]) {
      continue;
    }
    pool.push_back(NeighborCandidate{
        candidate, squared_l2(source, base.row(candidate), base.dim)});
  }
  std::sort(pool.begin(), pool.end(), BetterNeighborFirst{});

  std::vector<std::uint32_t> output;
  output.reserve(effective_degree);
  const double alpha_sq = alpha * alpha;
  while (!pool.empty() && output.size() < effective_degree) {
    const auto picked = pool.front();
    output.push_back(picked.id);

    std::vector<NeighborCandidate> remaining;
    remaining.reserve(pool.size());
    for (std::size_t i = 1; i < pool.size(); ++i) {
      const auto& candidate = pool[i];
      const float between =
          squared_l2(base.row(picked.id), base.row(candidate.id), base.dim);
      if (alpha_sq * static_cast<double>(between) >
          static_cast<double>(candidate.distance)) {
        remaining.push_back(candidate);
      }
    }
    pool = std::move(remaining);
  }

  return output;
}

void record_candidate_count(const DiskGraphBuildConfig& config,
                            std::size_t count) {
  if (config.stats != nullptr) {
    config.stats->candidate_counts.push_back(count);
  }
}

std::vector<std::uint32_t> unique_valid_candidates(
    const std::vector<std::uint32_t>& ids, std::uint32_t self,
    std::size_t count, const std::vector<char>& deleted) {
  std::vector<std::uint32_t> candidates;
  candidates.reserve(ids.size());
  for (const auto id : ids) {
    if (id != self && id < count && id < deleted.size() && !deleted[id]) {
      candidates.push_back(id);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  return candidates;
}

std::size_t add_reverse_edges_batch(
    const VectorSet& base, std::vector<std::vector<std::uint32_t>>& graph,
    std::size_t degree, const std::vector<char>& deleted) {
  // 批量补反向边后再统一裁剪，避免逐边插入导致反复重算距离。
  const std::size_t count = graph.size();
  std::size_t reverse_edges_added = 0;
  std::vector<std::vector<std::uint32_t>> reverse(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (i < deleted.size() && deleted[i]) {
      graph[i].clear();
      continue;
    }
    for (const auto neighbor : graph[i]) {
      if (neighbor < count && neighbor < deleted.size() && !deleted[neighbor]) {
        if (!contains_id(graph[neighbor], static_cast<std::uint32_t>(i))) {
          ++reverse_edges_added;
        }
        reverse[neighbor].push_back(static_cast<std::uint32_t>(i));
      }
    }
  }

  for (std::size_t i = 0; i < count; ++i) {
    if (i < deleted.size() && deleted[i]) {
      graph[i].clear();
      continue;
    }
    if (reverse[i].empty()) {
      continue;
    }
    std::vector<std::uint32_t> candidates;
    candidates.reserve(graph[i].size() + reverse[i].size());
    candidates.insert(candidates.end(), graph[i].begin(), graph[i].end());
    candidates.insert(candidates.end(), reverse[i].begin(), reverse[i].end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    graph[i] = nearest_neighbors_from_candidates(
        base, static_cast<std::uint32_t>(i), candidates, degree, deleted);
  }
  return reverse_edges_added;
}

struct ReversePatchStats {
  std::size_t reverse_edges_added = 0;
  std::size_t reverse_prune_count = 0;
};

ReversePatchStats add_reverse_edges_vamana(
    const VectorSet& base, std::vector<std::vector<std::uint32_t>>& graph,
    std::size_t degree, double alpha, const std::vector<char>& deleted) {
  ReversePatchStats stats;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
  for (std::size_t source = 0; source < graph.size(); ++source) {
    if (source >= deleted.size() || deleted[source]) {
      continue;
    }
    for (const auto target : graph[source]) {
      if (target < graph.size() && target < deleted.size() &&
          !deleted[target] && target != source) {
        edges.emplace_back(static_cast<std::uint32_t>(source), target);
      }
    }
  }

  for (const auto& edge : edges) {
    const std::uint32_t source = edge.first;
    const std::uint32_t target = edge.second;
    if (contains_id(graph[target], source)) {
      continue;
    }

    if (graph[target].size() < degree) {
      graph[target].push_back(source);
      ++stats.reverse_edges_added;
      continue;
    }

    std::vector<std::uint32_t> candidates = graph[target];
    candidates.push_back(source);
    candidates = unique_valid_candidates(candidates, target, graph.size(),
                                         deleted);
    ++stats.reverse_prune_count;
    const auto pruned = robust_prune_neighbors(base, target, candidates,
                                               degree, alpha, deleted);
    if (contains_id(pruned, source)) {
      ++stats.reverse_edges_added;
    }
    graph[target] = pruned;
  }

  return stats;
}

std::size_t prune_overfull_nodes(
    const VectorSet& base, std::vector<std::vector<std::uint32_t>>& graph,
    std::size_t degree, double alpha, const std::vector<char>& deleted) {
  std::size_t prune_count = 0;
  for (std::size_t node = 0; node < graph.size(); ++node) {
    if (node >= deleted.size() || deleted[node] ||
        graph[node].size() <= degree) {
      continue;
    }
    const auto candidates = unique_valid_candidates(
        graph[node], static_cast<std::uint32_t>(node), graph.size(), deleted);
    graph[node] = robust_prune_neighbors(
        base, static_cast<std::uint32_t>(node), candidates, degree, alpha,
        deleted);
    ++prune_count;
  }
  return prune_count;
}

void apply_freshvamana_delete_patch(
    const VectorSet& base, std::vector<std::vector<std::uint32_t>>& graph,
    std::size_t degree, double alpha, const std::vector<char>& deleted) {
  // 删除节点后，把其入邻居与出邻居重新连接，尽量保持图的可达性。
  if (deleted.empty()) {
    return;
  }

  std::vector<std::vector<std::uint32_t>> incoming(graph.size());
  for (std::size_t source = 0; source < graph.size(); ++source) {
    if (source < deleted.size() && deleted[source]) {
      continue;
    }
    for (const auto target : graph[source]) {
      if (target < deleted.size() && deleted[target]) {
        incoming[target].push_back(static_cast<std::uint32_t>(source));
      }
    }
  }

  std::vector<std::vector<std::uint32_t>> patch_candidates(graph.size());
  std::vector<std::uint32_t> affected;
  std::vector<char> affected_mask(graph.size(), 0);
  std::vector<std::uint32_t> deleted_nodes;

  for (std::size_t deleted_id = 0; deleted_id < deleted.size(); ++deleted_id) {
    if (!deleted[deleted_id]) {
      continue;
    }
    deleted_nodes.push_back(static_cast<std::uint32_t>(deleted_id));

    std::vector<std::uint32_t> out_neighbors;
    for (const auto neighbor : graph[deleted_id]) {
      if (neighbor < deleted.size() && !deleted[neighbor]) {
        out_neighbors.push_back(neighbor);
      }
    }
    auto& in_neighbors = incoming[deleted_id];
    in_neighbors.erase(
        std::remove_if(in_neighbors.begin(), in_neighbors.end(),
                       [&](std::uint32_t id) {
                         return id >= deleted.size() || deleted[id];
                       }),
        in_neighbors.end());

    for (const auto source : in_neighbors) {
      if (source >= patch_candidates.size()) {
        continue;
      }
      if (!affected_mask[source]) {
        affected_mask[source] = 1;
        affected.push_back(source);
      }
      patch_candidates[source].insert(patch_candidates[source].end(),
                                      out_neighbors.begin(),
                                      out_neighbors.end());
      patch_candidates[source].insert(patch_candidates[source].end(),
                                      in_neighbors.begin(),
                                      in_neighbors.end());
    }
  }

  for (const auto source : affected) {
    std::vector<std::uint32_t> candidates;
    candidates.reserve(graph[source].size() + patch_candidates[source].size());
    for (const auto neighbor : graph[source]) {
      if (neighbor < deleted.size() && !deleted[neighbor]) {
        candidates.push_back(neighbor);
      }
    }
    candidates.insert(candidates.end(), patch_candidates[source].begin(),
                      patch_candidates[source].end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    graph[source] =
        robust_prune_neighbors(base, source, candidates, degree, alpha, deleted);
  }

  for (const auto deleted_id : deleted_nodes) {
    if (deleted_id < graph.size()) {
      graph[deleted_id].clear();
    }
  }
}

std::uint32_t compute_lsh_signature(const float* row,
                                    const std::vector<float>& planes,
                                    std::size_t table, std::size_t bits,
                                    std::size_t dim) {
  std::uint32_t signature = 0;
  const std::size_t table_offset = table * bits * dim;
  for (std::size_t bit = 0; bit < bits; ++bit) {
    const float* plane = planes.data() + table_offset + bit * dim;
    float score = 0.0f;
    for (std::size_t d = 0; d < dim; ++d) {
      score += row[d] * plane[d];
    }
    if (score >= 0.0f) {
      signature |= (1u << bit);
    }
  }
  return signature;
}

LshIndex build_lsh_index(const VectorSet& base,
                         const DiskGraphBuildConfig& config) {
  // 多表 SimHash 只负责生成候选；最终边仍按原始向量距离筛选。
  LshIndex index;
  index.bits = config.lsh_bits;
  index.tables.resize(config.lsh_tables);

  const std::size_t bucket_count = std::size_t{1} << config.lsh_bits;
  std::vector<float> planes(config.lsh_tables * config.lsh_bits * base.dim);
  std::mt19937 rng(config.random_seed ^ 0x51f15eedu);
  std::uniform_int_distribution<int> sign_pick(0, 1);
  for (float& value : planes) {
    value = sign_pick(rng) == 0 ? -1.0f : 1.0f;
  }

  for (std::size_t t = 0; t < config.lsh_tables; ++t) {
    auto& table = index.tables[t];
    table.signatures.resize(base.size());
    table.offsets.assign(bucket_count + 1, 0);
    for (std::size_t i = 0; i < base.size(); ++i) {
      const auto signature =
          compute_lsh_signature(base.row(i), planes, t, config.lsh_bits,
                                base.dim);
      table.signatures[i] = signature;
      ++table.offsets[signature + 1];  // 先计数，再转为 CSR 前缀和。
    }

    for (std::size_t bucket = 1; bucket < table.offsets.size(); ++bucket) {
      table.offsets[bucket] += table.offsets[bucket - 1];
    }

    table.ids_by_bucket.resize(base.size());
    std::vector<std::uint32_t> cursor = table.offsets;
    for (std::size_t i = 0; i < base.size(); ++i) {
      const auto signature = table.signatures[i];
      table.ids_by_bucket[cursor[signature]++] =
          static_cast<std::uint32_t>(i);
    }
  }
  return index;
}

void add_lsh_bucket_candidates(std::vector<std::uint32_t>& candidates,
                               std::uint32_t self, const LshTable& table,
                               std::uint32_t signature,
                               const DiskGraphBuildConfig& config) {
  if (signature + 1 >= table.offsets.size()) {
    return;
  }
  const std::uint32_t begin = table.offsets[signature];
  const std::uint32_t end = table.offsets[signature + 1];
  const std::uint32_t size = end - begin;
  if (size <= config.lsh_bucket_limit) {
    for (std::uint32_t i = begin; i < end; ++i) {
      candidates.push_back(table.ids_by_bucket[i]);
    }
    return;
  }

  const std::uint32_t seed = mix_u32(self ^ signature ^ 0x15a4e35u);
  for (std::size_t i = 0; i < config.lsh_bucket_limit; ++i) {
    const auto offset = mix_u32(seed + static_cast<std::uint32_t>(i)) % size;
    candidates.push_back(table.ids_by_bucket[begin + offset]);
  }
}

void add_lsh_candidates(std::vector<std::uint32_t>& candidates,
                        std::uint32_t self, const LshIndex& index,
                        const DiskGraphBuildConfig& config) {
  const std::uint32_t mask =
      config.lsh_bits == 32 ? std::numeric_limits<std::uint32_t>::max()
                            : ((1u << config.lsh_bits) - 1u);
  for (const auto& table : index.tables) {
    const auto signature = table.signatures[self] & mask;
    add_lsh_bucket_candidates(candidates, self, table, signature, config);
    if (config.lsh_probe_radius == 1) {
      for (std::size_t bit = 0; bit < config.lsh_bits; ++bit) {
        add_lsh_bucket_candidates(candidates, self, table,
                                  signature ^ (1u << bit), config);
      }
    }
  }
}

std::vector<std::vector<std::uint32_t>> build_approx_projection_graph(
    const VectorSet& base, const DiskGraphBuildConfig& config,
    const std::vector<char>& deleted) {
  const ProjectionIndex projection_index = build_projection_index(base, config);
  std::vector<std::vector<std::uint32_t>> graph(base.size());

  for (std::size_t i = 0; i < base.size(); ++i) {
    if (i < deleted.size() && deleted[i]) {
      continue;
    }
    std::vector<std::uint32_t> candidates;
    candidates.reserve(config.approx_projections *
                           (config.approx_window * 2 + 1) +
                       config.approx_random_samples + config.approx_window * 2);
    const auto self = static_cast<std::uint32_t>(i);
    add_projection_candidates(candidates, self, projection_index, config,
                              base.size());
    add_deterministic_samples(candidates, self, config, base.size());
    deduplicate_and_limit(candidates, self, config, base.size(), deleted);
    record_candidate_count(config, candidates.size());
    graph[i] = robust_prune_neighbors(base, self, candidates, config.degree,
                                      config.robust_prune_alpha, deleted);
  }

  if (config.reverse_edge_patch) {
    const std::size_t passes = std::max<std::size_t>(1, config.prune_passes);
    for (std::size_t pass = 0; pass < passes; ++pass) {
      const std::size_t added =
          add_reverse_edges_batch(base, graph, config.degree, deleted);
      if (config.stats != nullptr) {
        config.stats->reverse_edges_added += added;
      }
    }
  }
  return graph;
}

std::vector<std::vector<std::uint32_t>> build_lsh_projection_graph(
    const VectorSet& base, const DiskGraphBuildConfig& config,
    const std::vector<char>& deleted) {
  // lsh-rp 是 SIFT100K/SIFT1M 的快速建图路径，避免 exact 的二次复杂度。
  const LshIndex lsh_index = build_lsh_index(base, config);
  std::vector<std::vector<std::uint32_t>> graph(base.size());

  for (std::size_t i = 0; i < base.size(); ++i) {
    if (i < deleted.size() && deleted[i]) {
      continue;
    }
    std::vector<std::uint32_t> candidates;
    const auto probe_multiplier =
        config.lsh_probe_radius == 0 ? 1 : (config.lsh_bits + 1);
    candidates.reserve(config.lsh_tables * probe_multiplier *
                           config.lsh_bucket_limit +
                       config.approx_random_samples + config.approx_window * 2);
    const auto self = static_cast<std::uint32_t>(i);
    add_lsh_candidates(candidates, self, lsh_index, config);
    add_deterministic_samples(candidates, self, config, base.size());
    deduplicate_and_limit(candidates, self, config, base.size(), deleted);
    record_candidate_count(config, candidates.size());
    if (config.build_policy == "lsh-vamana") {
      graph[i] = robust_prune_neighbors(base, self, candidates, config.degree,
                                        config.robust_prune_alpha, deleted);
    } else {
      graph[i] = nearest_neighbors_from_candidates(
          base, self, candidates, config.degree, deleted);
    }
  }

  if (config.reverse_edge_patch) {
    const std::size_t passes = std::max<std::size_t>(1, config.prune_passes);
    for (std::size_t pass = 0; pass < passes; ++pass) {
      if (config.build_policy == "lsh-vamana") {
        const ReversePatchStats patch = add_reverse_edges_vamana(
            base, graph, config.degree, config.robust_prune_alpha, deleted);
        const std::size_t overfull_prunes = prune_overfull_nodes(
            base, graph, config.degree, config.robust_prune_alpha, deleted);
        if (config.stats != nullptr) {
          config.stats->reverse_edges_added += patch.reverse_edges_added;
          config.stats->reverse_prune_count +=
              patch.reverse_prune_count + overfull_prunes;
        }
      } else {
        const std::size_t added =
            add_reverse_edges_batch(base, graph, config.degree, deleted);
        if (config.stats != nullptr) {
          config.stats->reverse_edges_added += added;
        }
      }
    }
  }
  return graph;
}

std::vector<std::vector<std::uint32_t>> build_graph(
    const VectorSet& base, const DiskGraphBuildConfig& config) {
  // 三种 builder 共用同一输出结构，删除修补作为统一的后处理阶段。
  validate_build_policy(config);
  const auto deleted = deleted_mask(base.size(), config.deleted_ids);
  const std::vector<char> no_deleted(base.size(), 0);
  std::vector<std::vector<std::uint32_t>> graph;
  if (config.build_policy == "exact") {
    graph = build_exact_graph(base, config.degree, no_deleted);
  } else if (config.build_policy == "vamana") {
    VamanaBuildConfig vamana_config;
    vamana_config.max_degree = config.degree;
    vamana_config.search_width =
        config.approx_candidate_limit == 0
            ? std::max<std::size_t>(config.degree * 8, 200)
            : std::max<std::size_t>(config.approx_candidate_limit,
                                    config.degree);
    vamana_config.alpha = config.robust_prune_alpha;
    vamana_config.seed = config.random_seed;
    vamana_config.add_reverse_edges = config.reverse_edge_patch;
    vamana_config.deleted_ids = config.deleted_ids;
    const VamanaGraph vamana = VamanaBuilder(vamana_config).build(base);
    graph = vamana.adjacency;
    if (config.stats != nullptr) {
      config.stats->reverse_edges_added +=
          vamana.stats.reverse_edges_added;
      config.stats->reverse_prune_count +=
          vamana.stats.reverse_prune_count;
      config.stats->candidate_counts.insert(
          config.stats->candidate_counts.end(),
          vamana.stats.candidate_counts.begin(),
          vamana.stats.candidate_counts.end());
    }
  } else if (config.build_policy == "approx-rp") {
    graph = build_approx_projection_graph(base, config, no_deleted);
  } else {
    graph = build_lsh_projection_graph(base, config, no_deleted);
  }
  if (!config.deleted_ids.empty() && config.build_policy != "vamana") {
    apply_freshvamana_delete_patch(base, graph, config.degree,
                                   config.robust_prune_alpha, deleted);
  }
  if (config.stats != nullptr && !graph.empty()) {
    std::size_t degree_sum = 0;
    std::size_t max_degree = 0;
    for (const auto& neighbors : graph) {
      degree_sum += neighbors.size();
      max_degree = std::max(max_degree, neighbors.size());
    }
    config.stats->avg_degree =
        static_cast<double>(degree_sum) / static_cast<double>(graph.size());
    config.stats->max_degree = max_degree;
    if (!config.stats->candidate_counts.empty()) {
      std::vector<std::size_t> sorted = config.stats->candidate_counts;
      std::sort(sorted.begin(), sorted.end());
      const auto percentile = [&](double p) {
        const double pos =
            (static_cast<double>(sorted.size()) - 1.0) * p;
        const auto lower = static_cast<std::size_t>(pos);
        const auto upper = std::min(lower + 1, sorted.size() - 1);
        const double fraction = pos - static_cast<double>(lower);
        return static_cast<double>(sorted[lower]) * (1.0 - fraction) +
               static_cast<double>(sorted[upper]) * fraction;
      };
      const double sum = static_cast<double>(std::accumulate(
          sorted.begin(), sorted.end(), static_cast<std::size_t>(0)));
      config.stats->graph_candidate_avg =
          sum / static_cast<double>(sorted.size());
      config.stats->graph_candidate_p95 = percentile(0.95);
      config.stats->graph_candidate_p99 = percentile(0.99);
    }
  }
  return graph;
}

std::vector<std::uint32_t> entry_points(std::uint64_t count,
                                        std::size_t requested) {
  std::vector<std::uint32_t> entries;
  if (count == 0 || requested == 0) {
    return entries;
  }

  const std::size_t actual =
      std::min<std::size_t>(requested, static_cast<std::size_t>(count));
  std::unordered_set<std::uint32_t> seen;
  entries.reserve(actual);

  for (std::size_t i = 0; i < actual; ++i) {
    std::uint64_t id = 0;
    if (actual == 1) {
      id = 0;
    } else {
      id = (static_cast<std::uint64_t>(i) * (count - 1)) /
           static_cast<std::uint64_t>(actual - 1);
    }
    const auto id32 = static_cast<std::uint32_t>(id);
    if (seen.insert(id32).second) {
      entries.push_back(id32);
    }
  }

  return entries;
}

std::vector<std::uint32_t> natural_order(std::size_t count) {
  std::vector<std::uint32_t> order;
  order.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    order.push_back(static_cast<std::uint32_t>(i));
  }
  return order;
}

std::vector<std::uint32_t> random_order(std::size_t count, std::uint32_t seed) {
  auto order = natural_order(count);
  std::mt19937 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

std::vector<std::uint32_t> bfs_order(
    const std::vector<std::vector<std::uint32_t>>& graph) {
  std::vector<std::uint32_t> order;
  order.reserve(graph.size());
  std::vector<char> seen(graph.size(), 0);
  std::deque<std::uint32_t> queue;

  for (std::size_t start = 0; start < graph.size(); ++start) {
    if (seen[start]) {
      continue;
    }
    seen[start] = 1;
    queue.push_back(static_cast<std::uint32_t>(start));
    while (!queue.empty()) {
      const auto current = queue.front();
      queue.pop_front();
      order.push_back(current);
      for (const auto neighbor : graph[current]) {
        if (neighbor < seen.size() && !seen[neighbor]) {
          seen[neighbor] = 1;
          queue.push_back(neighbor);
        }
      }
    }
  }

  return order;
}

std::vector<std::uint32_t> coaccess_order(
    const std::vector<std::vector<std::uint32_t>>& graph,
    std::size_t nodes_per_page, std::size_t sessions, std::size_t trace_length) {
  // 模拟 Agent 会话访问轨迹，将经常共同访问的节点尽量压入同一磁盘页。
  std::vector<std::uint32_t> order;
  order.reserve(graph.size());
  if (graph.empty()) {
    return order;
  }

  sessions = std::max<std::size_t>(1, std::min(sessions, graph.size()));
  trace_length = std::max<std::size_t>(1, trace_length);

  std::vector<double> hotness(graph.size(), 1.0);
  std::unordered_map<std::uint64_t, double> coaccess;

  for (std::size_t s = 0; s < sessions; ++s) {
    std::uint32_t current = static_cast<std::uint32_t>(
        (s * graph.size()) / sessions);
    std::uint32_t previous = std::numeric_limits<std::uint32_t>::max();

    for (std::size_t step = 0; step < trace_length; ++step) {
      hotness[current] += 2.0;
      if (previous != std::numeric_limits<std::uint32_t>::max()) {
        coaccess[pair_key(previous, current)] += 4.0;
      }

      const auto& neighbors = graph[current];
      for (std::size_t i = 0; i < std::min<std::size_t>(4, neighbors.size());
           ++i) {
        coaccess[pair_key(current, neighbors[i])] += 1.0;
      }

      if (neighbors.empty()) {
        break;
      }
      previous = current;
      current = neighbors[(step + s) % neighbors.size()];
    }
  }

  std::vector<char> placed(graph.size(), 0);
  std::size_t remaining = graph.size();
  std::size_t next_unplaced = 0;

  auto take_next_unplaced = [&]() -> std::uint32_t {
    while (next_unplaced < placed.size() && placed[next_unplaced]) {
      ++next_unplaced;
    }
    if (next_unplaced >= placed.size()) {
      return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(next_unplaced);
  };

  auto score_for_page = [&](std::uint32_t candidate,
                            const std::vector<std::uint32_t>& page_nodes) {
    double score = hotness[candidate] * 0.001;
    for (const auto page_node : page_nodes) {
      const auto found = coaccess.find(pair_key(candidate, page_node));
      if (found != coaccess.end()) {
        score += found->second * 20.0;
      }
      if (contains_id(graph[candidate], page_node) ||
          contains_id(graph[page_node], candidate)) {
        score += 10.0;
      }
    }
    return score;
  };

  while (remaining > 0) {
    std::vector<std::uint32_t> page_nodes;
    page_nodes.reserve(nodes_per_page);
    const std::uint32_t anchor = take_next_unplaced();
    if (anchor == std::numeric_limits<std::uint32_t>::max()) {
      break;
    }
    placed[anchor] = 1;
    --remaining;
    page_nodes.push_back(anchor);
    order.push_back(anchor);

    while (remaining > 0 && page_nodes.size() < nodes_per_page) {
      std::vector<std::uint32_t> candidates;
      for (const auto page_node : page_nodes) {
        for (const auto neighbor : graph[page_node]) {
          if (neighbor < placed.size() && !placed[neighbor]) {
            candidates.push_back(neighbor);
          }
        }
      }
      std::sort(candidates.begin(), candidates.end());
      candidates.erase(std::unique(candidates.begin(), candidates.end()),
                       candidates.end());

      std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
      double best_score = -1.0;
      for (const auto candidate : candidates) {
        const double score = score_for_page(candidate, page_nodes);
        if (score > best_score) {
          best_score = score;
          best = candidate;
        }
      }
      if (best == std::numeric_limits<std::uint32_t>::max()) {
        best = take_next_unplaced();
      }
      if (best == std::numeric_limits<std::uint32_t>::max()) {
        break;
      }

      placed[best] = 1;
      --remaining;
      page_nodes.push_back(best);
      order.push_back(best);
    }
  }

  return order;
}

std::vector<std::uint32_t> hotpath_order(
    const VectorSet& base, const std::vector<std::vector<std::uint32_t>>& graph,
    const DiskGraphBuildConfig& config) {
  // 用真实查询样本统计访问热度，再按访问次数重排节点。
  if (config.hotpath_queries == nullptr || config.hotpath_queries->empty()) {
    throw std::runtime_error(
        "hotpath packing requires sampled queries during index build");
  }
  if (config.hotpath_queries->dim != base.dim) {
    throw std::runtime_error("hotpath query dimension does not match base");
  }

  const std::size_t train_queries =
      std::min(config.hotpath_train_queries, config.hotpath_queries->size());
  std::vector<std::size_t> visit_count(graph.size(), 0);
  const auto entries = entry_points(graph.size(), config.hotpath_entry_count);

  for (std::size_t query_id = 0; query_id < train_queries; ++query_id) {
    const float* query = config.hotpath_queries->row(query_id);
    std::vector<char> visited(graph.size(), 0);
    std::priority_queue<SearchResult, std::vector<SearchResult>, CloserFirst>
        candidates;

    for (const auto entry : entries) {
      if (entry >= graph.size() || visited[entry]) {
        continue;
      }
      visited[entry] = 1;
      ++visit_count[entry];
      candidates.push(SearchResult{
          entry, squared_l2(query, base.row(entry), base.dim)});
    }

    std::size_t expanded = 0;
    while (!candidates.empty() && expanded < config.hotpath_search_width) {
      const auto current = candidates.top();
      candidates.pop();
      ++expanded;
      for (const auto neighbor : graph[current.id]) {
        if (neighbor >= graph.size() || visited[neighbor]) {
          continue;
        }
        visited[neighbor] = 1;
        ++visit_count[neighbor];
        candidates.push(SearchResult{
            neighbor, squared_l2(query, base.row(neighbor), base.dim)});
      }
    }
  }

  std::vector<std::uint32_t> order = natural_order(graph.size());
  std::stable_sort(order.begin(), order.end(),
                   [&](std::uint32_t lhs, std::uint32_t rhs) {
                     if (visit_count[lhs] == visit_count[rhs]) {
                       return lhs < rhs;
                     }
                     return visit_count[lhs] > visit_count[rhs];
                   });

  if (config.stats != nullptr) {
    config.stats->hotpath_train_queries = train_queries;
    config.stats->hotpath_unique_visited_nodes =
        static_cast<std::size_t>(std::count_if(
            visit_count.begin(), visit_count.end(),
            [](std::size_t count) { return count != 0; }));
    config.stats->hotpath_top_node_visit_count =
        visit_count.empty()
            ? 0
            : *std::max_element(visit_count.begin(), visit_count.end());
  }
  return order;
}

std::vector<std::uint32_t> packed_order(
    const VectorSet& base, const std::vector<std::vector<std::uint32_t>>& graph,
    const DiskGraphBuildConfig& config, std::size_t nodes_per_page) {
  if (config.packing_strategy == "random") {
    return random_order(graph.size(), config.random_seed);
  }
  if (config.packing_strategy == "bfs") {
    return bfs_order(graph);
  }
  if (config.packing_strategy == "coaccess") {
    return coaccess_order(graph, nodes_per_page, config.coaccess_sessions,
                          config.coaccess_trace_length);
  }
  if (config.packing_strategy == "hotpath") {
    return hotpath_order(base, graph, config);
  }
  throw std::runtime_error("Unknown packing strategy: " +
                           config.packing_strategy);
}

}  // namespace

void NaiveDiskGraphBuilder::build(const VectorSet& base,
                                  const std::string& path,
                                  const DiskGraphBuildConfig& config) {
  // V1 基线：一个向量固定占用一个磁盘页，便于观察随机读放大。
  if (base.empty()) {
    throw std::runtime_error("Cannot build graph index for empty base vectors");
  }
  if (base.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("V1 graph index supports at most uint32_t ids");
  }
  if (config.degree == 0) {
    throw std::runtime_error("Graph degree must be positive");
  }
  if (config.page_size < kRecordsOffset) {
    throw std::runtime_error("Page size must be at least 4096 bytes in V1");
  }
  if (record_bytes(base.dim, config.degree) > config.page_size) {
    throw std::runtime_error("Node record does not fit in configured page size");
  }
  const auto graph = build_graph(base, config);

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create graph index: " + path);
  }

  output.write(kMagic, sizeof(kMagic));
  write_value(output, kVersion);
  write_value(output, static_cast<std::uint64_t>(base.size()));
  write_value(output, static_cast<std::uint32_t>(base.dim));
  write_value(output, static_cast<std::uint32_t>(config.degree));
  write_value(output, static_cast<std::uint32_t>(config.page_size));
  write_value(output, kRecordsOffset);

  const auto header_pos = static_cast<std::uint64_t>(output.tellp());
  if (header_pos > kRecordsOffset) {
    throw std::runtime_error("Graph header exceeds reserved header page");
  }
  std::vector<char> header_padding(
      static_cast<std::size_t>(kRecordsOffset - header_pos), 0);
  output.write(header_padding.data(),
               static_cast<std::streamsize>(header_padding.size()));

  for (std::size_t i = 0; i < base.size(); ++i) {
    const auto& neighbors = graph[i];
    std::vector<char> page(config.page_size, 0);
    std::size_t offset = 0;
    const auto id = static_cast<std::uint32_t>(i);
    const auto degree = static_cast<std::uint32_t>(neighbors.size());
    put_bytes(page, offset, id);
    put_bytes(page, offset, degree);
    std::memcpy(page.data() + offset, base.row(i), base.dim * sizeof(float));
    offset += base.dim * sizeof(float);

    for (std::size_t n = 0; n < config.degree; ++n) {
      const std::uint32_t neighbor =
          n < neighbors.size() ? neighbors[n] : std::numeric_limits<std::uint32_t>::max();
      put_bytes(page, offset, neighbor);
    }

    output.write(page.data(), static_cast<std::streamsize>(page.size()));
    if (!output) {
      throw std::runtime_error("Failed while writing graph node page");
    }
  }
}


void PackedDiskGraphBuilder::build(const VectorSet& base,
                                   const std::string& path,
                                   const DiskGraphBuildConfig& config) {
  // V2 布局：多个节点共用一个页，降低图遍历过程中触发的页读取次数。
  if (base.empty()) {
    throw std::runtime_error("Cannot build packed graph index for empty base vectors");
  }
  if (base.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("V2 packed graph index supports at most uint32_t ids");
  }
  if (config.degree == 0) {
    throw std::runtime_error("Graph degree must be positive");
  }
  if (config.page_size < kRecordsOffset) {
    throw std::runtime_error("Page size must be at least 4096 bytes in V2");
  }

  const std::size_t nodes_per_page =
      packed_nodes_per_page(base.dim, config.degree, config.page_size);
  if (nodes_per_page == 0) {
    throw std::runtime_error("Packed page cannot hold even one node");
  }

  const auto graph = build_graph(base, config);
  const auto order = packed_order(base, graph, config, nodes_per_page);
  if (order.size() != base.size()) {
    throw std::runtime_error("Packed order does not cover all nodes");
  }

  const std::uint64_t page_count =
      (static_cast<std::uint64_t>(base.size()) + nodes_per_page - 1) /
      nodes_per_page;
  if (page_count > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("Too many packed pages for V2 directory");
  }

  const std::uint64_t directory_offset = kRecordsOffset;
  const std::uint64_t directory_bytes =
      static_cast<std::uint64_t>(base.size()) * sizeof(std::uint32_t);
  const std::uint64_t records_offset =
      align_up(directory_offset + directory_bytes, config.page_size);

  std::vector<std::uint32_t> node_to_page(base.size(), 0);  // 查询时 O(1) 定位所在页。
  for (std::size_t position = 0; position < order.size(); ++position) {
    const std::uint32_t page_id =
        static_cast<std::uint32_t>(position / nodes_per_page);
    node_to_page[order[position]] = page_id;
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create packed graph index: " + path);
  }

  output.write(kPackedMagic, sizeof(kPackedMagic));
  write_value(output, kPackedVersion);
  write_value(output, static_cast<std::uint64_t>(base.size()));
  write_value(output, static_cast<std::uint32_t>(base.dim));
  write_value(output, static_cast<std::uint32_t>(config.degree));
  write_value(output, static_cast<std::uint32_t>(config.page_size));
  write_value(output, records_offset);
  write_value(output, directory_offset);
  write_value(output, page_count);
  write_value(output, static_cast<std::uint32_t>(nodes_per_page));

  const auto header_pos = static_cast<std::uint64_t>(output.tellp());
  if (header_pos > kRecordsOffset) {
    throw std::runtime_error("Packed graph header exceeds reserved header page");
  }
  std::vector<char> header_padding(
      static_cast<std::size_t>(kRecordsOffset - header_pos), 0);
  output.write(header_padding.data(),
               static_cast<std::streamsize>(header_padding.size()));

  output.write(reinterpret_cast<const char*>(node_to_page.data()),
               static_cast<std::streamsize>(directory_bytes));
  const auto after_directory = static_cast<std::uint64_t>(output.tellp());
  if (after_directory > records_offset) {
    throw std::runtime_error("Packed graph directory exceeds records offset");
  }
  std::vector<char> directory_padding(
      static_cast<std::size_t>(records_offset - after_directory), 0);
  output.write(directory_padding.data(),
               static_cast<std::streamsize>(directory_padding.size()));

  for (std::uint64_t page_id = 0; page_id < page_count; ++page_id) {
    std::vector<char> page = DiskPageCodec::encode_packed_page(
        static_cast<std::uint32_t>(page_id), base, graph, order,
        nodes_per_page, config.page_size, config.degree);

    output.write(page.data(), static_cast<std::streamsize>(page.size()));
    if (!output) {
      throw std::runtime_error("Failed while writing packed graph page");
    }
  }
}


}  // namespace agentmem
