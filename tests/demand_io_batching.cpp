// Phase 1 回归：扩展路径的 demand 读必须成批提交，且成批不能改变搜索结果。
//
// Phase 0 §12 的结论是"每页一次 io_uring_submit + 一次 wait_cqe"：页数等于
// 系统调用数。修掉之后，同样的查询在 io_uring 下应当用远少于此的提交趟数
// 读同样多的页，并且 top-k 与 page_reads 不变。
//
// 三条断言：
//   1. 归因恒等式：各阶段 pages 之和 == page_reads == submitted_reads；
//      邻居阶段确实发生了 demand 读（否则这个测试什么也没覆盖）；
//   2. 批量生效（仅 io_uring）：io_submit_syscalls < page_reads；
//   3. 结果不变：io_uring 与 pread 两种 I/O 模式给出逐位相同的 top-k，
//      且与 pread 的 page_reads 相同。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/pq_encoder.h"
#include "graph/disk_graph_builder.h"
#include "graph/disk_graph_index.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// 确定性的、带簇结构的合成底库：纯线性同余会让极小图上的近邻关系退化，
// 这里用 splitmix64 生成 32 个簇心加噪声，查询取底库点加噪。
agent_aware::VectorSet make_clustered_vectors(std::size_t count, std::size_t dim,
                                              std::uint64_t seed,
                                              float noise) {
  auto splitmix = [&seed]() {
    seed += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = seed;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  };
  auto uniform = [&]() {
    return static_cast<float>(splitmix() >> 11) /
           static_cast<float>(1ULL << 53);
  };
  constexpr std::size_t kClusters = 32;
  std::vector<std::vector<float>> centers(
      kClusters, std::vector<float>(dim, 0.0f));
  for (auto& center : centers) {
    for (auto& value : center) value = uniform() * 2.0f - 1.0f;
  }
  agent_aware::VectorSet vectors(count, dim);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& center = centers[i % kClusters];
    for (std::size_t d = 0; d < dim; ++d) {
      vectors.values[i * dim + d] =
          center[d] + (uniform() * 2.0f - 1.0f) * noise;
    }
  }
  return vectors;
}

std::size_t stage_pages(const agent_aware::DiskGraphSearchStats& stats) {
  std::size_t total = 0;
  for (std::size_t stage = 0; stage < agent_aware::kDemandReadStageCount;
       ++stage) {
    total += stats.demand_by_stage[stage].pages_read;
  }
  return total;
}

struct SearchRun {
  std::vector<agent_aware::SearchResult> topk;
  agent_aware::DiskGraphSearchStats stats;
  bool io_uring = false;
};

SearchRun run_search(agent_aware::PackedDiskGraphIndex& index,
                     const std::string& io_mode,
                     const agent_aware::VectorSet& queries,
                     std::size_t query_limit) {
  SearchRun run;
  index.configure_io(io_mode, /*batch_size=*/16, /*io_depth=*/32);
  run.io_uring = index.io_status().io_uring_enabled;
  for (std::size_t q = 0; q < query_limit; ++q) {
    agent_aware::DiskGraphSearchConfig config;
    config.top_k = 10;
    config.search_width = 128;
    config.beam_width = 16;
    config.spread_start_count = 64;
    config.adc_enable = true;
    config.rerank_topk = 10;
    config.pq_model = &index.pq_model();
    config.query_id = q;
    const auto output = index.search_one(queries.row(q), config);
    if (q == 0) {
      run.topk = output.topk;
    }
    run.stats.node_reads += output.stats.node_reads;
    run.stats.submitted_reads += output.stats.submitted_reads;
    run.stats.io_submit_syscalls += output.stats.io_submit_syscalls;
    run.stats.demand_read_batches += output.stats.demand_read_batches;
    run.stats.candidate_pushes += output.stats.candidate_pushes;
    run.stats.candidate_pops += output.stats.candidate_pops;
    run.stats.candidate_corrections += output.stats.candidate_corrections;
    run.stats.demand_io_wait_us += output.stats.demand_io_wait_us;
    run.stats.demand_post_cpu_us += output.stats.demand_post_cpu_us;
    for (std::size_t stage = 0; stage < agent_aware::kDemandReadStageCount;
         ++stage) {
      auto& into = run.stats.demand_by_stage[stage];
      const auto& from = output.stats.demand_by_stage[stage];
      into.pages_read += from.pages_read;
      into.batches += from.batches;
      into.syscalls += from.syscalls;
      into.wait_calls += from.wait_calls;
      into.io_wait_us += from.io_wait_us;
      into.post_cpu_us += from.post_cpu_us;
    }
  }
  return run;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  using namespace agent_aware;

  constexpr std::size_t kCount = 600;
  constexpr std::size_t kDim = 32;
  constexpr std::size_t kQueries = 5;
  const VectorSet base = make_clustered_vectors(kCount, kDim, 20261027ULL, 0.30f);
  const VectorSet queries =
      make_clustered_vectors(kQueries, kDim, 777ULL, 0.35f);

  PqAdcModel model;
  PQTrainingConfig pq_config;
  pq_config.subspaces = 8;
  pq_config.centroids = 16;
  pq_config.train_limit = kCount;
  pq_config.iterations = 4;
  pq_config.seed = 11;
  (void)model.train(base, pq_config);
  require(model.enabled(), "PQ training failed");

  const fs::path root = fs::temp_directory_path() / "agent-aware-demand-io";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path index_path = root / "demand-io.idx";
  DiskGraphBuildConfig build;
  build.degree = 16;
  build.page_size = 4096;
  build.random_seed = 5;
  build.pq_model = &model;
  (void)PackedDiskGraphBuilder::build(base, index_path.string(), build);

  struct ResultSummary {
    std::uint32_t id = 0;
    float distance = 0.0f;
  };
  const auto summarize = [](const std::vector<SearchResult>& topk) {
    std::vector<ResultSummary> out;
    for (const auto& result : topk) out.push_back({result.id, result.distance});
    return out;
  };

  // 两次搜索必须各自新建 index 对象：会话已物化的页会留在旧对象里，
  // 复用会让第二次搜索几乎不产生 I/O，测不到任何东西。
  std::vector<ResultSummary> baseline_topk;
  std::size_t baseline_reads = 0;
  bool io_uring_available = false;
  {
    PackedDiskGraphIndex index(index_path.string());
    const SearchRun run = run_search(index, "pread", queries, kQueries);
    baseline_topk = summarize(run.topk);
    baseline_reads = run.stats.submitted_reads;
    require(!run.io_uring, "pread mode unexpectedly reported async I/O");
  }
  {
    PackedDiskGraphIndex index(index_path.string());
    const SearchRun run = run_search(index, "io_uring", queries, kQueries);
    io_uring_available = run.io_uring;
    const auto& stats = run.stats;

    // 1) 归因恒等式。
    require(stats.demand_by_stage[static_cast<std::size_t>(
                DemandReadStage::kNeighborRank)]
                .pages_read > 0,
            "no neighbor-stage demand reads: the regression exercises nothing");
    require(stage_pages(stats) == stats.submitted_reads,
            "sum of per-stage pages does not equal submitted page reads");
    require(stats.candidate_corrections == 0,
            "search fell back to approximate-distance corrections");
    require(stats.candidate_pops > 0 && stats.candidate_pushes > stats.candidate_pops,
            "candidate push/pop counters are not populated");
    require(stats.demand_io_wait_us >= 0.0 && stats.demand_post_cpu_us >= 0.0,
            "demand timing fields are negative");

    // 2) 结果不变：与 pread 逐位相同，页数也相同（批量只改提交方式，不改读什么）。
    const auto io_uring_topk = summarize(run.topk);
    require(io_uring_topk.size() == baseline_topk.size(),
            "io_uring and pread returned different top-k sizes");
    for (std::size_t i = 0; i < io_uring_topk.size(); ++i) {
      require(io_uring_topk[i].id == baseline_topk[i].id,
              "io_uring and pread returned different top-k ids");
      require(io_uring_topk[i].distance == baseline_topk[i].distance,
              "io_uring and pread returned different top-k distances");
    }
    require(stats.submitted_reads == baseline_reads,
            "io_uring and pread read a different number of pages");

    if (!run.io_uring) {
      std::printf("SKIP io_uring assertions (%s); identity checks passed\n",
                  index.io_status().fallback_reason.c_str());
      fs::remove_all(root);
      return 0;
    }

    // 3) 批量生效：这一条正是"每页一次系统调用"的反面。
    if (stats.io_submit_syscalls >= stats.submitted_reads) {
      std::fprintf(stderr,
                   "FAIL batching: page_reads=%zu io_submit_syscalls=%zu "
                   "read_batches=%zu\n",
                   stats.submitted_reads, stats.io_submit_syscalls,
                   stats.demand_read_batches);
      return 1;
    }
    std::printf(
        "OK page_reads=%zu io_submit_syscalls=%zu read_batches=%zu "
        "syscalls_per_page=%.3f stage_pages(entry/expand/neighbor/rerank)="
        "%zu/%zu/%zu/%zu\n",
        stats.submitted_reads, stats.io_submit_syscalls,
        stats.demand_read_batches,
        static_cast<double>(stats.io_submit_syscalls) /
            static_cast<double>(std::max<std::size_t>(1, stats.submitted_reads)),
        stats.demand_by_stage[0].pages_read,
        stats.demand_by_stage[1].pages_read,
        stats.demand_by_stage[2].pages_read,
        stats.demand_by_stage[3].pages_read);
  }

  fs::remove_all(root);
  return 0;
}
