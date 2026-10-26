// node_neighborhood <base.fvecs> <query.fvecs> <truth.ivecs> <node_id>...
//
// 检查某几个节点自己的出边是否指向真正的近邻。用来区分两种故障：
//   图本身局部好、搜索走不到   —— 出边距离很小
//   图本身局部就坏             —— 出边距离和随机点差不多

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "core/simd_distance.h"
#include "data/dataset.h"

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: %s <base.fvecs> <query.fvecs> <truth.ivecs> <node_id>...\n",
                 argv[0]);
    return 1;
  }
  const auto base = agent_aware::load_fvecs(argv[1]);
  const auto queries = agent_aware::load_fvecs(argv[2]);
  (void)argv[3];

  // 全库距离排序一次，作为"第 p 百分位"的基准。
  std::vector<float> all_distances;
  all_distances.reserve(queries.size() * base.size());
  for (std::size_t q = 0; q < queries.size(); ++q) {
    const float* query = queries.values.data() + q * base.dim;
    for (std::size_t i = 0; i < base.size(); ++i) {
      all_distances.push_back(
          agent_aware::l2_distance_sq_simd(query, base.row(i), base.dim));
    }
  }
  std::sort(all_distances.begin(), all_distances.end());

  const auto percentile = [&](float value) {
    const auto it =
        std::lower_bound(all_distances.begin(), all_distances.end(), value);
    return 100.0 * static_cast<double>(it - all_distances.begin()) /
           static_cast<double>(all_distances.size());
  };

  // 每个节点到最近 100 个底库点的距离，作为"该节点作为查询"的参考。
  std::vector<float> per_query;
  per_query.reserve(base.size());
  for (int a = 4; a < argc; ++a) {
    const auto node = static_cast<std::uint32_t>(std::atoi(argv[a]));
    if (node >= base.size()) {
      continue;
    }
    const float* row = base.row(node);
    per_query.clear();
    for (std::size_t i = 0; i < base.size(); ++i) {
      if (i == node) {
        continue;
      }
      per_query.push_back(agent_aware::l2_distance_sq_simd(row, base.row(i), base.dim));
    }
    std::partial_sort(per_query.begin(), per_query.begin() + 10, per_query.end());
    std::printf("node %u: nearest-10 of itself =", node);
    for (int i = 0; i < 10; ++i) {
      std::printf(" %.3f", static_cast<double>(per_query[i]));
    }
    std::printf("\n  global percentile of nearest: %.4f%%\n",
                percentile(per_query[0]));
  }
  return 0;
}
