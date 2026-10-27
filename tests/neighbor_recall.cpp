// neighbor_recall <index> <base.fvecs> [samples] [k]
//
// 图的"邻居召回"：对采样节点，先暴力找出它自己的最近 k 个点，再看该节点的
// 出边中有几个落在其中。这是不受查询影响的图质量指标 —— 比搜索召回更能
// 直接定位构建缺陷：搜索召回低可能是入口/终止问题，邻居召回低一定是构图问题。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/query_page_session.h"
#include "core/simd_distance.h"
#include "data/dataset.h"
#include "graph/disk_graph_index.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <index> <base.fvecs> [samples] [k]\n",
                 argv[0]);
    return 1;
  }
  const std::size_t samples =
      argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 200;
  const std::size_t k = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 10;

  const auto base = agent_aware::load_fvecs(argv[2]);
  agent_aware::PackedDiskGraphIndex index(argv[1]);
  agent_aware::DiskGraphSearchStats stats;
  agent_aware::QueryPageSession session(index, agent_aware::DiskGraphSearchConfig{},
                                        stats);
  const std::size_t count = index.metadata().vector_count;
  if (base.size() < count) {
    std::fprintf(stderr, "base has fewer vectors than the index\n");
    return 1;
  }

  const std::size_t step = std::max<std::size_t>(1, count / (samples + 1));
  std::vector<std::pair<float, std::uint32_t>> ranked;
  ranked.reserve(count);
  double recall_sum = 0.0;
  std::size_t probed = 0;
  for (std::size_t sample = 0; sample < samples; ++sample) {
    const auto id = static_cast<std::uint32_t>((sample + 1) * step % count);
    const float* row = base.row(id);
    ranked.clear();
    for (std::size_t other = 0; other < count; ++other) {
      if (other == id) {
        continue;
      }
      ranked.emplace_back(
          agent_aware::l2_distance_sq_simd(row, base.row(other), base.dim),
          static_cast<std::uint32_t>(other));
    }
    std::partial_sort(ranked.begin(), ranked.begin() + k, ranked.end());
    const auto& node = session.get_node(id, false);
    std::size_t hits = 0;
    for (std::size_t i = 0; i < k; ++i) {
      if (std::find(node.neighbors.begin(), node.neighbors.end(),
                    ranked[i].second) != node.neighbors.end()) {
        ++hits;
      }
    }
    recall_sum += static_cast<double>(hits) / static_cast<double>(k);
    ++probed;
  }
  std::printf("neighbor_recall@%zu=%.4f samples=%zu nodes=%zu\n", k,
              recall_sum / static_cast<double>(probed), probed, count);
  return 0;
}
