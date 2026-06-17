#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentmem/core/brute_force.h"
#include "agentmem/core/pq_encoder.h"
#include "agentmem/core/simd_distance.h"
#include "agentmem/data/dataset.h"
#include "agentmem/engine/storage_engine.h"
#include "agentmem/graph/disk_graph_index.h"
#include "agentmem/graph/vamana_builder.h"
#include "agentmem/storage/lsm_tree.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

agentmem::VectorSet tiny_vectors() {
  agentmem::VectorSet vectors(5, 2);
  const float values[] = {
      0.0f, 0.0f,
      1.0f, 0.0f,
      0.0f, 1.0f,
      5.0f, 5.0f,
      6.0f, 5.0f,
  };
  std::copy(std::begin(values), std::end(values), vectors.values.begin());
  return vectors;
}

agentmem::VectorSet patterned_vectors(std::size_t count, std::size_t dim) {
  agentmem::VectorSet vectors(count, dim);
  for (std::size_t row = 0; row < count; ++row) {
    float* values = vectors.mutable_row(row);
    for (std::size_t d = 0; d < dim; ++d) {
      const int bucket = static_cast<int>((row * 37 + d * 17) % 101);
      values[d] = static_cast<float>(bucket - 50) / 25.0f;
    }
  }
  return vectors;
}

void test_brute_force() {
  const auto base = tiny_vectors();
  const agentmem::BruteForceIndex exact(base);
  const float query[] = {0.05f, 0.0f};
  const auto results = exact.search_one(query, 2);
  require(results.size() == 2, "brute force result count");
  require(results[0].id == 0, "brute force nearest id");
  require(results[1].id == 1, "brute force second id");

  const auto fast_results = agentmem::search_memory_fast(base, query, 3);
  require(fast_results.size() == 3, "memory fast result count");
  require(fast_results[0].id == 0, "memory fast nearest id");
  require(fast_results[1].id == 1, "memory fast second id");

  agentmem::VectorSet query_batch(2, 2);
  const float batch_values[] = {
      0.05f, 0.0f,
      0.95f, 0.0f,
  };
  std::copy(std::begin(batch_values), std::end(batch_values),
            query_batch.values.begin());
  const auto batch_results =
      agentmem::search_memory_fast_batch(base, query_batch, 1);
  require(batch_results.size() == 2, "memory fast batch query count");
  require(batch_results[0][0].id == 0, "memory fast batch first nearest id");
  require(batch_results[1][0].id == 1, "memory fast batch second nearest id");

  agentmem::ExactMemoryEngine engine(base);
  const auto engine_result = engine.search_one(query, 1);
  require(engine_result.topk.size() == 1, "exact engine result count");
  require(engine_result.topk[0].id == 0, "exact engine nearest id");
}

void test_simd_distance_wrapper() {
  const std::vector<float> lhs = {
      0.0f, 1.0f, -2.0f, 3.5f, 4.0f, 5.25f, -6.0f, 7.0f, 8.5f};
  const std::vector<float> rhs = {
      1.0f, -1.0f, -3.0f, 2.5f, 5.5f, 3.25f, -4.0f, 6.0f, 10.0f};
  const float expected = agentmem::squared_l2(lhs.data(), rhs.data(),
                                             lhs.size());
  const float actual = agentmem::l2_distance_sq_simd(lhs.data(), rhs.data(),
                                                    lhs.size());
  require(actual == expected, "simd distance wrapper matches squared_l2");
}

void test_synthetic_data() {
  agentmem::SyntheticConfig config;
  config.base_count = 32;
  config.query_count = 8;
  config.dim = 4;
  config.clusters = 4;
  config.workload = "agent";
  const auto data = agentmem::generate_synthetic(config);
  require(data.base.size() == 32, "synthetic base count");
  require(data.queries.size() == 8, "synthetic query count");
  require(data.base.dim == 4, "synthetic base dim");
  require(data.queries.dim == 4, "synthetic query dim");

  agentmem::DatasetLoadConfig load_config;
  load_config.synthetic = true;
  load_config.synthetic_config = config;
  const auto loaded = agentmem::load_dataset(load_config);
  require(loaded.mode == "synthetic", "loaded synthetic mode");
  require(!loaded.truth_from_file, "synthetic truth source");
  require(loaded.base.size() == 32, "loaded synthetic base count");
  require(loaded.queries.size() == 8, "loaded synthetic query count");
}

void test_wal_and_delta() {
  std::filesystem::create_directories("build");
  const std::string wal_path = "build/smoke_lsm.wal";
  std::filesystem::remove(wal_path);

  const float first[] = {1.0f, 2.0f};
  const float second[] = {3.0f, 4.0f};
  {
    agentmem::WalWriter wal(wal_path, 2);
    wal.append_insert(10, first);
    wal.append_delete(3);
    wal.append_insert(11, second);
    wal.flush();
    require(wal.stats().records == 3, "wal record count");
  }

  std::vector<std::uint32_t> replayed_ids;
  std::vector<std::uint32_t> deleted_ids;
  agentmem::DeltaFlatIndex delta(2);
  const auto replay = agentmem::replay_wal_records(
      wal_path, 2,
      [&](std::uint32_t id, const float* vector) {
        replayed_ids.push_back(id);
        delta.insert(id, vector);
      },
      [&](std::uint32_t id) {
        deleted_ids.push_back(id);
      });

  require(replay.records == 3, "wal replay records");
  require(replay.inserts == 2, "wal replay inserts");
  require(replay.deletes == 1, "wal replay deletes");
  require(replayed_ids.size() == 2, "wal replay insert ids");
  require(deleted_ids.size() == 1, "wal replay delete ids");

  const float query[] = {1.0f, 2.1f};
  const auto delta_results = delta.search_one(query, 1);
  require(delta_results.size() == 1, "delta result count");
  require(delta_results[0].id == 10, "delta nearest id");
}

void test_pq_adc() {
  const auto base = tiny_vectors();
  agentmem::PQEncoder pq;
  const auto stats = pq.train(
      base, agentmem::PQTrainingConfig{2, 2, base.size(), 2, 7});
  require(pq.enabled(), "pq enabled");
  require(pq.subspaces() == 2, "pq subspaces");
  require(pq.code_bytes() == base.size() * 2, "pq code bytes");
  require(stats.training_vectors == base.size(), "pq training vector count");

  const float query[] = {0.0f, 0.0f};
  const auto code = pq.encode(query);
  const auto decoded = pq.decode_code(code.data());
  require(code.size() == 2, "pq encoded code size");
  require(decoded.size() == base.dim, "pq decoded vector dim");
  const auto table = pq.build_adc_table(query);
  const float distance = pq.adc_distance(0, table);
  require(distance >= 0.0f, "pq adc distance");

  agentmem::PqAdcModel compat;
  compat.train(base, 2, 2, base.size(), 2, 7);
  require(compat.code_bytes() == pq.code_bytes(), "pq compatibility wrapper");
}

void test_vamana_builder() {
  const auto base = tiny_vectors();
  agentmem::VamanaBuildConfig config;
  config.max_degree = 2;
  config.search_width = 8;
  config.alpha = 1.2;
  config.seed = 11;

  const agentmem::VamanaBuilder builder(config);
  const auto graph = builder.build(base);
  require(graph.adjacency.size() == base.size(), "vamana graph size");
  require(graph.medoid < base.size(), "vamana medoid range");
  require(graph.stats.max_degree <= config.max_degree,
          "vamana max degree bound");
  require(!graph.stats.candidate_counts.empty(),
          "vamana candidate stats");

  for (std::size_t i = 0; i < graph.adjacency.size(); ++i) {
    require(graph.adjacency[i].size() <= config.max_degree,
            "vamana node degree bound");
    for (const auto neighbor : graph.adjacency[i]) {
      require(neighbor < base.size(), "vamana neighbor range");
      require(neighbor != i, "vamana excludes self loops");
    }
  }

  const auto candidates = builder.search_for_construction(
      base, graph.adjacency, base.row(0), graph.medoid, 0);
  require(!candidates.empty(), "vamana construction search candidates");
  const auto pruned = builder.robust_prune(base, 0, candidates);
  require(pruned.size() <= config.max_degree, "vamana robust prune degree");

  agentmem::VamanaBuildConfig partial_delete = config;
  partial_delete.deleted_ids = {0};
  const auto partial_graph = agentmem::VamanaBuilder(partial_delete).build(base);
  require(partial_graph.medoid != 0, "vamana medoid skips deleted nodes");
  require(partial_graph.adjacency[0].empty(),
          "vamana deleted node adjacency is empty");

  agentmem::VamanaBuildConfig all_deleted = config;
  all_deleted.deleted_ids = {0, 1, 2, 3, 4};
  bool threw = false;
  try {
    (void)agentmem::VamanaBuilder(all_deleted).find_medoid(base);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "vamana all-deleted medoid throws");
}

void test_packed_graph_search() {
  std::filesystem::create_directories("build");
  const std::string index_path = "build/smoke_graph.idx";
  std::filesystem::remove(index_path);

  const auto base = tiny_vectors();
  agentmem::DiskGraphBuildConfig build;
  build.degree = 2;
  build.page_size = 4096;
  build.build_policy = "vamana";
  build.packing_strategy = "bfs";
  build.approx_candidate_limit = 8;
  build.robust_prune_alpha = 1.2;
  agentmem::PackedDiskGraphBuilder::build(base, index_path, build);

  agentmem::PackedDiskGraphIndex index(index_path);
  index.configure_cache("graph-aware-2q", 2, true, 1);
  index.configure_io("pread", 1, 1);

  agentmem::DiskGraphSearchConfig search;
  search.top_k = 2;
  search.search_width = 8;
  search.entry_count = 2;
  search.seed_ids = {0, 1};

  const float query[] = {0.0f, 0.0f};
  const auto result = index.search_one(query, search);
  require(result.topk.size() == 2, "packed graph result count");
  require(result.topk[0].id == 0, "packed graph nearest id");
  require(result.stats.expanded > 0, "packed graph expanded nodes");
}

void test_packed_graph_beam_batching() {
  std::filesystem::create_directories("build");
  const std::string index_path = "build/beam_batching_smoke.idx";
  std::filesystem::remove(index_path);

  const auto base = patterned_vectors(32, 8);
  agentmem::DiskGraphBuildConfig build;
  build.degree = 4;
  build.page_size = 4096;
  build.build_policy = "exact";
  build.packing_strategy = "bfs";
  agentmem::PackedDiskGraphBuilder::build(base, index_path, build);

  agentmem::PackedDiskGraphIndex index(index_path);
  index.configure_io("pread", 1, 1);

  agentmem::DiskGraphSearchConfig search;
  search.top_k = 3;
  search.search_width = 8;
  search.entry_count = 4;
  search.seed_ids = {0, 1, 2, 3};
  search.prefetch_policy = "none";

  const float* query = base.row(0);

  search.beam_width = 1;
  auto result = index.search_one(query, search);
  require(!result.topk.empty(), "beam_width=1 returns results");
  require(result.stats.expanded <= search.search_width,
          "beam_width=1 respects search_width");
  require(result.stats.max_batch_size <= 1,
          "beam_width=1 limits batch size");
  require(result.stats.batch_expanded == result.stats.expanded,
          "beam_width=1 batch stats match expanded");

  search.beam_width = 3;
  result = index.search_one(query, search);
  require(result.stats.expanded <= search.search_width,
          "beam_width<search_width respects search_width");
  require(result.stats.max_batch_size <= search.beam_width,
          "beam_width<search_width limits batch size");
  require(result.stats.batch_count > 0,
          "beam_width<search_width records batches");
  require(result.stats.duplicate_pages_eliminated > 0,
          "beam batch deduplicates same-page candidates");

  search.search_width = 5;
  search.beam_width = 32;
  result = index.search_one(query, search);
  require(result.stats.expanded <= search.search_width,
          "beam_width>search_width respects search_width");
  require(result.stats.max_batch_size <= search.search_width,
          "beam_width>search_width clamps effective batch size");

  std::filesystem::remove(index_path);
}

void test_packed_graph_io_uring_fallback_batch() {
  std::filesystem::create_directories("build");
  const std::string index_path = "build/uring_fallback_batch_smoke.idx";
  std::filesystem::remove(index_path);

  const auto base = patterned_vectors(24, 8);
  agentmem::DiskGraphBuildConfig build;
  build.degree = 4;
  build.page_size = 5000;
  build.build_policy = "exact";
  build.packing_strategy = "bfs";
  agentmem::PackedDiskGraphBuilder::build(base, index_path, build);

  agentmem::PackedDiskGraphIndex index(index_path);
  index.configure_io("io_uring", 8, 8);
  require(!index.io_status().io_uring_enabled,
          "unaligned page size falls back from io_uring");

  agentmem::DiskGraphSearchConfig search;
  search.top_k = 3;
  search.search_width = 8;
  search.beam_width = 4;
  search.entry_count = 4;
  search.seed_ids = {0, 1, 2, 3};
  search.prefetch_policy = "none";

  const auto result = index.search_one(base.row(0), search);
  require(!result.topk.empty(), "io_uring fallback returns results");
  require(result.stats.expanded <= search.search_width,
          "io_uring fallback respects search_width");
  require(result.stats.batch_count > 0,
          "io_uring fallback still records beam batches");
  require(result.stats.io_submits == result.stats.io_completions,
          "io_uring fallback reads complete");

  std::filesystem::remove(index_path);
}

void test_packed_graph_async_prefetch() {
  std::filesystem::create_directories("build");
  const std::string index_path = "build/p4_async_prefetch_smoke.idx";
  std::filesystem::remove(index_path);

  const auto base = patterned_vectors(64, 64);
  agentmem::DiskGraphBuildConfig build;
  build.degree = 8;
  build.page_size = 4096;
  build.build_policy = "exact";
  build.packing_strategy = "bfs";
  agentmem::PackedDiskGraphBuilder::build(base, index_path, build);

  agentmem::PackedDiskGraphIndex sync_index(index_path);
  sync_index.configure_io("pread", 1, 1);
  agentmem::DiskGraphSearchConfig search;
  search.top_k = 5;
  search.search_width = 24;
  search.entry_count = 4;
  search.seed_ids = {0, 16, 32, 48};
  search.prefetch_policy = "none";

  const float* query = base.row(7);
  const auto sync_result = sync_index.search_one(query, search);
  require(sync_result.topk.size() == search.top_k,
          "sync packed graph result count");

  agentmem::PackedDiskGraphIndex async_index(index_path);
  async_index.configure_io("io_uring", 4, 4);
  if (!async_index.io_status().io_uring_enabled) {
    std::filesystem::remove(index_path);
    return;
  }

  search.prefetch_policy = "frontier-next-hop";
  search.prefetch_width = 2;
  search.page_dedup = true;
  const auto async_result = async_index.search_one(query, search);
  require(async_result.topk.size() == sync_result.topk.size(),
          "async packed graph result count");
  for (std::size_t i = 0; i < sync_result.topk.size(); ++i) {
    require(async_result.topk[i].id == sync_result.topk[i].id,
            "async prefetch preserves search result ids");
  }
  require(async_result.stats.io_prefetches > 0,
          "async prefetch submits reads");
  require(async_result.stats.io_submits == async_result.stats.io_completions,
          "async submits complete");
  require(async_result.stats.io_pending_pages_peak <=
              async_index.io_status().depth,
          "async pending depth is bounded");
  require(async_result.stats.prefetch_submitted_pages >=
              async_result.stats.prefetch_useful_pages,
          "prefetch useful pages do not exceed submitted pages");

  std::filesystem::remove(index_path);
}

}  // namespace

int main() {
  test_brute_force();
  test_simd_distance_wrapper();
  test_synthetic_data();
  test_wal_and_delta();
  test_pq_adc();
  test_vamana_builder();
  test_packed_graph_search();
  test_packed_graph_beam_batching();
  test_packed_graph_io_uring_fallback_batch();
  test_packed_graph_async_prefetch();
  return 0;
}
