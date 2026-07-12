#include <filesystem>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <string>

#include "core/pq_encoder.h"
#include "data/dataset.h"
#include "graph/disk_graph_index.h"
#include "harmony/harmony_engine_facade.h"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() {
  namespace fs = std::filesystem;
  using namespace agent_aware;
  using namespace agent_aware::harmony;

  const fs::path root = fs::temp_directory_path() / "agent-aware-harmony-facade";
  fs::remove_all(root);
  fs::create_directories(root);
  SyntheticConfig synthetic;
  synthetic.base_count = 128;
  synthetic.query_count = 4;
  synthetic.dim = 16;
  synthetic.clusters = 8;
  const auto data = generate_synthetic(synthetic);

  PqAdcModel pq;
  PQTrainingConfig training;
  training.subspaces = 4;
  training.centroids = 8;
  training.train_limit = 64;
  training.iterations = 2;
  (void)pq.train(data.base, training);
  const fs::path index_path = root / "base.idx";
  const fs::path pq_path = root / "base.idx.pq";
  pq.write_file(pq_path.string());
  DiskGraphBuildConfig build;
  build.degree = 8;
  build.pq_model = &pq;
  PackedDiskGraphBuilder::build(data.base, index_path.string(), build);

  HarmonyEngineConfig config;
  config.base_index_path = index_path.string();
  config.pq_model_path = pq_path.string();
  config.dynamic_dir = (root / "dynamic").string();
  config.expected_dimension = 16;
  config.cache_pages = 16;
  config.io_mode = "thread_pool_pread";
  config.io_depth = 2;
  config.prefetch_policy = "none";

  HarmonyEngineConfig embedded_config = config;
  embedded_config.pq_model_path.clear();
  embedded_config.dynamic_dir = (root / "embedded-dynamic").string();
  HarmonyEngineFacade embedded_facade;
  const auto embedded_opened = embedded_facade.open(embedded_config);
  require(embedded_opened.status.ok(), "embedded PQ facade open failed");
  const auto embedded_search =
      embedded_facade.search(data.queries.row(0), 16, 10);
  require(embedded_search.status.ok() && !embedded_search.value.topk.empty(),
          "embedded PQ facade search failed");
  require(embedded_facade.close().ok(), "embedded PQ facade close failed");

  HarmonyEngineFacade facade;
  const auto opened = facade.open(config);
  require(opened.status.ok(), "facade open failed");
  require(opened.value.effective_io_mode == "thread_pool_pread",
          "thread pool backend was not selected");
  auto search = facade.search(data.queries.row(0), 16, 10);
  require(search.status.ok() && !search.value.topk.empty(), "search failed");
  require(!facade.search(data.queries.row(0), 15, 10).status.ok(),
          "dimension mismatch was accepted");
  require(facade.upsert(500, data.queries.row(1), 16,
                        WriteMode::kInsert).ok(), "insert failed");
  require(facade.erase(500).ok(), "erase failed");
  require(facade.flush().ok(), "flush failed");
  require(facade.trigger_merge().ok(), "manual graph merge trigger failed");
  bool merge_complete = false;
  for (int i = 0; i < 2000; ++i) {
    const auto merge = facade.merge_status();
    require(merge.status.ok(), "merge status failed");
    if (merge.value.phase == PackedGraphEngine::MergeStatus::Phase::kComplete) {
      merge_complete = true;
      break;
    }
    if (merge.value.phase == PackedGraphEngine::MergeStatus::Phase::kFailed) {
      throw std::runtime_error(merge.value.error);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(merge_complete, "manual graph merge did not complete");
  require(facade.set_resource_profile(ResourceProfile::kCritical).ok(),
          "critical profile failed");
  const auto metrics = facade.metrics();
  require(metrics.status.ok(), "metrics failed");
  require(metrics.value.total_searches == 1, "search metric mismatch");
  require(metrics.value.total_writes == 1, "write metric mismatch");
  require(metrics.value.total_deletes == 1, "delete metric mismatch");
  require(metrics.value.io.depth == 1, "critical I/O depth mismatch");
  require(facade.close().ok() && facade.close().ok(), "close is not idempotent");

  HarmonyEngineFacade recovered;
  require(recovered.open(config).status.ok(), "recovery open failed");
  require(recovered.metrics().value.dynamic_sequence >= 2,
          "dynamic sequence was not recovered");
  require(recovered.close().ok(), "recovery close failed");
  fs::remove_all(root);
  return 0;
}
