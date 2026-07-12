#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "core/pq_encoder.h"
#include "graph/packed_graph_generation.h"

using namespace agent_aware;

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "agent_aware_packed_generation";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto base_path = root / "base.idx";

  VectorSet training(4, 4);
  for (std::size_t i = 0; i < training.values.size(); ++i) {
    training.values[i] = static_cast<float>(i);
  }
  auto pq = std::make_shared<PQEncoder>();
  PQTrainingConfig config;
  config.subspaces = 2;
  config.centroids = 2;
  config.train_limit = 4;
  config.iterations = 2;
  pq->train(training, config);

  auto generation0 = PackedGraphGeneration::open_or_create(base_path, pq);
  assert(generation0->id() == 0);
  assert(generation0->incorporated_sequence() == 0);

  MaterializedRecord first;
  first.node_id = 10;
  first.sequence_id = 1;
  first.dim = 4;
  first.vector = {0, 1, 2, 3};
  MaterializedRecord future = first;
  future.sequence_id = 2;
  future.vector = {9, 9, 9, 9};
  auto generation1 = PackedGraphGeneration::publish(
      base_path, *generation0, {first, future}, 1, pq);
  assert(generation1->id() == 1);
  assert(generation1->incorporated_sequence() == 1);
  MaterializedRecord resolved;
  assert(generation1->resolve(10, resolved));
  assert(resolved.sequence_id == 1);
  assert(resolved.vector == first.vector);

  auto recovered = PackedGraphGeneration::open_or_create(base_path, pq);
  assert(recovered->id() == 1);
  assert(recovered->resolve(10, resolved));
  assert(resolved.sequence_id == 1);

  {
    const auto live = std::filesystem::path(base_path.string() + ".live");
    std::ofstream corrupt(live / "segment_1.bin", std::ios::binary | std::ios::app);
    const char byte = 'x';
    corrupt.write(&byte, 1);
  }
  bool checksum_rejected = false;
  try {
    (void)PackedGraphGeneration::open_or_create(base_path, pq);
  } catch (const std::runtime_error&) {
    checksum_rejected = true;
  }
  assert(checksum_rejected);
  // Restore generation 1 so generation 2 can be published from valid bytes.
  generation1 = PackedGraphGeneration::publish(
      base_path, *generation0, {first}, 1, pq);

  MaterializedRecord erased;
  erased.node_id = 10;
  erased.sequence_id = 3;
  erased.deleted = true;
  auto generation2 = PackedGraphGeneration::publish(
      base_path, *generation1, {future, erased}, 3, pq);
  assert(generation2->resolve(10, resolved));
  assert(resolved.deleted);

  std::filesystem::remove_all(root);
  return 0;
}
