// V3 packed 页的度数约束：邻接表长度超过页格式的 degree 时必须拒绝编码，
// 而不是静默截断。静默截断会让"内存图连通、落盘图断裂"，而且没有任何日志
// ——这正是 1M 索引召回塌到 0.0002 的直接原因之一。

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "core/types.h"
#include "graph/disk_page_codec.h"

int main() {
  constexpr std::size_t kDegree = 2;
  constexpr std::size_t kPageSize = 4096;
  constexpr std::uint32_t kCount = 4;

  agent_aware::VectorSet base;
  base.dim = 4;
  base.values.assign(static_cast<std::size_t>(kCount) * base.dim, 0.0f);

  std::vector<std::vector<std::uint32_t>> graph(kCount);
  std::vector<std::uint32_t> order(kCount);
  for (std::uint32_t i = 0; i < kCount; ++i) {
    order[i] = i;
  }

  const std::size_t nodes_per_page =
      agent_aware::packed_nodes_per_page(base.dim, kDegree, kPageSize);
  if (nodes_per_page == 0) {
    std::fprintf(stderr, "page too small for the test geometry\n");
    return 1;
  }

  const auto encode = [&](std::uint32_t page_id) {
    return agent_aware::DiskPageCodec::encode_packed_page(
        page_id, base, graph, order, nodes_per_page, kPageSize, kDegree);
  };

  // 每条边都在度数以内：应当成功。
  graph[0] = {1, 2};
  try {
    (void)encode(0);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "legal page was rejected: %s\n", error.what());
    return 1;
  }

  // 节点 1 有 3 条边，超过 degree=2：必须拒绝。
  graph[1] = {0, 2, 3};
  bool rejected = false;
  try {
    (void)encode(0);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  if (!rejected) {
    std::fprintf(stderr,
                 "overfull adjacency was silently truncated instead of rejected\n");
    return 1;
  }
  std::printf("packed_degree_guard: ok\n");
  return 0;
}
