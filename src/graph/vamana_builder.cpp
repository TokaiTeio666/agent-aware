#include "graph/vamana_builder.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "core/simd_distance.h"

namespace agent_aware {
namespace {

// VamanaBuilder 在磁盘 builder 序列化前先构造内存图；deleted id 在整个构建过程中都视为不存在。
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
  medoid_cache_ = output.medoid;
  output.adjacency = initial_graph(base, deleted);
  output.stats.medoid = output.medoid;

  const auto order = build_order(base.size());
  // 随机插入顺序避免原始 id 顺序带来的偏置，同时固定 seed 下保持确定性。
  //
  // 第一遍：先在随机的环+随机初始边上依次插入，此时 alpha 从 1.0 线性升到
  // 配置值（Vamana 的做法）。固定 alpha 时，早期插入的节点面对的是一个几乎
  // 空的图，候选池里全是"已插入的远处点"，宽松的 alpha 把它们全部留下；
  // 到插入后期又不断被反向边挤占，最终每个节点的出边都不在本地点附近。
  // 实测邻居召回（出边中有几个是该点自己的最近 10 个）随规模单调下滑：
  // 20k 0.975 → 80k 0.703 → 160k 0.472 → 1M 0.05。
  //
  // 第二遍：图已完整，alpha 恢复到配置值，对每个节点在完整图上重新
  // "搜索 + robust_prune"，把第一遍留下的不完整邻域彻底重建。
  for (int pass = 0; pass < 2; ++pass) {
    const double alpha_target = config_.alpha;
    for (std::size_t index = 0; index < order.size(); ++index) {
      const auto node_id = order[index];
      if (is_deleted(deleted, node_id)) {
        output.adjacency[node_id].clear();
        continue;
      }
      if (pass == 0) {
        // 1.0 → alpha_target 线性过渡；单点规模下直接给最终值。
        const double fraction =
            order.size() <= 1
                ? 1.0
                : static_cast<double>(index) /
                      static_cast<double>(order.size() - 1);
        active_alpha_ = 1.0 + (alpha_target - 1.0) * fraction;
        insert(base, output.adjacency, node_id, output.medoid, &output.stats);
        continue;
      }
      active_alpha_ = alpha_target;
      refine_node(base, output.adjacency, node_id, deleted);
    }
  }
  active_alpha_ = config_.alpha;

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
    entry_id = medoid_cache_;
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
  // 单入口的条件贪心会让整条搜索被第一个局部极小决定：medoid 对查询往往很远，
  // 第一步一旦走错就再也回不来（实测从 medoid 出发的召回率是 0，而从真值出发是 1）。
  // 因此把若干固定锚点同时作为起点，让 frontier 一开始就覆盖整个空间，
  // 再由搜索宽度收敛到查询附近。
  for (const auto seed : start_candidates(base, excluded_id, deleted)) {
    push(seed);
  }
  if (frontier.empty()) {
    for (std::size_t i = 0; i < graph.size(); ++i) {
      push(static_cast<std::uint32_t>(i));
      if (!frontier.empty()) {
        break;
      }
    }
  }

  while (!frontier.empty() && explored.size() < config_.search_width) {
    // 构建阶段 search 在当前图上贪心扩展，返回给 robust prune 的候选池，而不是最终查询结果。
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
  // 删除掩码按需构造（见 insert 中的说明）。
  const std::vector<char> deleted =
      config_.deleted_ids.empty() ? std::vector<char>()
                                  : deleted_mask(base.size());
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
  const double alpha_sq = active_alpha_ * active_alpha_;
  std::vector<Candidate> remaining;
  remaining.reserve(pool.size());
  while (!pool.empty() && output.size() < effective_degree) {
    const Candidate picked = pool.front();
    output.push_back(picked.id);

    // robust prune 只保留不能被更近已选邻居解释的候选；alpha 越大，裁剪越宽松。
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

std::vector<std::uint32_t> VamanaBuilder::start_candidates(
    const VectorSet& base, std::uint32_t excluded_id,
    const std::vector<char>& deleted) const {
  // 构造搜索的起点集合：入口节点之外再加一批均匀切片，保证 frontier 覆盖整个
  // 向量空间，而不是被入口附近的局部极小锁死。
  std::vector<std::uint32_t> starts;
  const std::size_t count = base.size();
  const std::size_t stride =
      std::max<std::size_t>(1, count / std::max<std::size_t>(1, config_.start_count));
  starts.reserve(config_.start_count);
  for (std::size_t i = 0; starts.size() < config_.start_count && i < count;
       i += stride) {
    starts.push_back(static_cast<std::uint32_t>(i));
  }
  return unique_ids(std::move(starts), excluded_id, deleted);
}

std::vector<std::uint32_t> VamanaBuilder::beacon_candidates(
    const VectorSet& base, std::uint32_t node_id,
    const std::vector<char>& deleted) const {
  // 贪心构建只从 medoid 出发搜索邻居，图的可达性因此取决于"从 medoid 能不能
  // 走到每个区域"。20 万节点以内通常没问题，到 1M 就退化成: 从入口只能到达
  // 0.09% 的节点，召回率与搜索宽度完全无关。经典的修复是远点采样/beacon 边：
  // 给每个节点若干"锚点"作为候选，robust prune 自行决定保留哪些。
  // 锚点集合取 medoid + 固定间隔的均匀切片 + 一小撮固定种子的随机点。
  const std::size_t count = base.size();
  std::vector<std::uint32_t> anchors;
  if (count <= 1) {
    return anchors;
  }
  const std::size_t beacon_count =
      std::min<std::size_t>(config_.beacon_count, count);
  anchors.reserve(beacon_count + 1);
  // medoid 对整个 base 是常量：缓存它，否则每个节点都重算一次 O(n·dim) 的
  // medoid，1M 构建会从几分钟变成小时级。medoid 已由 build() 算好放在
  // medoid_cache_ 里。
  anchors.push_back(medoid_cache_);
  if (beacon_count > 1) {
    const std::size_t stride = std::max<std::size_t>(1, count / beacon_count);
    for (std::size_t i = 0; anchors.size() < beacon_count && i < count; i += stride) {
      anchors.push_back(static_cast<std::uint32_t>(i));
    }
  }
  std::mt19937 rng(config_.seed ^ 0x5bd1e995u);
  std::uniform_int_distribution<std::uint32_t> pick(0, static_cast<std::uint32_t>(count - 1));
  const std::size_t target = beacon_count + beacon_count / 4;
  for (std::size_t guard = 0; anchors.size() < target && guard < target * 4; ++guard) {
    anchors.push_back(pick(rng));
  }
  return unique_ids(std::move(anchors), node_id, deleted);
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
  // 只有真的存在删除列表时才构造掩码：它在每个节点都要建一次，
  // 1M 规模下即使列表为空也会白白分配并清零 1 MB。
  const std::vector<char> deleted =
      config_.deleted_ids.empty() ? std::vector<char>()
                                  : deleted_mask(base.size());
  if (is_deleted(deleted, node_id)) {
    graph[node_id].clear();
    return;
  }

  std::vector<std::uint32_t> candidates = search_for_construction(
      base, graph, base.row(node_id), entry_id, node_id);
  candidates.insert(candidates.end(), graph[node_id].begin(),
                    graph[node_id].end());
  const auto beacons = beacon_candidates(base, node_id, deleted);
  candidates.insert(candidates.end(), beacons.begin(), beacons.end());
  candidates = unique_ids(std::move(candidates), node_id, deleted);
  if (stats != nullptr) {
    stats->candidate_counts.push_back(candidates.size());
    ++stats->prune_count;
  }

  graph[node_id] = robust_prune(base, node_id, candidates);
  if (!config_.add_reverse_edges) {
    return;
  }
  // 反向边改善可达性；目标邻接表超载时用同样的 robust-prune 规则保持度数有界。
  const std::vector<std::uint32_t> neighbors = graph[node_id];
  for (const auto target : neighbors) {
    add_reverse_edge(base, graph, node_id, target, stats);
  }
}

void VamanaBuilder::refine_node(
    const VectorSet& base,
    std::vector<std::vector<std::uint32_t>>& graph,
    std::uint32_t node_id, const std::vector<char>& deleted) const {
  // 第二遍：图已完整，从 medoid 重新搜索并裁剪，再把新出边的反向边补回去。
  std::vector<std::uint32_t> candidates = search_for_construction(
      base, graph, base.row(node_id), medoid_cache_, node_id);
  candidates.insert(candidates.end(), graph[node_id].begin(),
                    graph[node_id].end());
  candidates = unique_ids(std::move(candidates), node_id, deleted);
  graph[node_id] = robust_prune(base, node_id, candidates);
  if (!config_.add_reverse_edges) {
    return;
  }
  // 逆边必须同样受度数约束：这里如果直接 push_back，节点度数会突破
  // max_degree（实测 12 万点时 88% 的节点最多涨到 119 条），而 V3 页格式
  // 只按 metadata.degree 编码每条邻接表的前 degree 条 —— 多出来的边在
  // 序列化时被静默丢弃，内存里连通、落盘后断裂。
  for (const auto target : graph[node_id]) {
    add_reverse_edge(base, graph, node_id, target, nullptr);
  }
}

std::uint32_t VamanaBuilder::select_entry_node(
    const std::vector<std::vector<std::uint32_t>>& graph,
    const std::vector<char>& deleted) const {
  // 便宜的 PageRank（无向化、固定迭代次数）：入度本身会被"好当邻居的点"主导，
  // 而 PageRank 把"被中心性高的点指向"也算进去，选出的入口更靠近图的骨架。
  // 1M 节点 × 32 边 × 20 轮在构建时间面前可以忽略。
  const std::size_t count = graph.size();
  if (count == 0) {
    return 0;
  }
  constexpr int kIterations = 20;
  constexpr double kDamping = 0.85;
  const double teleport = (1.0 - kDamping) / static_cast<double>(count);
  std::vector<double> rank(count, 1.0 / static_cast<double>(count));
  std::vector<double> next(count, 0.0);
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    std::fill(next.begin(), next.end(), teleport);
    for (std::size_t from = 0; from < count; ++from) {
      const auto& neighbors = graph[from];
      if (neighbors.empty() || is_deleted(deleted, from)) {
        continue;
      }
      const double share =
          kDamping * rank[from] / static_cast<double>(neighbors.size());
      for (const auto to : neighbors) {
        if (to < count) {
          next[to] += share;
        }
      }
    }
    rank.swap(next);
  }
  std::uint32_t best = 0;
  double best_rank = -1.0;
  for (std::size_t i = 0; i < count; ++i) {
    if (is_deleted(deleted, i)) {
      continue;
    }
    if (rank[i] > best_rank) {
      best_rank = rank[i];
      best = static_cast<std::uint32_t>(i);
    }
  }
  return best;
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
  if (contains_id(graph[target], source)) {
    return;
  }
  // 删除掩码按需构造：反向边是热路径，每个边都会走到这里。
  if (!config_.deleted_ids.empty()) {
    const auto deleted = deleted_mask(base.size());
    if (is_deleted(deleted, source) || is_deleted(deleted, target)) {
      return;
    }
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
