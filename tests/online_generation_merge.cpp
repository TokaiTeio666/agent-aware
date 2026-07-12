#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "data/dataset.h"
#include "dynamic/dynamic_write_manager.h"
#include "engine/storage_engine.h"
#include "graph/disk_graph_index.h"

using namespace agent_aware;

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "agent-aware-online-merge";
  fs::remove_all(root);
  fs::create_directories(root);

  SyntheticConfig synthetic;
  synthetic.base_count = 128;
  synthetic.query_count = 4;
  synthetic.dim = 16;
  synthetic.clusters = 8;
  const auto data = generate_synthetic(synthetic);

  auto pq = std::make_shared<PQEncoder>();
  PQTrainingConfig training;
  training.subspaces = 4;
  training.centroids = 8;
  training.train_limit = 64;
  training.iterations = 2;
  pq->train(data.base, training);
  const auto index_path = root / "base.idx";
  DiskGraphBuildConfig build;
  build.degree = 8;
  build.pq_model = pq.get();
  PackedDiskGraphBuilder::build(data.base, index_path.string(), build);

  dynamic::DynamicWriteOptions options;
  options.dynamic_dir = root / "dynamic";
  options.pq_model = pq;
  options.enable_auto_flush = false;
  auto manager = std::make_shared<dynamic::DynamicWriteManager>(options);
  require(manager->open(), "manager open failed");

  PackedGraphEngineConfig engine_config;
  engine_config.index_path = index_path.string();
  engine_config.pq_model = pq;
  engine_config.dynamic_manager = manager;
  engine_config.search.search_width = 64;
  engine_config.search.entry_count = 1;
  engine_config.enable_auto_graph_merge = false;
  PackedGraphEngine engine(engine_config);

  const std::uint32_t node_id = 128;
  engine.insert(node_id, data.queries.row(0));
  require(manager->current_sequence() == 1, "first sequence mismatch");
  require(engine.trigger_merge(), "merge trigger failed");

  bool captured = false;
  for (int i = 0; i < 1000; ++i) {
    const auto status = engine.merge_status();
    if (status.to_sequence == 1) {
      captured = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(captured, "merge did not capture cutoff S=1");
  engine.update(node_id, data.queries.row(1));
  require(manager->current_sequence() == 2, "concurrent sequence mismatch");

  for (int i = 0; i < 5000; ++i) {
    const auto status = engine.merge_status();
    if (status.phase == PackedGraphEngine::MergeStatus::Phase::kComplete) break;
    if (status.phase == PackedGraphEngine::MergeStatus::Phase::kFailed) {
      throw std::runtime_error(status.error);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(engine.base_sequence() == 1, "generation absorbed sequence > S");
  const auto result = engine.search_one(data.queries.row(1), 10);
  require(!result.topk.empty(), "search after merge failed");
  bool found = false;
  for (const auto& hit : result.topk) found = found || hit.id == node_id;
  require(found, "sequence S+1 was not visible through active Delta");

  require(manager->close(), "manager close failed");
  fs::remove_all(root);
  return 0;
}
