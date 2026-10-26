// 索引完整性核对：通过图索引自己的页读取路径取回节点，逐分量与底库原始向量比对。
// "id 对、距离错"只有两种解释 —— 要么页里存的向量不是这个 id 的向量，
// 要么拿到的向量不是页里存的那份。这个测试直接把两者分开。
//
// 用法：index_integrity <index> <base.fvecs> [samples]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/brute_force.h"
#include "core/query_page_session.h"
#include "data/dataset.h"
#include "graph/disk_graph_index.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <index> <base.fvecs> [samples]\n", argv[0]);
    return 1;
  }
  const std::size_t samples =
      argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 200;
  const auto base = agent_aware::load_fvecs(argv[2]);
  agent_aware::PackedDiskGraphIndex index(argv[1]);
  const auto& meta = index.metadata();
  std::printf("vector_count=%zu base=%zu dim=%u nodes_per_page=%u degree=%u\n",
              static_cast<std::size_t>(meta.vector_count), base.size(), meta.dim,
              meta.nodes_per_page, meta.degree);
  if (base.size() != meta.vector_count) {
    std::printf("error: base count disagrees with index\n");
    return 1;
  }

  agent_aware::DiskGraphSearchStats stats;
  agent_aware::DiskGraphSearchConfig config;
  agent_aware::QueryPageSession session(index, config, stats);

  // 均匀取样覆盖整张索引，外加前后各 20 个边界节点。
  std::vector<std::uint32_t> ids;
  for (std::size_t i = 0; i < samples && i < base.size(); ++i) {
    ids.push_back(static_cast<std::uint32_t>(
        (i * (base.size() / (samples + 1))) % base.size()));
  }
  for (std::uint32_t i = 0; i < 20 && i < base.size(); ++i) {
    ids.push_back(i);
    ids.push_back(static_cast<std::uint32_t>(base.size() - 1 - i));
  }

  std::size_t missing = 0;
  std::size_t mismatched = 0;
  std::size_t checked = 0;
  float worst = 0.0f;
  std::uint32_t worst_id = 0;
  for (const auto id : ids) {
    const auto& node = session.get_node(id, false);
    const float* stored = node.vector.data();
    const float* truth = base.row(id);
    float diff = 0.0f;
    for (std::uint32_t d = 0; d < meta.dim; ++d) {
      diff = std::max(diff, std::fabs(stored[d] - truth[d]));
    }
    ++checked;
    if (node.id != id) {
      ++missing;
    } else if (diff > 1e-5f) {
      ++mismatched;
    }
    if (diff > worst) {
      worst = diff;
      worst_id = id;
    }
    if (checked <= 5) {
      std::printf(
          "node %u: stored[0..3]=%.4f %.4f %.4f %.4f | base[0..3]=%.4f %.4f "
          "%.4f %.4f | max|diff|=%.6f\n",
          id, stored[0], stored[1], stored[2], stored[3], truth[0], truth[1],
          truth[2], truth[3], diff);
    }
  }
  std::printf("checked=%zu id_mismatch=%zu vector_mismatch=%zu worst|diff|=%.6f "
              "(node %u)\n",
              checked, missing, mismatched, worst, worst_id);
  if (missing != 0 || mismatched != 0) {
    return 1;
  }

  // 连通性：从入口点沿邻居能不能走到全部节点。Vamana 的贪心搜索只能沿边前进，
  // 走不到的节点在查询时永远召不回，无论宽度多大。
  const std::uint32_t entry = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 0;
  std::vector<char> reached(meta.vector_count, 0);
  std::vector<std::uint32_t> frontier{entry};
  reached[entry] = 0;
  std::size_t reachable = 0;
  std::size_t dead_ends = 0;
  std::size_t self_loops = 0;
  while (!frontier.empty()) {
    std::vector<std::uint32_t> next;
    for (const auto id : frontier) {
      if (reached[id]) {
        continue;
      }
      reached[id] = 1;
      ++reachable;
      const auto& node = session.get_node(id, false);
      if (node.neighbors.empty()) {
        ++dead_ends;
      }
      for (const auto neighbor : node.neighbors) {
        if (neighbor == id) {
          ++self_loops;
        }
        if (neighbor < reached.size() && !reached[neighbor]) {
          next.push_back(neighbor);
        }
      }
    }
    frontier.swap(next);
  }
  // 局部性：好的近邻图里，一个节点的邻居应该就是它附近的点。统计邻居与节点
  // 的距离在"全库随机取 32 个点"的分布里排到第几百分位；100 表示邻居和随机点
  // 没区别（纯随机图），接近 0 表示邻居确实是近邻。
  {
    const std::size_t probe_nodes = 20;
    double sum_pct = 0.0;
    for (std::size_t i = 0; i < probe_nodes; ++i) {
      const auto id = static_cast<std::uint32_t>((i * 9973) % base.size());
      const auto& node = session.get_node(id, false);
      if (node.neighbors.empty()) {
        continue;
      }
      std::vector<float> all;
      all.reserve(base.size());
      for (std::size_t j = 0; j < base.size(); ++j) {
        all.push_back(agent_aware::squared_l2(base.row(id), base.row(j), base.dim));
      }
      double pct = 0.0;
      for (const auto n : node.neighbors) {
        const float d = agent_aware::squared_l2(base.row(id), base.row(n), base.dim);
        const auto rank = std::count_if(all.begin(), all.end(),
                                        [d](float x) { return x < d; });
        pct += static_cast<double>(rank) / static_cast<double>(all.size());
      }
      sum_pct += pct / static_cast<double>(node.neighbors.size());
    }
    std::printf("neighbor_distance_percentile=%.1f%% (100%% = same as random)\n",
                100.0 * sum_pct / static_cast<double>(probe_nodes));
  }

  // 入口点自身的出边数，作为"图是否像样"的粗检。
  std::printf(
      "reachable_from_entry=%zu/%zu (%.2f%%) dead_ends=%zu self_loops=%zu "
      "entry_degree=%zu\n",
      reachable, static_cast<std::size_t>(meta.vector_count),
      100.0 * static_cast<double>(reachable) /
          static_cast<double>(meta.vector_count),
      dead_ends, self_loops, session.get_node(entry, false).neighbors.size());
  return reachable == meta.vector_count ? 0 : 2;
}
