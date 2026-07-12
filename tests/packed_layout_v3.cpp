#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/pq_encoder.h"
#include "graph/disk_graph_builder.h"
#include "graph/disk_page_codec.h"
#include "graph/vamana_builder.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

agent_aware::VectorSet make_vectors(std::size_t count, std::size_t dim) {
  agent_aware::VectorSet vectors(count, dim);
  for (std::size_t i = 0; i < count; ++i) {
    for (std::size_t d = 0; d < dim; ++d) {
      vectors.values[i * dim + d] =
          static_cast<float>((i * 17 + d * 3) % 31) / 31.0f;
    }
  }
  return vectors;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  using namespace agent_aware;

  require(graph_record_bytes(128, 32) == 648,
          "unexpected V3 graph record size");
  require(packed_nodes_per_page(128, 32, 4096) == 6,
          "SIFT1M V3 pages must hold six nodes");

  const VectorSet base = make_vectors(256, 8);
  PqAdcModel model;
  PQTrainingConfig pq;
  pq.subspaces = 4;
  pq.centroids = 4;
  pq.train_limit = 64;
  pq.iterations = 2;
  pq.seed = 7;
  (void)model.train(base, pq);

  std::stringstream serialized(std::ios::in | std::ios::out | std::ios::binary);
  model.write(serialized);
  serialized.seekg(0);
  const PqAdcModel restored = PqAdcModel::read(serialized);
  const auto table = model.build_adc_table(base.row(3));
  const auto restored_table = restored.build_adc_table(base.row(3));
  for (std::uint32_t id = 0; id < base.size(); ++id) {
    require(std::fabs(model.adc_distance(id, table) -
                      restored.adc_distance(id, restored_table)) < 1e-6f,
            "PQ round-trip changed ADC distance");
  }

  const fs::path root = fs::temp_directory_path() / "agent-aware-v3-layout-test";
  fs::create_directories(root);
  const fs::path index_path = root / "small-v3.idx";
  DiskGraphBuildConfig build;
  build.degree = 4;
  build.page_size = 4096;
  build.random_seed = 9;
  build.pq_model = &model;
  const auto build_result =
      PackedDiskGraphBuilder::build(base, index_path.string(), build);
  const VamanaBuildConfig vamana_config{
      build.degree, std::max<std::size_t>(build.degree, 256), 1.2,
      build.random_seed, true, build.deleted_ids};
  require(build_result.medoid == VamanaBuilder(vamana_config).find_medoid(base),
          "packed builder did not expose the Vamana medoid");

  const auto loads_before = PackedDiskGraphIndex::pq_disk_load_count();
  PackedDiskGraphIndex index(index_path.string());
  const auto& metadata = index.metadata();
  require(metadata.format_version == 3, "index is not V3");
  require(metadata.pq_codes_bytes == model.code_bytes(),
          "global PQ code size mismatch");
  require(metadata.pq_codebook_bytes == model.codebook_bytes(),
          "global PQ codebook size mismatch");
  require(index.pq_model().vector_count() == base.size(),
          "index PQ model vector count mismatch");
  PackedDiskGraphIndex shared_index(index_path.string());
  require(&shared_index.pq_model() == &index.pq_model(),
          "independent index readers did not share the immutable PQ model");
  require(PackedDiskGraphIndex::pq_disk_load_count() == loads_before + 1,
          "independent index readers loaded duplicate PQ codes from disk");
  require(metadata.index_bytes == fs::file_size(index_path),
          "reported index size mismatch");

  index.configure_cache("graph-aware-2q", 2, true, 0);
  DiskGraphSearchConfig search;
  search.top_k = 4;
  search.search_width = 128;
  search.beam_width = 16;
  search.entry_count = base.size();
  search.seed_ids.resize(base.size());
  for (std::uint32_t id = 0; id < base.size(); ++id) search.seed_ids[id] = id;
  search.rerank_topk = 4;
  const auto cold = index.search_one(base.row(3), search);
  require(!cold.topk.empty() && cold.stats.page_cache_misses > 0,
          "cold graph-aware cache search did not read pages");
  require(cold.stats.page_cache_evictions > 0,
          "capacity-two graph-aware cache did not evict a page");
  require(index.cache_resident_pages() <= 2,
          "graph-aware cache exceeded its frame capacity");
  const auto warm = index.search_one(base.row(3), search);
  require(warm.stats.page_cache_hits > 0,
          "warm graph-aware cache search did not hit resident pages");
  require(cold.stats.page_cache_promotions == 0 &&
              warm.stats.page_cache_promotions == 0,
          "foreground cache hit maintained a strict 2Q/LRU chain");
  require(index.cache_resident_pages() <= 2,
          "graph-aware cache exceeded capacity after CLOCK reuse");
  require(warm.stats.query_workspace_budget_bytes == 5 * 1024 * 1024,
          "query workspace hard budget was not reported");
  require(warm.stats.query_workspace_peak_bytes <=
              warm.stats.query_workspace_budget_bytes,
          "query workspace exceeded its hard budget");

  DiskGraphSearchConfig oversized = search;
  oversized.search_width = base.size() * 4096;
  oversized.beam_width = oversized.search_width;
  oversized.rerank_topk = oversized.search_width;
  oversized.prefetch_policy = "topology";
  oversized.prefetch_top_k = 1024;
  oversized.prefetch_max_inflight = 1024;
  const auto degraded = index.search_one(base.row(3), oversized);
  require(degraded.stats.workspace_degraded,
          "oversized query was not degraded to the workspace budget");
  require(degraded.stats.query_workspace_peak_bytes <= 5 * 1024 * 1024,
          "degraded query exceeded its workspace budget");
  require(degraded.stats.effective_search_width < oversized.search_width,
          "oversized search width was not reduced");

  std::vector<std::thread> readers;
  std::vector<std::vector<SearchResult>> concurrent_results(8);
  std::vector<std::exception_ptr> reader_errors(8);
  readers.reserve(concurrent_results.size());
  for (std::size_t thread_id = 0; thread_id < concurrent_results.size();
       ++thread_id) {
    readers.emplace_back([&, thread_id] {
      try {
        concurrent_results[thread_id] =
            index.search_one(base.row(3), search).topk;
      } catch (...) {
        reader_errors[thread_id] = std::current_exception();
      }
    });
  }
  for (auto& reader : readers) reader.join();
  for (const auto& error : reader_errors) {
    if (error) std::rethrow_exception(error);
  }
  for (const auto& results : concurrent_results) {
    require(results.size() == warm.topk.size(),
            "concurrent search changed Top-K size");
    for (std::size_t i = 0; i < results.size(); ++i) {
      require(results[i].id == warm.topk[i].id,
              "concurrent search changed result ordering");
    }
  }
  require(index.cache_resident_pages() <= 2,
          "concurrent search exceeded shared cache capacity");

  const fs::path v2_path = root / "old-v2.idx";
  {
    std::ofstream output(v2_path, std::ios::binary | std::ios::trunc);
    const char magic[8] = {'A', 'M', 'F', 'P', 'V', '2', '\0', '\0'};
    output.write(magic, sizeof(magic));
  }
  bool rejected_v2 = false;
  try {
    PackedDiskGraphIndex old(v2_path.string());
  } catch (const std::runtime_error& error) {
    rejected_v2 = std::string(error.what()).find("rebuild with --rebuild-index 1") !=
                  std::string::npos;
  }
  require(rejected_v2, "V2 index did not produce the rebuild diagnostic");

  fs::remove_all(root);
  return 0;
}
