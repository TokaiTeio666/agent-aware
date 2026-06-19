#include "agent_aware/engine/storage_engine.h"

#include <stdexcept>
#include <utility>

namespace agent_aware {

void StorageEngine::insert(std::uint32_t, const float*) {
  throw std::runtime_error("StorageEngine insert is not implemented");
}

void StorageEngine::update(std::uint32_t, const float*) {
  throw std::runtime_error("StorageEngine update is not implemented");
}

void StorageEngine::erase(std::uint32_t) {
  throw std::runtime_error("StorageEngine erase is not implemented");
}

ExactMemoryEngine::ExactMemoryEngine(const VectorSet& base) : index_(base) {}

EngineSearchResult ExactMemoryEngine::search_one(const float* query,
                                                 std::size_t top_k) {
  EngineSearchResult output;
  output.topk = index_.search_one(query, top_k);
  return output;
}

PackedGraphEngine::PackedGraphEngine(PackedGraphEngineConfig config)
    : config_(std::move(config)), index_(config_.index_path) {
  index_.configure_cache(config_.cache_policy, config_.cache_pages,
                         config_.protect_hot_pages,
                         config_.hot_degree_threshold);
  index_.configure_io(config_.io_mode, config_.io_batch_size,
                      config_.io_depth);
}

EngineSearchResult PackedGraphEngine::search_one(const float* query,
                                                 std::size_t top_k) {
  DiskGraphSearchConfig search = config_.search;
  search.top_k = top_k;

  const auto graph_result = index_.search_one(query, search);
  EngineSearchResult output;
  if (config_.dynamic_manager) {
    const auto delta_records = config_.dynamic_manager->search_delta_l2(
        query, metadata().dim, top_k);
    output.topk = dynamic::merge_base_and_delta_l2(
        graph_result.topk, delta_records, query, metadata().dim, top_k);
  } else {
    output.topk = graph_result.topk;
  }
  output.stats.used_graph_path = true;
  output.stats.graph = graph_result.stats;
  return output;
}

void PackedGraphEngine::insert(std::uint32_t id, const float* vector) {
  if (!config_.dynamic_manager) {
    throw std::runtime_error("PackedGraphEngine insert requires DynamicWriteManager");
  }
  if (!config_.dynamic_manager->insert(id, vector, metadata().dim)) {
    throw std::runtime_error("PackedGraphEngine insert failed");
  }
}

void PackedGraphEngine::update(std::uint32_t id, const float* vector) {
  if (!config_.dynamic_manager) {
    throw std::runtime_error("PackedGraphEngine update requires DynamicWriteManager");
  }
  if (!config_.dynamic_manager->update(id, vector, metadata().dim)) {
    throw std::runtime_error("PackedGraphEngine update failed");
  }
}

void PackedGraphEngine::erase(std::uint32_t id) {
  if (!config_.dynamic_manager) {
    throw std::runtime_error("PackedGraphEngine erase requires DynamicWriteManager");
  }
  if (!config_.dynamic_manager->erase(id)) {
    throw std::runtime_error("PackedGraphEngine erase failed");
  }
}

}  // namespace agent_aware
