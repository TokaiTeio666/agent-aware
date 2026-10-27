// entry_audit <index> [entry_id] [node_to_dump ...]
//
// 入口点审计：统计图的入度分布，并报告入口节点自身的度数与入度。
// Vamana 贪心搜索只在"从入口可达"的子图里走，因此入口若是个入度为 0、
// 只往外连边的起点，搜索就会把整个空间当成它自己的局部邻域。
// 附加的节点 id 会打印其出边，以及每个出边邻居到它自身向量的距离。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/query_page_session.h"
#include "core/simd_distance.h"
#include "graph/disk_graph_index.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <index> [entry_id]\n", argv[0]);
    return 1;
  }
  agent_aware::PackedDiskGraphIndex index(argv[1]);
  const auto& meta = index.metadata();
  const auto& indeg = index.node_in_degrees();

  const std::uint32_t entry =
      argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 0;

  std::uint64_t edges = 0;
  std::uint64_t isolated = 0;
  std::uint64_t max_indeg = 0;
  for (const auto value : indeg) {
    edges += value;
    isolated += (value == 0) ? 1 : 0;
    max_indeg = std::max<std::uint64_t>(max_indeg, value);
  }

  std::printf("nodes=%llu edges=%llu avg_indegree=%.2f max_indegree=%llu\n",
              static_cast<unsigned long long>(meta.vector_count),
              static_cast<unsigned long long>(edges),
              static_cast<double>(edges) / static_cast<double>(meta.vector_count),
              static_cast<unsigned long long>(max_indeg));
  std::printf("zero_indegree=%llu (%.2f%%)\n",
              static_cast<unsigned long long>(isolated),
              100.0 * static_cast<double>(isolated) /
                  static_cast<double>(meta.vector_count));
  if (entry < indeg.size()) {
    std::printf("entry=%u indegree=%u\n", entry, indeg[entry]);
  } else {
    std::printf("entry=%u out_of_range\n", entry);
  }

  // 无向连通分量：有向可达率掩盖了"图本身是否碎成互不相连的岛"。
  {
    std::vector<std::vector<std::uint32_t>> reverse(meta.vector_count);
    std::vector<char> seen(meta.vector_count, 0);
    std::size_t components = 0;
    std::size_t largest = 0;
    for (std::uint32_t node = 0; node < meta.vector_count; ++node) {
      if (seen[node]) {
        continue;
      }
      ++components;
      std::size_t size = 0;
      std::vector<std::uint32_t> stack{node};
      seen[node] = 1;
      agent_aware::DiskGraphSearchStats diagnose_stats;
      agent_aware::QueryPageSession diagnose_session(
          index, agent_aware::DiskGraphSearchConfig{}, diagnose_stats);
      while (!stack.empty()) {
        const auto current = stack.back();
        stack.pop_back();
        ++size;
        const auto& node_data = diagnose_session.get_node(current, false);
        for (const auto neighbor : node_data.neighbors) {
          if (neighbor < meta.vector_count && !seen[neighbor]) {
            seen[neighbor] = 1;
            stack.push_back(neighbor);
          }
        }
      }
      largest = std::max(largest, size);
    }
    std::printf(
        "undirected_components=%zu largest=%zu (%.2f%%) "
        "note: 只沿出边统计，孤立指向不构成连接\n",
        components, largest,
        100.0 * static_cast<double>(largest) /
            static_cast<double>(meta.vector_count));
    (void)reverse;
  }

  if (argc > 3) {
    agent_aware::DiskGraphSearchStats stats;
    agent_aware::QueryPageSession session(index, agent_aware::DiskGraphSearchConfig{},
                                          stats);
    for (int a = 3; a < argc; ++a) {
      const auto id = static_cast<std::uint32_t>(std::atoi(argv[a]));
      if (id >= meta.vector_count) {
        continue;
      }
      const auto& node = session.get_node(id, false);
      std::printf("node %u: degree=%zu out-edges:", id, node.neighbors.size());
      std::size_t close = 0;
      for (const auto neighbor : node.neighbors) {
        if (neighbor >= meta.vector_count) {
          continue;
        }
        const auto& other = session.get_node(neighbor, false);
        const float distance = agent_aware::squared_l2(
            node.vector.data(), other.vector.data(), meta.dim);
        if (distance < 20.0f) {
          ++close;
        }
        std::printf("\n  -> %-8u d=%.3f indeg=%u page=%u", neighbor,
                    static_cast<double>(distance), indeg[neighbor],
                    session.page_for_node(neighbor));
      }
      std::printf("\n  close(<20) out-edges: %zu/%zu\n", close,
                  node.neighbors.size());
    }
  }
  return 0;
}
