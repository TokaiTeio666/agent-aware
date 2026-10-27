// u_reach <index> <node>...
//
// 有向可达规模：从每个给定节点出发，沿出边 BFS 能到达多少个节点。
// 用来判断某个节点是不是处在一个"孤岛"里。用位图而不是无序容器，
// 120 万节点规模的多次 BFS 也能跑得动。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/query_page_session.h"
#include "graph/disk_graph_index.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <index> <node>...\n", argv[0]);
    return 1;
  }
  agent_aware::PackedDiskGraphIndex index(argv[1]);
  if (argc > 2 && std::string(argv[2]) == "--cache") {
    index.configure_cache("lru", 64, false, 0);
  }
  agent_aware::DiskGraphSearchStats stats;
  agent_aware::QueryPageSession session(index, agent_aware::DiskGraphSearchConfig{},
                                        stats);
  const std::size_t count = index.metadata().vector_count;
  std::vector<char> seen(count, 0);
  std::vector<std::uint32_t> queue;
  queue.reserve(count);
  const int first = (argc > 2 && std::string(argv[2]) == "--cache") ? 3 : 2;
  for (int a = first; a < argc; ++a) {
    const auto start = static_cast<std::uint32_t>(std::atoi(argv[a]));
    if (start >= count) {
      continue;
    }
    std::fill(seen.begin(), seen.end(), 0);
    queue.clear();
    seen[start] = 1;
    queue.push_back(start);
    for (std::size_t head = 0; head < queue.size(); ++head) {
      for (const auto neighbor : session.get_node(queue[head], false).neighbors) {
        if (neighbor < count && !seen[neighbor]) {
          seen[neighbor] = 1;
          queue.push_back(neighbor);
        }
      }
    }
    std::printf("%u reachable=%zu (%.3f%%)\n", start, queue.size(),
                100.0 * static_cast<double>(queue.size()) /
                    static_cast<double>(count));
    if (std::getenv("U_REACH_LEVELS") != nullptr) {
      // 逐层 BFS：每层打印节点数，以及是否有节点"只是自己"（无出边）。
      std::fill(seen.begin(), seen.end(), 0);
      std::vector<std::uint32_t> frontier{start};
      seen[start] = 1;
      for (int level = 0; level < 8 && !frontier.empty(); ++level) {
        std::printf("  level %d: %zu nodes\n", level, frontier.size());
        std::vector<std::uint32_t> next;
        std::size_t sinks = 0;
        for (const auto id : frontier) {
          const auto& node = session.get_node(id, false);
          if (node.neighbors.empty()) {
            ++sinks;
          }
          for (const auto neighbor : node.neighbors) {
            if (neighbor < count && !seen[neighbor]) {
              seen[neighbor] = 1;
              next.push_back(neighbor);
            }
          }
        }
        std::printf("    new=%zu sinks=%zu\n", next.size(), sinks);
        frontier.swap(next);
      }
    }
  }
  return 0;
}
