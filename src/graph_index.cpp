#include "agentmem/graph_index.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#define AGENTMEM_HAS_IO_URING 1
#else
#define AGENTMEM_HAS_IO_URING 0
#endif
#else
#define AGENTMEM_HAS_IO_URING 0
#endif

#include "agentmem/brute_force.h"

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

template <typename T>
void write_value(std::ofstream& output, const T& value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(T));
  if (!output) {
    throw std::runtime_error("Failed to write graph index");
  }
}

template <typename T>
T read_value(std::ifstream& input) {
  T value{};
  input.read(reinterpret_cast<char*>(&value), sizeof(T));
  if (!input) {
    throw std::runtime_error("Failed to read graph index header");
  }
  return value;
}

template <typename T>
void put_bytes(std::vector<char>& page, std::size_t& offset, const T& value) {
  if (offset + sizeof(T) > page.size()) {
    throw std::runtime_error("Graph node page overflow");
  }
  std::memcpy(page.data() + offset, &value, sizeof(T));
  offset += sizeof(T);
}

template <typename T>
T get_bytes(const std::vector<char>& page, std::size_t& offset) {
  if (offset + sizeof(T) > page.size()) {
    throw std::runtime_error("Graph node page is truncated");
  }
  T value{};
  std::memcpy(&value, page.data() + offset, sizeof(T));
  offset += sizeof(T);
  return value;
}

std::size_t record_bytes(std::size_t dim, std::size_t degree) {
  return sizeof(std::uint32_t) * 2 + dim * sizeof(float) +  // id、degree、向量、邻居。
         degree * sizeof(std::uint32_t);
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
  if (alignment == 0) {
    return value;
  }
  const std::uint64_t remainder = value % alignment;
  return remainder == 0 ? value : value + alignment - remainder;
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
         config.build_policy == "lsh-rp";
}

void validate_build_policy(const DiskGraphBuildConfig& config) {
  if (config.build_policy != "exact" && !is_candidate_build_policy(config)) {
    throw std::runtime_error(
        "--graph-build-policy must be exact, approx-rp, or lsh-rp");
  }
  if (is_candidate_build_policy(config)) {
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
  if (config.build_policy == "lsh-rp") {
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

void add_reverse_edges_batch(const VectorSet& base,
                             std::vector<std::vector<std::uint32_t>>& graph,
                             std::size_t degree,
                             const std::vector<char>& deleted) {
  // 批量补反向边后再统一裁剪，避免逐边插入导致反复重算距离。
  const std::size_t count = graph.size();
  std::vector<std::vector<std::uint32_t>> reverse(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (i < deleted.size() && deleted[i]) {
      graph[i].clear();
      continue;
    }
    for (const auto neighbor : graph[i]) {
      if (neighbor < count && neighbor < deleted.size() && !deleted[neighbor]) {
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
    graph[i] = robust_prune_neighbors(base, self, candidates, config.degree,
                                      config.robust_prune_alpha, deleted);
  }

  add_reverse_edges_batch(base, graph, config.degree, deleted);
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
    graph[i] = nearest_neighbors_from_candidates(
        base, self, candidates, config.degree, deleted);
  }

  add_reverse_edges_batch(base, graph, config.degree, deleted);
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
  } else if (config.build_policy == "approx-rp") {
    graph = build_approx_projection_graph(base, config, no_deleted);
  } else {
    graph = build_lsh_projection_graph(base, config, no_deleted);
  }
  if (!config.deleted_ids.empty()) {
    apply_freshvamana_delete_patch(base, graph, config.degree,
                                   config.robust_prune_alpha, deleted);
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

std::vector<SearchResult> sorted_results(
    std::priority_queue<SearchResult, std::vector<SearchResult>,
                        WorseResultFirst>& heap) {
  std::vector<SearchResult> results;
  results.reserve(heap.size());
  while (!heap.empty()) {
    results.push_back(heap.top());
    heap.pop();
  }
  std::sort(results.begin(), results.end(),
            [](const SearchResult& lhs, const SearchResult& rhs) {
              if (lhs.distance == rhs.distance) {
                return lhs.id < rhs.id;
              }
              return lhs.distance < rhs.distance;
            });
  return results;
}

std::size_t packed_nodes_per_page(std::size_t dim, std::size_t degree,
                                  std::size_t page_size) {
  if (page_size <= sizeof(std::uint32_t) * 2) {
    return 0;
  }
  const std::size_t per_node = sizeof(std::uint32_t) * 2 +
                               dim * sizeof(float) +
                               degree * sizeof(std::uint32_t);
  return (page_size - sizeof(std::uint32_t) * 2) / per_node;
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

namespace {

#ifdef __linux__
constexpr std::size_t kDirectIoAlignment = 4096;

std::string errno_reason(const std::string& prefix) {
  return prefix + "_" + std::to_string(errno);
}
#endif

#if AGENTMEM_HAS_IO_URING && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
// 直接调用 Linux syscall 的最小 io_uring 包装，避免引入 liburing 依赖。
class RawIoUring {
 public:
  RawIoUring() = default;

  ~RawIoUring() {
    close();
  }

  RawIoUring(const RawIoUring&) = delete;
  RawIoUring& operator=(const RawIoUring&) = delete;

  struct Completion {
    std::uint64_t user_data = 0;
    int result = 0;
  };

  bool open(std::size_t entries, std::string& reason) {
    close();
    io_uring_params params{};
    const auto depth = static_cast<unsigned>(
        std::max<std::size_t>(1, std::min<std::size_t>(entries, 4096)));
    ring_fd_ = static_cast<int>(
        syscall(__NR_io_uring_setup, depth, &params));
    if (ring_fd_ < 0) {
      reason = errno_reason("io_uring_setup_failed_errno");
      return false;
    }

    sq_ring_size_ =
        params.sq_off.array + params.sq_entries * sizeof(unsigned);
    cq_ring_size_ =
        params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
    if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0) {
      sq_ring_size_ = std::max(sq_ring_size_, cq_ring_size_);
      cq_ring_size_ = sq_ring_size_;
      single_mmap_ = true;
    }

    sq_ring_ = mmap(nullptr, sq_ring_size_, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, ring_fd_,
                    IORING_OFF_SQ_RING);
    if (sq_ring_ == MAP_FAILED) {
      sq_ring_ = nullptr;
      reason = errno_reason("io_uring_sq_mmap_failed_errno");
      close();
      return false;
    }

    if (single_mmap_) {
      cq_ring_ = sq_ring_;
    } else {
      cq_ring_ = mmap(nullptr, cq_ring_size_, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, ring_fd_,
                      IORING_OFF_CQ_RING);
      if (cq_ring_ == MAP_FAILED) {
        cq_ring_ = nullptr;
        reason = errno_reason("io_uring_cq_mmap_failed_errno");
        close();
        return false;
      }
    }

    sqes_size_ = params.sq_entries * sizeof(io_uring_sqe);
    sqes_ = static_cast<io_uring_sqe*>(
        mmap(nullptr, sqes_size_, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES));
    if (sqes_ == MAP_FAILED) {
      sqes_ = nullptr;
      reason = errno_reason("io_uring_sqe_mmap_failed_errno");
      close();
      return false;
    }

    const auto* sq = static_cast<char*>(sq_ring_);
    const auto* cq = static_cast<char*>(cq_ring_);
    sq_head_ = reinterpret_cast<unsigned*>(
        const_cast<char*>(sq) + params.sq_off.head);
    sq_tail_ = reinterpret_cast<unsigned*>(
        const_cast<char*>(sq) + params.sq_off.tail);
    sq_ring_mask_ = reinterpret_cast<unsigned*>(
        const_cast<char*>(sq) + params.sq_off.ring_mask);
    sq_ring_entries_ = reinterpret_cast<unsigned*>(
        const_cast<char*>(sq) + params.sq_off.ring_entries);
    sq_array_ = reinterpret_cast<unsigned*>(
        const_cast<char*>(sq) + params.sq_off.array);
    cq_head_ = reinterpret_cast<unsigned*>(
        const_cast<char*>(cq) + params.cq_off.head);
    cq_tail_ = reinterpret_cast<unsigned*>(
        const_cast<char*>(cq) + params.cq_off.tail);
    cq_ring_mask_ = reinterpret_cast<unsigned*>(
        const_cast<char*>(cq) + params.cq_off.ring_mask);
    cqes_ = reinterpret_cast<io_uring_cqe*>(
        const_cast<char*>(cq) + params.cq_off.cqes);
    return true;
  }

  void close() {
    if (sqes_ != nullptr) {
      munmap(sqes_, sqes_size_);
      sqes_ = nullptr;
    }
    if (cq_ring_ != nullptr && !single_mmap_) {
      munmap(cq_ring_, cq_ring_size_);
    }
    cq_ring_ = nullptr;
    if (sq_ring_ != nullptr) {
      munmap(sq_ring_, sq_ring_size_);
      sq_ring_ = nullptr;
    }
    if (ring_fd_ >= 0) {
      ::close(ring_fd_);
      ring_fd_ = -1;
    }
    single_mmap_ = false;
    queued_submissions_ = 0;
  }

  bool queue_read(int fd, std::uint64_t offset, void* buffer,
                  std::size_t bytes, std::uint64_t user_data,
                  std::string& reason) {
    const unsigned head = *sq_head_;
    const unsigned tail = *sq_tail_;
    if (tail - head >= *sq_ring_entries_) {
      reason = "io_uring_submission_queue_full";
      return false;
    }

    const unsigned index = tail & *sq_ring_mask_;
    io_uring_sqe& sqe = sqes_[index];
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_READ;
    sqe.fd = fd;
    sqe.off = offset;
    sqe.addr = reinterpret_cast<std::uint64_t>(buffer);
    sqe.len = static_cast<std::uint32_t>(bytes);
    sqe.user_data = user_data;
    sq_array_[index] = index;
    std::atomic_thread_fence(std::memory_order_release);
    *sq_tail_ = tail + 1;
    ++queued_submissions_;
    return true;
  }

  bool submit_queued(std::string& reason,
                     std::size_t* submit_syscalls = nullptr) {
    std::size_t syscall_count = 0;
    while (queued_submissions_ > 0) {
      const int submitted = static_cast<int>(
          syscall(__NR_io_uring_enter, ring_fd_, queued_submissions_, 0, 0,
                  nullptr, 0));
      if (submitted < 0) {
        reason = errno_reason("io_uring_enter_failed_errno");
        return false;
      }
      if (submitted == 0) {
        reason = "io_uring_enter_submitted_zero";
        return false;
      }
      ++syscall_count;
      queued_submissions_ -= static_cast<unsigned>(submitted);
    }
    if (submit_syscalls != nullptr) {
      *submit_syscalls += syscall_count;
    }
    return true;
  }

  bool has_completion() const {
    return *cq_head_ != *cq_tail_;
  }

  bool pop_completion(Completion& completion, bool wait,
                      std::string& reason) {
    if (!has_completion() && wait) {
      const int waited = static_cast<int>(
          syscall(__NR_io_uring_enter, ring_fd_, 0, 1,
                  IORING_ENTER_GETEVENTS, nullptr, 0));
      if (waited < 0) {
        reason = errno_reason("io_uring_wait_failed_errno");
        return false;
      }
    }
    if (!has_completion()) {
      return false;
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    const unsigned cq_head = *cq_head_;
    const io_uring_cqe& cqe = cqes_[cq_head & *cq_ring_mask_];
    completion.user_data = cqe.user_data;
    completion.result = cqe.res;
    *cq_head_ = cq_head + 1;
    return true;
  }

  bool read(int fd, std::uint64_t offset, void* buffer, std::size_t bytes,
            DiskGraphSearchStats& stats, std::string& reason) {
    const std::uint64_t user_data = next_user_data_++;
    std::size_t submit_syscalls = 0;
    if (!queue_read(fd, offset, buffer, bytes, user_data, reason) ||
        !submit_queued(reason, &submit_syscalls)) {
      return false;
    }
    ++stats.io_submits;
    stats.io_submit_syscalls += submit_syscalls;

    Completion completion;
    do {
      if (!pop_completion(completion, true, reason)) {
        return false;
      }
    } while (completion.user_data != user_data);

    ++stats.io_completions;
    const int result = completion.result;
    if (result != static_cast<int>(bytes)) {
      reason = result < 0
                   ? "io_uring_read_failed_errno_" + std::to_string(-result)
                   : "io_uring_short_read_" + std::to_string(result);
      return false;
    }
    return true;
  }

 private:
  int ring_fd_ = -1;
  void* sq_ring_ = nullptr;
  void* cq_ring_ = nullptr;
  io_uring_sqe* sqes_ = nullptr;
  std::size_t sq_ring_size_ = 0;
  std::size_t cq_ring_size_ = 0;
  std::size_t sqes_size_ = 0;
  bool single_mmap_ = false;
  unsigned* sq_head_ = nullptr;
  unsigned* sq_tail_ = nullptr;
  unsigned* sq_ring_mask_ = nullptr;
  unsigned* sq_ring_entries_ = nullptr;
  unsigned* sq_array_ = nullptr;
  unsigned* cq_head_ = nullptr;
  unsigned* cq_tail_ = nullptr;
  unsigned* cq_ring_mask_ = nullptr;
  io_uring_cqe* cqes_ = nullptr;
  std::uint64_t next_user_data_ = 1;
  unsigned queued_submissions_ = 0;
};
#endif

}  // namespace

// 屏蔽平台差异：Linux 可选 pread/O_DIRECT/io_uring，其他平台回退到 ifstream。
class DiskPageReader {
 public:
  DiskPageReader(std::string path, std::size_t page_size)
      : path_(std::move(path)), page_size_(page_size) {
    configure("pread", 1);
  }

  ~DiskPageReader() {
    close_native();
  }

  DiskPageReader(const DiskPageReader&) = delete;
  DiskPageReader& operator=(const DiskPageReader&) = delete;

  void configure(const std::string& mode, std::size_t batch_size) {
    if (mode != "pread" && mode != "odirect" && mode != "io_uring") {
      throw std::runtime_error("Unsupported disk I/O mode: " + mode);
    }
    if (batch_size == 0) {
      throw std::runtime_error("Disk I/O batch size must be positive");
    }

    close_native();
    status_ = DiskGraphIoStatus{};
    status_.requested_mode = mode;  // requested 与 effective 分开，便于实验识别回退。
    status_.batch_size = batch_size;

#ifdef __linux__
    if (mode == "pread") {
      open_linux_fd(O_RDONLY | O_CLOEXEC, "pread_open_failed_errno");
      return;
    }

    if (mode == "odirect") {
      if (!direct_io_layout_supported()) {  // O_DIRECT 要求页偏移和长度按 4 KB 对齐。
        fallback_to_pread("odirect_requires_4096_aligned_pages");
        return;
      }
      if (!try_open_linux_fd(O_RDONLY | O_CLOEXEC | O_DIRECT)) {
        fallback_to_pread(errno_reason("odirect_open_failed_errno"));
        return;
      }
      status_.effective_mode = "odirect";
      status_.direct_enabled = true;
      return;
    }

#if AGENTMEM_HAS_IO_URING && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
    bool direct_opened = false;
    if (direct_io_layout_supported()) {
      direct_opened = try_open_linux_fd(O_RDONLY | O_CLOEXEC | O_DIRECT);
    }
    if (!direct_opened && !try_open_linux_fd(O_RDONLY | O_CLOEXEC)) {
      throw std::runtime_error(
          "Cannot open graph index for io_uring: " + path_);
    }

    std::string reason;
    if (!ring_.open(batch_size, reason)) {
      fallback_to_pread(reason);
      return;
    }
    status_.effective_mode = "io_uring";
    status_.direct_enabled = direct_opened;
    status_.io_uring_enabled = true;
    if (!direct_opened) {
      status_.fallback_reason =
          "io_uring_active_with_buffered_fd_odirect_unavailable";
    }
#else
    fallback_to_pread("io_uring_headers_or_syscalls_unavailable");
#endif
#else
    if (mode == "pread") {
      open_buffered_stream();
      return;
    }
    fallback_to_pread(mode + "_requires_linux");
#endif
  }

  const DiskGraphIoStatus& status() const {
    return status_;
  }

  struct CompletedRead {
    std::uint64_t token = 0;
    std::vector<char> data;
  };

  bool async_enabled() const {
#ifdef __linux__
    return status_.effective_mode == "io_uring";
#else
    return false;
#endif
  }

  std::size_t max_pending_reads() const {
    return async_enabled() ? std::max<std::size_t>(1, status_.batch_size) : 0;
  }

  std::size_t pending_async_reads() const {
    return async_buffers_.size();
  }

  std::uint64_t start_async_read(std::uint64_t offset, std::size_t bytes,
                                 DiskGraphSearchStats& stats) {
    if (!async_enabled()) {
      throw std::runtime_error("Async graph reads require io_uring mode");
    }
#ifdef __linux__
#if AGENTMEM_HAS_IO_URING && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
    if (status_.direct_enabled &&
        (offset % kDirectIoAlignment != 0 || bytes % kDirectIoAlignment != 0)) {
      throw std::runtime_error(
          "Direct graph I/O requires aligned page offsets and lengths");
    }

    void* aligned = nullptr;
    if (posix_memalign(&aligned, kDirectIoAlignment, bytes) != 0) {
      throw std::runtime_error("Failed to allocate aligned graph I/O buffer");
    }

    const std::uint64_t token = next_async_token_++;
    std::string reason;
    if (!ring_.queue_read(fd_, offset, aligned, bytes, token, reason)) {
      std::free(aligned);
      throw std::runtime_error("Graph page async read failed: " + reason);
    }

    async_buffers_.emplace(token, AsyncBuffer{aligned, bytes});
    ++stats.io_submits;
    return token;
#else
    (void)offset;
    (void)bytes;
    (void)stats;
    throw std::runtime_error("io_uring headers or syscalls are unavailable");
#endif
#else
    (void)offset;
    (void)bytes;
    (void)stats;
    throw std::runtime_error("Async graph reads require Linux");
#endif
  }

  void submit_async_reads(DiskGraphSearchStats& stats) {
    if (!async_enabled()) {
      return;
    }
#ifdef __linux__
#if AGENTMEM_HAS_IO_URING && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
    std::string reason;
    std::size_t submit_syscalls = 0;
    if (!ring_.submit_queued(reason, &submit_syscalls)) {
      throw std::runtime_error("Graph page async submit failed: " + reason);
    }
    stats.io_submit_syscalls += submit_syscalls;
#endif
#endif
  }

  bool reap_async_read(bool wait, CompletedRead& completed,
                       DiskGraphSearchStats& stats) {
    if (!async_enabled()) {
      return false;
    }
#ifdef __linux__
#if AGENTMEM_HAS_IO_URING && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
    RawIoUring::Completion completion;
    std::string reason;
    if (!ring_.pop_completion(completion, wait, reason)) {
      if (!reason.empty()) {
        throw std::runtime_error("Graph page async wait failed: " + reason);
      }
      return false;
    }

    auto found = async_buffers_.find(completion.user_data);
    if (found == async_buffers_.end()) {
      throw std::runtime_error("Unknown graph async completion token");
    }
    AsyncBuffer buffer = found->second;
    async_buffers_.erase(found);
    ++stats.io_completions;

    if (completion.result != static_cast<int>(buffer.bytes)) {
      const std::string failure =
          completion.result < 0
              ? "io_uring_read_failed_errno_" +
                    std::to_string(-completion.result)
              : "io_uring_short_read_" + std::to_string(completion.result);
      std::free(buffer.aligned);
      throw std::runtime_error("Graph page async read failed: " + failure);
    }

    completed.token = completion.user_data;
    completed.data.assign(static_cast<const char*>(buffer.aligned),
                          static_cast<const char*>(buffer.aligned) +
                              buffer.bytes);
    std::free(buffer.aligned);
    return true;
#else
    (void)wait;
    (void)completed;
    (void)stats;
    return false;
#endif
#else
    (void)wait;
    (void)completed;
    (void)stats;
    return false;
#endif
  }

  std::vector<char> read(std::uint64_t offset, std::size_t bytes,
                         DiskGraphSearchStats& stats) {
    std::vector<char> output(bytes, 0);
#ifdef __linux__
    if (status_.effective_mode == "odirect" ||
        status_.effective_mode == "io_uring") {
      if (status_.effective_mode == "io_uring" && !async_buffers_.empty()) {
        throw std::runtime_error(
            "Cannot perform synchronous io_uring read with pending async reads");
      }
      if (status_.direct_enabled &&
          (offset % kDirectIoAlignment != 0 ||
           bytes % kDirectIoAlignment != 0)) {
        throw std::runtime_error(
            "Direct graph I/O requires aligned page offsets and lengths");
      }

      void* aligned = nullptr;  // O_DIRECT 不能直接读入普通 vector 缓冲区。
      if (posix_memalign(&aligned, kDirectIoAlignment, bytes) != 0) {
        throw std::runtime_error("Failed to allocate aligned graph I/O buffer");
      }
      bool ok = false;
      std::string reason;
      if (status_.effective_mode == "io_uring") {
#if AGENTMEM_HAS_IO_URING && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
        ok = ring_.read(fd_, offset, aligned, bytes, stats, reason);
#endif
      } else {
        ok = read_with_pread(aligned, bytes, offset, stats, reason);
      }
      if (ok) {
        std::memcpy(output.data(), aligned, bytes);
      }
      std::free(aligned);
      if (!ok) {
        throw std::runtime_error("Graph page read failed: " + reason);
      }
      return output;
    }

    std::string reason;
    if (!read_with_pread(output.data(), bytes, offset, stats, reason)) {
      throw std::runtime_error("Graph page read failed: " + reason);
    }
#else
    buffered_input_.clear();
    buffered_input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!buffered_input_) {
      throw std::runtime_error("Failed to seek graph index: " + path_);
    }
    buffered_input_.read(output.data(), static_cast<std::streamsize>(bytes));
    if (!buffered_input_) {
      throw std::runtime_error("Failed to read graph page");
    }
    ++stats.io_submits;
    ++stats.io_completions;
#endif
    return output;
  }

 private:
  struct AsyncBuffer {
    void* aligned = nullptr;
    std::size_t bytes = 0;
  };

#ifdef __linux__
  bool direct_io_layout_supported() const {
    return page_size_ % kDirectIoAlignment == 0;
  }

  bool try_open_linux_fd(int flags) {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    fd_ = ::open(path_.c_str(), flags);
    return fd_ >= 0;
  }

  void open_linux_fd(int flags, const std::string& error_prefix) {
    if (!try_open_linux_fd(flags)) {
      throw std::runtime_error(errno_reason(error_prefix) + ": " + path_);
    }
  }

  void fallback_to_pread(const std::string& reason) {
    close_native();
    open_linux_fd(O_RDONLY | O_CLOEXEC, "pread_fallback_open_failed_errno");
    status_.effective_mode = "pread";
    status_.direct_enabled = false;
    status_.io_uring_enabled = false;
    status_.fallback_reason = reason;
  }

  bool read_with_pread(void* buffer, std::size_t bytes, std::uint64_t offset,
                       DiskGraphSearchStats& stats, std::string& reason) {
    const ssize_t result =
        ::pread(fd_, buffer, bytes, static_cast<off_t>(offset));
    ++stats.io_submits;
    if (result < 0) {
      reason = errno_reason("pread_failed_errno");
      return false;
    }
    ++stats.io_completions;
    if (result != static_cast<ssize_t>(bytes)) {
      reason = "pread_short_read_" + std::to_string(result);
      return false;
    }
    return true;
  }
#else
  void open_buffered_stream() {
    buffered_input_.open(path_, std::ios::binary);
    if (!buffered_input_) {
      throw std::runtime_error("Cannot open graph index: " + path_);
    }
  }

  void fallback_to_pread(const std::string& reason) {
    open_buffered_stream();
    status_.effective_mode = "pread";
    status_.fallback_reason = reason;
  }
#endif

  void close_native() {
    for (auto& entry : async_buffers_) {
      std::free(entry.second.aligned);
    }
    async_buffers_.clear();
#ifdef __linux__
#if AGENTMEM_HAS_IO_URING && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
    ring_.close();
#endif
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#else
    if (buffered_input_.is_open()) {
      buffered_input_.close();
    }
#endif
  }

  std::string path_;
  std::size_t page_size_ = 0;
  DiskGraphIoStatus status_;
  std::unordered_map<std::uint64_t, AsyncBuffer> async_buffers_;
  std::uint64_t next_async_token_ = 1;
#ifdef __linux__
  int fd_ = -1;
#if AGENTMEM_HAS_IO_URING && defined(__NR_io_uring_setup) && \
    defined(__NR_io_uring_enter)
  RawIoUring ring_;
#endif
#else
  std::ifstream buffered_input_;
#endif
};

void PqAdcModel::train(const VectorSet& base, std::size_t subspaces,
                       std::size_t centroids, std::size_t train_limit,
                       std::size_t iterations, std::uint32_t seed) {
  // 将原始向量切为多个子空间，各自训练码本并为每个 base 向量生成短码。
  if (base.empty() || subspaces == 0 || centroids < 2 || centroids > 256 ||
      iterations == 0) {
    throw std::runtime_error("Invalid PQ training configuration");
  }
  subspaces_ = std::min(subspaces, base.dim);
  centroids_ = centroids;
  dim_ = base.dim;
  offsets_.resize(subspaces_ + 1);
  for (std::size_t m = 0; m <= subspaces_; ++m) {
    offsets_[m] = (m * dim_) / subspaces_;
  }

  const std::size_t sample_count =
      std::min(base.size(), std::max(centroids_, train_limit));
  std::vector<std::uint32_t> sample_ids(sample_count);
  for (std::size_t i = 0; i < sample_count; ++i) {
    sample_ids[i] = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(i) * base.size()) / sample_count);
  }

  codebooks_.assign(subspaces_ * centroids_ * dim_, 0.0f);
  std::mt19937 rng(seed ^ 0x0adc0deu);
  for (std::size_t m = 0; m < subspaces_; ++m) {
    const std::size_t begin = offsets_[m];
    const std::size_t end = offsets_[m + 1];
    for (std::size_t c = 0; c < centroids_; ++c) {
      const std::size_t sample =
          sample_ids[(c * sample_count) / centroids_];
      float* centroid = codebooks_.data() + (m * centroids_ + c) * dim_;
      std::copy(base.row(sample) + begin, base.row(sample) + end,
                centroid + begin);
    }

    std::vector<float> sums(centroids_ * (end - begin), 0.0f);
    std::vector<std::size_t> counts(centroids_, 0);
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
      std::fill(sums.begin(), sums.end(), 0.0f);
      std::fill(counts.begin(), counts.end(), 0);
      for (std::size_t i = 0; i < sample_count; ++i) {
        const float* row = base.row(sample_ids[i]);
        std::size_t best = 0;
        float best_distance = std::numeric_limits<float>::max();
        for (std::size_t c = 0; c < centroids_; ++c) {
          const float* centroid =
              codebooks_.data() + (m * centroids_ + c) * dim_;
          float distance = 0.0f;
          for (std::size_t d = begin; d < end; ++d) {
            const float diff = row[d] - centroid[d];
            distance += diff * diff;
          }
          if (distance < best_distance) {
            best_distance = distance;
            best = c;
          }
        }
        ++counts[best];
        for (std::size_t d = begin; d < end; ++d) {
          sums[best * (end - begin) + (d - begin)] += row[d];
        }
      }

      for (std::size_t c = 0; c < centroids_; ++c) {
        float* centroid = codebooks_.data() + (m * centroids_ + c) * dim_;
        if (counts[c] == 0) {
          const std::size_t replacement =
              sample_ids[std::uniform_int_distribution<std::size_t>(
                  0, sample_count - 1)(rng)];
          std::copy(base.row(replacement) + begin, base.row(replacement) + end,
                    centroid + begin);
          continue;
        }
        for (std::size_t d = begin; d < end; ++d) {
          centroid[d] =
              sums[c * (end - begin) + (d - begin)] /
              static_cast<float>(counts[c]);
        }
      }
    }
  }

  codes_.resize(base.size() * subspaces_);
  for (std::size_t i = 0; i < base.size(); ++i) {
    const float* row = base.row(i);
    for (std::size_t m = 0; m < subspaces_; ++m) {
      const std::size_t begin = offsets_[m];
      const std::size_t end = offsets_[m + 1];
      std::size_t best = 0;
      float best_distance = std::numeric_limits<float>::max();
      for (std::size_t c = 0; c < centroids_; ++c) {
        const float* centroid =
            codebooks_.data() + (m * centroids_ + c) * dim_;
        float distance = 0.0f;
        for (std::size_t d = begin; d < end; ++d) {
          const float diff = row[d] - centroid[d];
          distance += diff * diff;
        }
        if (distance < best_distance) {
          best_distance = distance;
          best = c;
        }
      }
      codes_[i * subspaces_ + m] = static_cast<std::uint8_t>(best);
    }
  }
}

std::vector<float> PqAdcModel::build_adc_table(const float* query) const {
  // 每个 query 只构造一次查表距离，图遍历时按 PQ code 快速累加。
  if (!enabled()) {
    return {};
  }
  std::vector<float> table(subspaces_ * centroids_, 0.0f);
  for (std::size_t m = 0; m < subspaces_; ++m) {
    const std::size_t begin = offsets_[m];
    const std::size_t end = offsets_[m + 1];
    for (std::size_t c = 0; c < centroids_; ++c) {
      const float* centroid =
          codebooks_.data() + (m * centroids_ + c) * dim_;
      float distance = 0.0f;
      for (std::size_t d = begin; d < end; ++d) {
        const float diff = query[d] - centroid[d];
        distance += diff * diff;
      }
      table[m * centroids_ + c] = distance;
    }
  }
  return table;
}

float PqAdcModel::adc_distance(std::uint32_t id,
                              const std::vector<float>& table) const {
  if (!enabled() || id * subspaces_ + subspaces_ > codes_.size() ||
      table.size() != subspaces_ * centroids_) {
    throw std::runtime_error("Invalid PQ ADC lookup");
  }
  float distance = 0.0f;
  for (std::size_t m = 0; m < subspaces_; ++m) {
    distance += table[m * centroids_ + codes_[id * subspaces_ + m]];
  }
  return distance;
}

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

NaiveDiskGraphIndex::NaiveDiskGraphIndex(const std::string& path)
    : path_(path), input_(path, std::ios::binary) {
  if (!input_) {
    throw std::runtime_error("Cannot open graph index: " + path);
  }

  char magic[8] = {};
  input_.read(magic, sizeof(magic));
  if (!input_ || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
    throw std::runtime_error("Invalid graph index magic: " + path);
  }

  const auto version = read_value<std::uint32_t>(input_);
  if (version != kVersion) {
    throw std::runtime_error("Unsupported graph index version");
  }

  metadata_.vector_count = read_value<std::uint64_t>(input_);
  metadata_.dim = read_value<std::uint32_t>(input_);
  metadata_.degree = read_value<std::uint32_t>(input_);
  metadata_.page_size = read_value<std::uint32_t>(input_);
  metadata_.records_offset = read_value<std::uint64_t>(input_);
  metadata_.directory_offset = 0;
  metadata_.page_count = metadata_.vector_count;
  metadata_.nodes_per_page = 1;

  if (metadata_.vector_count == 0 || metadata_.dim == 0 ||
      metadata_.degree == 0 || metadata_.page_size == 0) {
    throw std::runtime_error("Graph index metadata is invalid");
  }
  if (record_bytes(metadata_.dim, metadata_.degree) > metadata_.page_size) {
    throw std::runtime_error("Graph index record is larger than page size");
  }
  page_reader_ = std::make_unique<DiskPageReader>(path_, metadata_.page_size);
}

NaiveDiskGraphIndex::~NaiveDiskGraphIndex() = default;

void NaiveDiskGraphIndex::configure_io(const std::string& mode,
                                       std::size_t batch_size) {
  page_reader_->configure(mode, batch_size);
}

const DiskGraphIoStatus& NaiveDiskGraphIndex::io_status() const {
  return page_reader_->status();
}

NaiveDiskGraphIndex::DiskNode NaiveDiskGraphIndex::read_node(
    std::uint32_t id, DiskGraphSearchStats& stats) {
  if (id >= metadata_.vector_count) {
    throw std::runtime_error("Graph node id out of range");
  }

  const std::uint64_t offset =
      metadata_.records_offset +
      static_cast<std::uint64_t>(id) * metadata_.page_size;
  std::vector<char> page =
      page_reader_->read(offset, metadata_.page_size, stats);

  std::size_t cursor = 0;
  DiskNode node;
  node.id = get_bytes<std::uint32_t>(page, cursor);
  const auto degree = get_bytes<std::uint32_t>(page, cursor);
  if (node.id != id || degree > metadata_.degree) {
    throw std::runtime_error("Corrupted graph node page");
  }

  node.vector.resize(metadata_.dim);
  std::memcpy(node.vector.data(), page.data() + cursor,
              metadata_.dim * sizeof(float));
  cursor += metadata_.dim * sizeof(float);

  node.neighbors.reserve(degree);
  for (std::uint32_t i = 0; i < metadata_.degree; ++i) {
    const auto neighbor = get_bytes<std::uint32_t>(page, cursor);
    if (i < degree && neighbor != std::numeric_limits<std::uint32_t>::max()) {
      node.neighbors.push_back(neighbor);
    }
  }
  return node;
}

DiskGraphSearchResult NaiveDiskGraphIndex::search_one(
    const float* query, const DiskGraphSearchConfig& config) {
  if (config.top_k == 0 || config.search_width == 0 ||
      config.entry_count == 0) {
    throw std::runtime_error("Graph search top_k, search_width, and entry_count must be positive");
  }

  DiskGraphSearchResult output;
  std::unordered_set<std::uint32_t> visited;
  std::unordered_map<std::uint32_t, DiskNode> local_nodes;
  std::priority_queue<SearchResult, std::vector<SearchResult>, CloserFirst>
      candidates;
  std::priority_queue<SearchResult, std::vector<SearchResult>, WorseResultFirst>
      best;

  auto load_node = [&](std::uint32_t id) -> DiskNode& {
    auto found = local_nodes.find(id);
    if (found != local_nodes.end()) {
      return found->second;
    }
    DiskNode node = read_node(id, output.stats);
    ++output.stats.node_reads;
    auto inserted = local_nodes.emplace(id, std::move(node));
    return inserted.first->second;
  };

  std::vector<float> adc_table;
  if (config.adc_enable && config.pq_model != nullptr) {
    const auto adc_start = std::chrono::steady_clock::now();
    adc_table = config.pq_model->build_adc_table(query);
    const auto adc_end = std::chrono::steady_clock::now();
    output.stats.adc_table_build_us =
        std::chrono::duration<double, std::micro>(adc_end - adc_start).count();
  }
  auto query_distance = [&](std::uint32_t id, const DiskNode& node) {
    if (!adc_table.empty()) {
      return config.pq_model->adc_distance(id, adc_table);
    }
    return squared_l2(query, node.vector.data(), metadata_.dim);
  };

  const std::size_t effective_k =
      std::min<std::size_t>(config.top_k,
                            static_cast<std::size_t>(metadata_.vector_count));
  const std::size_t result_capacity =
      std::min<std::size_t>(
          std::max(effective_k, config.rerank_topk),
          static_cast<std::size_t>(metadata_.vector_count));

  auto add_best = [&](const SearchResult& result) {
    if (best.size() < result_capacity) {
      best.push(result);
    } else {
      const auto& worst = best.top();
      if (result.distance < worst.distance ||
          (result.distance == worst.distance && result.id < worst.id)) {
        best.pop();
        best.push(result);
      }
    }
  };

  auto should_stop = [&](const SearchResult& next) {
    if (!config.early_stop || best.size() < result_capacity ||
        output.stats.expanded < config.early_stop_min_expansions) {
      return false;
    }
    const auto& worst = best.top();
    return next.distance > worst.distance;
  };
  std::size_t stagnant_expansions = 0;
  double previous_worst_distance = std::numeric_limits<double>::infinity();
  auto update_adaptive_stop = [&]() {
    if (!config.adaptive_early_stop || best.size() < result_capacity) {
      return;
    }
    const double current = best.top().distance;
    const double improvement =
        std::isfinite(previous_worst_distance)
            ? (previous_worst_distance - current) /
                  std::max(1.0, std::abs(previous_worst_distance))
            : std::numeric_limits<double>::infinity();
    if (improvement > config.early_stop_eps) {
      stagnant_expansions = 0;
    } else {
      ++stagnant_expansions;
    }
    previous_worst_distance = current;
  };

  const std::vector<std::uint32_t> entries =
      config.seed_ids.empty()
          ? entry_points(metadata_.vector_count, config.entry_count)
          : config.seed_ids;

  for (const auto entry : entries) {
    if (entry >= metadata_.vector_count) {
      continue;
    }
    if (!visited.insert(entry).second) {
      continue;
    }
    const DiskNode& node = load_node(entry);
    const float distance = query_distance(entry, node);
    const SearchResult result{entry, distance};
    candidates.push(result);
    add_best(result);
  }

  while (!candidates.empty() && output.stats.expanded < config.search_width) {
    if (config.adaptive_early_stop &&
        output.stats.expanded >= config.min_expansions &&
        stagnant_expansions >= config.early_stop_patience) {
      break;
    }
    const SearchResult current = candidates.top();
    if (should_stop(current)) {
      break;
    }
    candidates.pop();
    const DiskNode& node = load_node(current.id);
    ++output.stats.expanded;

    for (const auto neighbor_id : node.neighbors) {
      if (neighbor_id >= metadata_.vector_count ||
          !visited.insert(neighbor_id).second) {
        continue;
      }
      const DiskNode& neighbor = load_node(neighbor_id);
      const float distance = query_distance(neighbor_id, neighbor);
      const SearchResult candidate{neighbor_id, distance};
      candidates.push(candidate);
      add_best(candidate);
    }
    update_adaptive_stop();
  }

  output.stats.visited = visited.size();
  output.topk = sorted_results(best);
  if (!adc_table.empty() && config.rerank_topk > 0) {
    for (auto& result : output.topk) {
      const auto found = local_nodes.find(result.id);
      if (found != local_nodes.end()) {
        result.distance =
            squared_l2(query, found->second.vector.data(), metadata_.dim);
      }
    }
    std::sort(output.topk.begin(), output.topk.end(),
              [](const SearchResult& lhs, const SearchResult& rhs) {
                return lhs.distance == rhs.distance ? lhs.id < rhs.id
                                                    : lhs.distance < rhs.distance;
              });
  }
  if (output.topk.size() > effective_k) {
    output.topk.resize(effective_k);
  }
  return output;
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
    const std::size_t begin =
        static_cast<std::size_t>(page_id) * nodes_per_page;
    const std::size_t end = std::min(begin + nodes_per_page, order.size());
    const auto node_count = static_cast<std::uint32_t>(end - begin);

    std::vector<char> page(config.page_size, 0);
    std::size_t offset = 0;
    put_bytes(page, offset, static_cast<std::uint32_t>(page_id));
    put_bytes(page, offset, node_count);

    for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {  // SoA: id 区。
      const std::uint32_t id =
          slot < node_count ? order[begin + slot]
                            : std::numeric_limits<std::uint32_t>::max();
      put_bytes(page, offset, id);
    }

    for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {  // SoA: degree 区。
      const std::uint32_t degree =
          slot < node_count
              ? static_cast<std::uint32_t>(graph[order[begin + slot]].size())
              : 0;
      put_bytes(page, offset, degree);
    }

    for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {  // SoA: vector 区。
      if (offset + base.dim * sizeof(float) > page.size()) {
        throw std::runtime_error("Packed vector block exceeds page size");
      }
      if (slot < node_count) {
        std::memcpy(page.data() + offset, base.row(order[begin + slot]),
                    base.dim * sizeof(float));
      }
      offset += base.dim * sizeof(float);
    }

    for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {  // SoA: neighbor 区。
      const std::uint32_t node_id =
          slot < node_count ? order[begin + slot]
                            : std::numeric_limits<std::uint32_t>::max();
      for (std::size_t n = 0; n < config.degree; ++n) {
        std::uint32_t neighbor = std::numeric_limits<std::uint32_t>::max();
        if (slot < node_count && n < graph[node_id].size()) {
          neighbor = graph[node_id][n];
        }
        put_bytes(page, offset, neighbor);
      }
    }

    output.write(page.data(), static_cast<std::streamsize>(page.size()));
    if (!output) {
      throw std::runtime_error("Failed while writing packed graph page");
    }
  }
}

PackedDiskGraphIndex::PackedDiskGraphIndex(const std::string& path)
    : path_(path), input_(path, std::ios::binary) {
  if (!input_) {
    throw std::runtime_error("Cannot open packed graph index: " + path);
  }

  char magic[8] = {};
  input_.read(magic, sizeof(magic));
  if (!input_ || std::memcmp(magic, kPackedMagic, sizeof(kPackedMagic)) != 0) {
    throw std::runtime_error("Invalid packed graph index magic: " + path);
  }

  const auto version = read_value<std::uint32_t>(input_);
  if (version != kPackedVersion) {
    throw std::runtime_error("Unsupported packed graph index version");
  }

  metadata_.vector_count = read_value<std::uint64_t>(input_);
  metadata_.dim = read_value<std::uint32_t>(input_);
  metadata_.degree = read_value<std::uint32_t>(input_);
  metadata_.page_size = read_value<std::uint32_t>(input_);
  metadata_.records_offset = read_value<std::uint64_t>(input_);
  metadata_.directory_offset = read_value<std::uint64_t>(input_);
  metadata_.page_count = read_value<std::uint64_t>(input_);
  metadata_.nodes_per_page = read_value<std::uint32_t>(input_);

  if (metadata_.vector_count == 0 || metadata_.dim == 0 ||
      metadata_.degree == 0 || metadata_.page_size == 0 ||
      metadata_.page_count == 0 || metadata_.nodes_per_page == 0) {
    throw std::runtime_error("Packed graph index metadata is invalid");
  }

  node_to_page_.resize(static_cast<std::size_t>(metadata_.vector_count));
  input_.clear();
  input_.seekg(static_cast<std::streamoff>(metadata_.directory_offset),
               std::ios::beg);
  input_.read(reinterpret_cast<char*>(node_to_page_.data()),
              static_cast<std::streamsize>(node_to_page_.size() *
                                           sizeof(std::uint32_t)));
  if (!input_) {
    throw std::runtime_error("Failed to read packed graph directory");
  }
  page_reader_ = std::make_unique<DiskPageReader>(path_, metadata_.page_size);
}

PackedDiskGraphIndex::~PackedDiskGraphIndex() = default;

void PackedDiskGraphIndex::configure_cache(const std::string& policy,
                                           std::size_t capacity_pages) {
  if (policy != "none" && policy != "lru" && policy != "agent") {
    throw std::runtime_error("Cache policy must be none, lru, or agent");
  }
  if (policy != cache_policy_ || capacity_pages != cache_capacity_pages_) {
    page_cache_.clear();
    cache_clock_ = 0;
  }
  cache_policy_ = policy;
  cache_capacity_pages_ = capacity_pages;
}

void PackedDiskGraphIndex::configure_io(const std::string& mode,
                                        std::size_t batch_size) {
  page_reader_->configure(mode, batch_size);
}

const DiskGraphIoStatus& PackedDiskGraphIndex::io_status() const {
  return page_reader_->status();
}

PackedDiskGraphIndex::DecodedPage PackedDiskGraphIndex::read_page(
    std::uint32_t page_id, DiskGraphSearchStats& stats) {
  // 解码顺序必须与 PackedDiskGraphBuilder 写入的四个 SoA 区域严格一致。
  if (page_id >= metadata_.page_count) {
    throw std::runtime_error("Packed page id out of range");
  }

  const std::uint64_t offset =
      metadata_.records_offset +
      static_cast<std::uint64_t>(page_id) * metadata_.page_size;
  std::vector<char> page = page_reader_->read(offset, metadata_.page_size,
                                               stats);
  return decode_page(page_id, page);
}

PackedDiskGraphIndex::DecodedPage PackedDiskGraphIndex::decode_page(
    std::uint32_t page_id, const std::vector<char>& page) const {
  std::size_t cursor = 0;
  DecodedPage decoded;
  decoded.page_id = get_bytes<std::uint32_t>(page, cursor);
  const auto node_count = get_bytes<std::uint32_t>(page, cursor);
  if (decoded.page_id != page_id || node_count > metadata_.nodes_per_page) {
    throw std::runtime_error("Corrupted packed graph page header");
  }

  std::vector<std::uint32_t> ids(metadata_.nodes_per_page);
  std::vector<std::uint32_t> degrees(metadata_.nodes_per_page);
  for (std::uint32_t i = 0; i < metadata_.nodes_per_page; ++i) {
    ids[i] = get_bytes<std::uint32_t>(page, cursor);
  }
  for (std::uint32_t i = 0; i < metadata_.nodes_per_page; ++i) {
    degrees[i] = get_bytes<std::uint32_t>(page, cursor);
  }

  decoded.nodes.reserve(node_count);
  for (std::uint32_t slot = 0; slot < metadata_.nodes_per_page; ++slot) {
    if (cursor + metadata_.dim * sizeof(float) > page.size()) {
      throw std::runtime_error("Packed vector block is truncated");
    }
    if (slot < node_count) {
      DiskNode node;
      node.id = ids[slot];
      node.vector.resize(metadata_.dim);
      std::memcpy(node.vector.data(), page.data() + cursor,
                  metadata_.dim * sizeof(float));
      decoded.nodes.push_back(std::move(node));
    }
    cursor += metadata_.dim * sizeof(float);
  }

  for (std::uint32_t slot = 0; slot < metadata_.nodes_per_page; ++slot) {
    std::vector<std::uint32_t> neighbors;
    if (slot < node_count) {
      neighbors.reserve(degrees[slot]);
    }
    for (std::uint32_t n = 0; n < metadata_.degree; ++n) {
      const auto neighbor = get_bytes<std::uint32_t>(page, cursor);
      if (slot < node_count && n < degrees[slot] &&
          neighbor != std::numeric_limits<std::uint32_t>::max()) {
        neighbors.push_back(neighbor);
      }
    }
    if (slot < node_count) {
      decoded.nodes[slot].neighbors = std::move(neighbors);
    }
  }

  return decoded;
}

bool PackedDiskGraphIndex::cache_enabled() const {
  return cache_policy_ != "none" && cache_capacity_pages_ > 0;
}

const PackedDiskGraphIndex::DecodedPage*
PackedDiskGraphIndex::lookup_cached_page(std::uint32_t page_id,
                                         DiskGraphSearchStats& stats) {
  if (!cache_enabled()) {
    return nullptr;
  }

  auto found = page_cache_.find(page_id);
  if (found == page_cache_.end()) {
    return nullptr;
  }

  ++stats.page_cache_hits;
  found->second.last_access = ++cache_clock_;
  ++found->second.frequency;
  return &found->second.page;
}

const PackedDiskGraphIndex::DecodedPage&
PackedDiskGraphIndex::store_cached_page(DecodedPage page) {
  if (!cache_enabled()) {
    scratch_page_ = std::move(page);
    return scratch_page_;
  }

  const std::uint32_t page_id = page.page_id;
  auto found = page_cache_.find(page_id);
  if (found != page_cache_.end()) {
    found->second.page = std::move(page);
    found->second.last_access = ++cache_clock_;
    ++found->second.frequency;
    return found->second.page;
  }

  if (page_cache_.size() >= cache_capacity_pages_) {
    evict_one_page();
  }

  CacheEntry entry;
  entry.page = std::move(page);
  entry.last_access = ++cache_clock_;
  entry.frequency = 1;
  auto inserted = page_cache_.emplace(page_id, std::move(entry));
  return inserted.first->second.page;
}

double PackedDiskGraphIndex::cache_score(const CacheEntry& entry) const {
  if (cache_policy_ == "lru") {
    return static_cast<double>(entry.last_access);
  }

  const double frequency = static_cast<double>(entry.frequency);
  const double recency = static_cast<double>(entry.last_access);
  const double density = static_cast<double>(entry.page.nodes.size());
  return frequency * 1000.0 + recency + density * 0.01;  // Agent 策略优先长期热点。
}

void PackedDiskGraphIndex::evict_one_page() {
  if (page_cache_.empty()) {
    return;
  }

  auto victim = page_cache_.begin();
  double victim_score = cache_score(victim->second);
  for (auto it = page_cache_.begin(); it != page_cache_.end(); ++it) {
    const double score = cache_score(it->second);
    if (score < victim_score ||
        (score == victim_score && it->first < victim->first)) {
      victim = it;
      victim_score = score;
    }
  }
  page_cache_.erase(victim);
}

const PackedDiskGraphIndex::DecodedPage& PackedDiskGraphIndex::load_page(
    std::uint32_t page_id, DiskGraphSearchStats& stats) {
  ++stats.page_requests;

  if (const DecodedPage* cached = lookup_cached_page(page_id, stats)) {
    return *cached;
  }

  if (cache_policy_ != "none" && cache_capacity_pages_ > 0) {
    auto found = page_cache_.find(page_id);
    if (found != page_cache_.end()) {
      ++stats.page_cache_hits;  // 跨 query 复用已解码页。
      found->second.last_access = ++cache_clock_;
      ++found->second.frequency;
      return found->second.page;
    }
  }

  ++stats.page_cache_misses;
  ++stats.node_reads;
  DecodedPage page = read_page(page_id, stats);

  if (cache_policy_ == "none" || cache_capacity_pages_ == 0) {
    scratch_page_ = std::move(page);
    return scratch_page_;
  }

  if (page_cache_.size() >= cache_capacity_pages_) {
    evict_one_page();
  }

  CacheEntry entry;
  entry.page = std::move(page);
  entry.last_access = ++cache_clock_;
  entry.frequency = 1;
  auto inserted = page_cache_.emplace(page_id, std::move(entry));
  return inserted.first->second.page;
}

DiskGraphSearchResult PackedDiskGraphIndex::search_one(
    const float* query, const DiskGraphSearchConfig& config) {
  if (config.top_k == 0 || config.search_width == 0 ||
      config.entry_count == 0) {
    throw std::runtime_error("Packed graph search top_k, search_width, and entry_count must be positive");
  }

  DiskGraphSearchResult output;
  std::unordered_set<std::uint32_t> visited;
  std::unordered_map<std::uint32_t, DiskNode> local_nodes;  // 单次查询内避免重复解码节点。
  std::priority_queue<SearchResult, std::vector<SearchResult>, CloserFirst>
      candidates;
  std::priority_queue<SearchResult, std::vector<SearchResult>, WorseResultFirst>
      best;

  const bool async_prefetch = page_reader_->async_enabled() &&
                              page_reader_->max_pending_reads() > 0;
  const std::size_t prefetch_budget_pages =
      async_prefetch ? page_reader_->max_pending_reads() : 0;
  std::unordered_map<std::uint32_t, DecodedPage> ready_prefetch_pages;
  std::unordered_map<std::uint64_t, std::uint32_t> token_to_page;
  std::unordered_set<std::uint32_t> pending_pages;
  std::unordered_set<std::uint32_t> completed_prefetch_pages;

  auto async_page_footprint = [&]() {
    return pending_pages.size() + ready_prefetch_pages.size();
  };
  auto update_pending_peak = [&]() {
    output.stats.io_pending_pages_peak = std::max(
        output.stats.io_pending_pages_peak, async_page_footprint());
  };
  auto page_in_cache = [&](std::uint32_t page_id) {
    return cache_enabled() && page_cache_.find(page_id) != page_cache_.end();
  };
  auto page_offset = [&](std::uint32_t page_id) {
    return metadata_.records_offset +
           static_cast<std::uint64_t>(page_id) * metadata_.page_size;
  };
  auto materialize_page = [&](const DecodedPage& page) {
    for (const auto& node : page.nodes) {
      local_nodes.emplace(node.id, node);
    }
  };

  auto harvest_prefetch = [&](bool wait) {
    if (!async_prefetch) {
      return false;
    }
    page_reader_->submit_async_reads(output.stats);

    bool harvested = false;
    DiskPageReader::CompletedRead completed;
    while (page_reader_->reap_async_read(wait && !harvested, completed,
                                         output.stats)) {
      harvested = true;
      const auto token_found = token_to_page.find(completed.token);
      if (token_found == token_to_page.end()) {
        throw std::runtime_error("Packed graph async page token is unknown");
      }
      const std::uint32_t page_id = token_found->second;
      token_to_page.erase(token_found);
      pending_pages.erase(page_id);

      DecodedPage decoded = decode_page(page_id, completed.data);
      completed_prefetch_pages.insert(page_id);
      if (cache_enabled()) {
        (void)store_cached_page(std::move(decoded));
      } else if (ready_prefetch_pages.size() < prefetch_budget_pages) {
        ready_prefetch_pages.emplace(page_id, std::move(decoded));
      }
      update_pending_peak();
      completed = DiskPageReader::CompletedRead{};
    }
    return harvested;
  };

  auto wait_for_prefetch_page = [&](std::uint32_t page_id) {
    while (pending_pages.find(page_id) != pending_pages.end()) {
      ++output.stats.io_prefetch_waits;
      if (!harvest_prefetch(true)) {
        throw std::runtime_error("Failed to wait for packed graph prefetch");
      }
    }
  };

  auto prefetch_page = [&](std::uint32_t page_id) {
    if (!async_prefetch || page_id >= metadata_.page_count ||
        page_in_cache(page_id) ||
        ready_prefetch_pages.find(page_id) != ready_prefetch_pages.end() ||
        pending_pages.find(page_id) != pending_pages.end()) {
      return;
    }

    if (async_page_footprint() >= prefetch_budget_pages) {
      (void)harvest_prefetch(false);
    }
    if (async_page_footprint() >= prefetch_budget_pages) {
      return;
    }

    const std::uint64_t token = page_reader_->start_async_read(
        page_offset(page_id), metadata_.page_size, output.stats);
    token_to_page[token] = page_id;
    pending_pages.insert(page_id);
    ++output.stats.node_reads;
    ++output.stats.io_prefetches;
    update_pending_peak();
  };

  auto submit_prefetches = [&]() {
    if (async_prefetch) {
      page_reader_->submit_async_reads(output.stats);
      (void)harvest_prefetch(false);
    }
  };

  auto prefetch_ids = [&](const std::vector<std::uint32_t>& ids) {
    if (!async_prefetch) {
      return;
    }
    for (const auto id : ids) {
      if (id < metadata_.vector_count &&
          visited.find(id) == visited.end() &&
          local_nodes.find(id) == local_nodes.end()) {
        prefetch_page(node_to_page_[id]);
      }
    }
    submit_prefetches();
  };

  auto prefetch_frontier = [&]() {
    if (!async_prefetch || candidates.empty()) {
      return;
    }

    auto frontier = candidates;
    while (!frontier.empty() &&
           async_page_footprint() < prefetch_budget_pages) {
      const auto next = frontier.top();
      frontier.pop();
      if (local_nodes.find(next.id) == local_nodes.end()) {
        prefetch_page(node_to_page_[next.id]);
      }
    }
    submit_prefetches();
  };

  auto load_node = [&](std::uint32_t id) -> DiskNode& {
    auto node_found = local_nodes.find(id);
    if (node_found != local_nodes.end()) {
      return node_found->second;
    }

    {
      const std::uint32_t page_id = node_to_page_[id];
      ++output.stats.page_requests;

      if (const DecodedPage* cached =
              lookup_cached_page(page_id, output.stats)) {
        if (completed_prefetch_pages.erase(page_id) > 0) {
          ++output.stats.io_prefetch_hits;
        }
        materialize_page(*cached);
      } else {
        auto ready = ready_prefetch_pages.find(page_id);
        if (ready != ready_prefetch_pages.end()) {
          ++output.stats.page_cache_hits;
          ++output.stats.io_prefetch_hits;
          materialize_page(ready->second);
          ready_prefetch_pages.erase(ready);
        } else {
          if (pending_pages.find(page_id) == pending_pages.end() &&
              async_prefetch) {
            while (async_page_footprint() >= prefetch_budget_pages &&
                   !pending_pages.empty()) {
              (void)harvest_prefetch(true);
            }
            prefetch_page(page_id);
            submit_prefetches();
          }

          if (pending_pages.find(page_id) != pending_pages.end()) {
            wait_for_prefetch_page(page_id);
          }

          if (const DecodedPage* prefetched =
                  lookup_cached_page(page_id, output.stats)) {
            if (completed_prefetch_pages.erase(page_id) > 0) {
              ++output.stats.io_prefetch_hits;
            }
            materialize_page(*prefetched);
          } else {
            ready = ready_prefetch_pages.find(page_id);
            if (ready != ready_prefetch_pages.end()) {
              ++output.stats.page_cache_hits;
              ++output.stats.io_prefetch_hits;
              materialize_page(ready->second);
              ready_prefetch_pages.erase(ready);
            } else {
              while (async_prefetch && !pending_pages.empty()) {
                (void)harvest_prefetch(true);
              }
              ++output.stats.page_cache_misses;
              ++output.stats.node_reads;
              DecodedPage page = read_page(page_id, output.stats);
              materialize_page(page);
            }
          }
        }
      }

      node_found = local_nodes.find(id);
      if (node_found == local_nodes.end()) {
        throw std::runtime_error("Node is missing from packed page");
      }
      return node_found->second;
    }

    const std::uint32_t page_id = node_to_page_[id];  // 一次读页会顺带缓存页内其他节点。
    const DecodedPage& page = load_page(page_id, output.stats);
    for (const auto& node : page.nodes) {
      local_nodes.emplace(node.id, node);
    }

    node_found = local_nodes.find(id);
    if (node_found == local_nodes.end()) {
      throw std::runtime_error("Node is missing from packed page");
    }
    return node_found->second;
  };

  std::vector<float> adc_table;  // 留空时使用原始向量距离。
  if (config.adc_enable && config.pq_model != nullptr) {
    const auto adc_start = std::chrono::steady_clock::now();
    adc_table = config.pq_model->build_adc_table(query);
    const auto adc_end = std::chrono::steady_clock::now();
    output.stats.adc_table_build_us =
        std::chrono::duration<double, std::micro>(adc_end - adc_start).count();
  }
  auto query_distance = [&](std::uint32_t id, const DiskNode& node) {
    if (!adc_table.empty()) {
      return config.pq_model->adc_distance(id, adc_table);
    }
    return squared_l2(query, node.vector.data(), metadata_.dim);
  };

  const std::size_t effective_k =
      std::min<std::size_t>(config.top_k,
                            static_cast<std::size_t>(metadata_.vector_count));
  const std::size_t result_capacity =
      std::min<std::size_t>(
          std::max(effective_k, config.rerank_topk),
          static_cast<std::size_t>(metadata_.vector_count));

  auto add_best = [&](const SearchResult& result) {
    if (best.size() < result_capacity) {
      best.push(result);
    } else {
      const auto& worst = best.top();
      if (result.distance < worst.distance ||
          (result.distance == worst.distance && result.id < worst.id)) {
        best.pop();
        best.push(result);
      }
    }
  };

  auto should_stop = [&](const SearchResult& next) {  // frontier 已不可能改善 Top-K。
    if (!config.early_stop || best.size() < result_capacity ||
        output.stats.expanded < config.early_stop_min_expansions) {
      return false;
    }
    const auto& worst = best.top();
    return next.distance > worst.distance;
  };
  std::size_t stagnant_expansions = 0;
  double previous_worst_distance = std::numeric_limits<double>::infinity();
  auto update_adaptive_stop = [&]() {
    if (!config.adaptive_early_stop || best.size() < result_capacity) {
      return;
    }
    const double current = best.top().distance;
    const double improvement =
        std::isfinite(previous_worst_distance)
            ? (previous_worst_distance - current) /
                  std::max(1.0, std::abs(previous_worst_distance))
            : std::numeric_limits<double>::infinity();
    if (improvement > config.early_stop_eps) {
      stagnant_expansions = 0;
    } else {
      ++stagnant_expansions;
    }
    previous_worst_distance = current;
  };

  const std::vector<std::uint32_t> entries =
      config.seed_ids.empty()
          ? entry_points(metadata_.vector_count, config.entry_count)
          : config.seed_ids;

  prefetch_ids(entries);
  for (const auto entry : entries) {
    if (entry >= metadata_.vector_count) {
      continue;
    }
    if (!visited.insert(entry).second) {
      continue;
    }
    const DiskNode& node = load_node(entry);
    const float distance = query_distance(entry, node);
    const SearchResult result{entry, distance};
    candidates.push(result);
    add_best(result);
  }

  while (!candidates.empty() && output.stats.expanded < config.search_width) {
    prefetch_frontier();
    if (config.adaptive_early_stop &&
        output.stats.expanded >= config.min_expansions &&
        stagnant_expansions >= config.early_stop_patience) {
      break;
    }
    const SearchResult current = candidates.top();
    if (should_stop(current)) {
      break;
    }
    candidates.pop();
    const DiskNode& node = load_node(current.id);
    ++output.stats.expanded;

    prefetch_ids(node.neighbors);
    for (const auto neighbor_id : node.neighbors) {
      if (neighbor_id >= metadata_.vector_count ||
          !visited.insert(neighbor_id).second) {
        continue;
      }
      const DiskNode& neighbor = load_node(neighbor_id);
      const float distance = query_distance(neighbor_id, neighbor);
      const SearchResult candidate{neighbor_id, distance};
      candidates.push(candidate);
      add_best(candidate);
    }
    update_adaptive_stop();
  }

  while (async_prefetch && !pending_pages.empty()) {
    (void)harvest_prefetch(true);
  }

  output.stats.visited = visited.size();
  output.topk = sorted_results(best);
  if (!adc_table.empty() && config.rerank_topk > 0) {  // 用原始向量恢复候选排序精度。
    for (auto& result : output.topk) {
      const auto found = local_nodes.find(result.id);
      if (found != local_nodes.end()) {
        result.distance =
            squared_l2(query, found->second.vector.data(), metadata_.dim);
      }
    }
    std::sort(output.topk.begin(), output.topk.end(),
              [](const SearchResult& lhs, const SearchResult& rhs) {
                return lhs.distance == rhs.distance ? lhs.id < rhs.id
                                                    : lhs.distance < rhs.distance;
              });
  }
  if (output.topk.size() > effective_k) {
    output.topk.resize(effective_k);
  }
  return output;
}

}  // namespace agentmem
