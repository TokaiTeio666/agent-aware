#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

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

  const VectorSet base = make_vectors(17, 8);
  PqAdcModel model;
  PQTrainingConfig pq;
  pq.subspaces = 4;
  pq.centroids = 4;
  pq.train_limit = 12;
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

  PackedDiskGraphIndex index(index_path.string());
  const auto& metadata = index.metadata();
  require(metadata.format_version == 3, "index is not V3");
  require(metadata.pq_codes_bytes == model.code_bytes(),
          "global PQ code size mismatch");
  require(metadata.pq_codebook_bytes == model.codebook_bytes(),
          "global PQ codebook size mismatch");
  require(index.pq_model().vector_count() == base.size(),
          "index PQ model vector count mismatch");
  require(metadata.index_bytes == fs::file_size(index_path),
          "reported index size mismatch");

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
