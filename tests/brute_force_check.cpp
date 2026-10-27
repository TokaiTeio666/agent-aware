// 精确暴力检索基线：给定 .fvecs 底库/查询集与 .ivecs 真值，直接算 top-k，
// 不经过任何索引。用途是给图索引的 recall 结论一个可信的参照点：
// 若暴力基线自己都对不上真值文件，那么索引报出的 recall 就没有意义。
//
// 用法：brute_force_check <base.fvecs> <query.fvecs> <truth.ivecs> [query-limit]
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/brute_force.h"
#include "data/dataset.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s <base.fvecs> <query.fvecs> <truth.ivecs> "
                 "[query-limit]\n",
                 argv[0]);
    return 1;
  }
  const std::size_t limit = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 0;
  const auto base = agent_aware::load_fvecs(argv[1]);
  const auto queries = agent_aware::load_fvecs(argv[2], limit);
  const auto truth = agent_aware::load_ivecs(argv[3], limit);
  std::printf("base=%zu queries=%zu dim=%zu truth=%zu\n", base.size(),
              queries.size(), base.dim, truth.size());
  if (queries.size() != truth.size() || base.dim == 0) {
    std::fprintf(stderr, "error: query/truth counts disagree\n");
    return 1;
  }

  double recall_sum = 0.0;
  for (std::size_t q = 0; q < queries.size(); ++q) {
    const auto& truth_ids = truth[q];
    const std::size_t k = truth_ids.size();
    const auto got = agent_aware::search_memory_fast(
        base, queries.values.data() + q * base.dim, k);
    std::vector<std::uint32_t> got_ids;
    got_ids.reserve(got.size());
    for (const auto& r : got) {
      got_ids.push_back(r.id);
    }
    std::size_t hits = 0;
    for (const auto id : truth_ids) {
      if (std::find(got_ids.begin(), got_ids.end(), id) != got_ids.end()) {
        ++hits;
      }
    }
    recall_sum += k == 0 ? 0.0 : static_cast<double>(hits) /
                                    static_cast<double>(k);
  }
  const double recall = recall_sum / static_cast<double>(queries.size());
  // 打印几条 top-1 对照，便于肉眼判断是"整体错"还是"边缘错"。
  for (std::size_t q = 0; q < std::min<std::size_t>(queries.size(), 5); ++q) {
    const auto got = agent_aware::search_memory_fast(
        base, queries.values.data() + q * base.dim, 1);
    std::printf("q%zu brute_top1=%u truth_top1=%u%s\n", q,
                got.empty() ? 0u : got[0].id, truth[q].empty() ? 0u : truth[q][0],
                (!got.empty() && !truth[q].empty() && got[0].id == truth[q][0])
                    ? ""
                    : "   <-- MISMATCH");
  }
  std::printf("brute_force_recall_at_%zu %.6f (top1 agreement %zu/5)\n",
              truth.empty() ? 0 : truth[0].size(), recall,
              std::min<std::size_t>(queries.size(), 5));
  return 0;
}
