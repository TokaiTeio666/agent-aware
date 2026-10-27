// 图索引召回诊断：对每个查询用多个入口点分别跑一次搜索，看召回率是否
// 取决于入口。若"从真值邻居出发"能召回、"从当前入口出发"不能，问题在图
// 连通性/入口选择；若两者都召回不到，问题在搜索终止条件或 Rerank 距离。
//
// 用法：graph_probe <index> <base.fvecs> <query.fvecs> <truth.ivecs>
//                    <entry_id> [query-limit] [search-width] [--adc 0|1]
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "core/brute_force.h"
#include "data/dataset.h"
#include "graph/disk_graph_index.h"

namespace {

using agent_aware::DiskGraphSearchConfig;
using agent_aware::PackedDiskGraphIndex;

std::size_t overlap(const std::vector<agent_aware::SearchResult>& got,
                    const std::vector<std::uint32_t>& truth) {
  std::size_t hits = 0;
  for (const auto id : truth) {
    for (const auto& r : got) {
      if (r.id == id) {
        ++hits;
        break;
      }
    }
  }
  return hits;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr,
                 "usage: %s <index> <base.fvecs> <query.fvecs> <truth.ivecs> "
                 "<entry_id> [query-limit] [search-width] [--adc 0|1]\n",
                 argv[0]);
    return 1;
  }
  const std::size_t limit = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 20;
  const std::size_t width = argc > 7 ? std::strtoull(argv[7], nullptr, 10) : 64;
  bool adc = false;
  for (int i = 8; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--adc") == 0) {
      adc = std::atoi(argv[i + 1]) != 0;
    }
  }
  const auto entry =
      static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 10));
  const auto base = agent_aware::load_fvecs(argv[2]);
  const auto queries = agent_aware::load_fvecs(argv[3], limit);
  const auto truth = agent_aware::load_ivecs(argv[4], limit);
  PackedDiskGraphIndex index(argv[1]);
  std::printf(
      "vector_count=%zu degree=%u nodes_per_page=%u entry=%u width=%zu "
      "queries=%zu adc=%d\n",
      index.metadata().vector_count, index.metadata().degree,
      index.metadata().nodes_per_page, entry, width, queries.size(), adc);

  const auto run = [&](const float* query,
                       const std::vector<std::uint32_t>& seeds,
                       const std::vector<std::uint32_t>& ids) {
    DiskGraphSearchConfig config;
    config.top_k = ids.size();
    config.search_width = width;
    config.seed_ids = seeds;
    config.entry_count = seeds.size();
    if (const char* beam = std::getenv("GRAPH_PROBE_BEAM")) {
      config.beam_width = static_cast<std::size_t>(std::atoi(beam));
    }
    // search_one 在 adc_enable=true 时把 pq_model 覆盖为空就以索引自带的 PQ
    // 模型为准（见 disk_graph_index.cpp 的 effective_config.adc_enable = true），
    // 所以"关掉 ADC"的对照靠传 --adc 0 区分，传空 pq_model 并不会关掉 ADC。
    config.adc_enable = adc;
    config.pq_model = &index.pq_model();
    config.record_expanded_distances = std::getenv("GRAPH_PROBE_VERBOSE") != nullptr;
    const auto outcome = index.search_one(query, config);
    if (std::getenv("GRAPH_PROBE_VERBOSE") != nullptr) {
      std::printf(
          "     stats: expanded=%zu visited=%zu batch=%zu max_batch=%zu\n",
          outcome.stats.expanded, outcome.stats.visited,
          outcome.stats.batch_count, outcome.stats.max_batch_size);
      // 贪心只翻"更近"的点：与查询距离单调下降，于是它走进哪个局部极小就是终点。
      // 连续扩展的距离序列能直接看出它是不是卡在某个局部极小里。
      float prev = std::numeric_limits<float>::max();
      std::size_t backward = 0;
      std::printf("     expanded distances:");
      for (std::size_t i = 0; i < outcome.expanded_distances.size() && i < 40; ++i) {
        const float d = outcome.expanded_distances[i];
        if (d > prev) {
          ++backward;
        }
        prev = d;
        std::printf(" %.2f", d);
      }
      std::printf(" (backward steps in first 40: %zu)\n", backward);
    }
    return outcome;
  };

  double sum_entry = 0.0;
  double sum_truth_seed = 0.0;
  double sum_multi = 0.0;
  for (std::size_t q = 0; q < queries.size(); ++q) {
    const float* query = queries.values.data() + q * base.dim;
    const auto& ids = truth[q];
    const auto from_entry = run(query, {entry}, ids);
    const auto from_truth = run(query, {ids.front()}, ids);
    std::vector<std::uint32_t> multi(ids.begin(),
                                     ids.begin() + std::min<std::size_t>(3, ids.size()));
    const auto from_multi = run(query, multi, ids);
    const auto brute = agent_aware::search_memory_fast(base, query, ids.size());
    // 真值可能包含底库子集之外的 id（用 --base-limit 建的部分索引），
    // 这些 id 没有 PQ 码，查表会越界。诊断只统计子集内的 id。
    bool truth_inside_base = true;
    for (const auto id : ids) {
      truth_inside_base = truth_inside_base && id < base.size();
    }
    if (q < 3 && truth_inside_base) {
      // ADC 近似距离与真实距离的排序一致性：若 ADC 把最好的点排到几十名之外，
      // 图的贪心扩展就永远走不到它。
      const auto& model = index.pq_model();
      const auto table = model.build_adc_table(query);
      std::vector<std::pair<float, std::uint32_t>> adc_rank;
      adc_rank.reserve(ids.size());
      for (const auto id : ids) {
        adc_rank.emplace_back(model.adc_distance(id, table), id);
      }
      std::sort(adc_rank.begin(), adc_rank.end());
      for (const auto& [adc, id] : adc_rank) {
        std::printf("   [adc] id=%u adc=%.5f true=%.5f\n", id, adc,
                    agent_aware::squared_l2(query, base.row(id), base.dim));
      }
      std::printf("   [adc] best-by-adc=%u  best-by-true=%u\n", adc_rank[0].second,
                  brute.empty() ? 0u : brute[0].id);
    }
    const double r_entry = static_cast<double>(overlap(from_entry.topk, ids)) /
                           static_cast<double>(ids.size());
    const double r_truth = static_cast<double>(overlap(from_truth.topk, ids)) /
                           static_cast<double>(ids.size());
    const double r_multi =
        static_cast<double>(overlap(from_multi.topk, ids)) /
        static_cast<double>(ids.size());
    const double r_brute =
        static_cast<double>(overlap(brute, ids)) / static_cast<double>(ids.size());
    sum_entry += r_entry;
    sum_truth_seed += r_truth;
    sum_multi += r_multi;
    if (q < 5) {
      std::printf(
          "q%zu graph_recall(entry)=%.2f (truth_seed)=%.2f (multi)=%.2f "
          "brute=%.2f | graph_top1=%u d=%.4f | brute_top1=%u d=%.4f | truth0=%u\n",
          q, r_entry, r_truth, r_multi, r_brute,
          from_entry.topk.empty() ? 0u : from_entry.topk[0].id,
          from_entry.topk.empty() ? 0.0f : from_entry.topk[0].distance,
          brute.empty() ? 0u : brute[0].id,
          brute.empty() ? 0.0f : brute[0].distance, ids.empty() ? 0u : ids[0]);
    }
  }
  const double n = static_cast<double>(queries.size());
  std::printf("mean recall  entry=%.4f  truth_seed=%.4f  multi_seed=%.4f\n",
              sum_entry / n, sum_truth_seed / n, sum_multi / n);
  return 0;
}
