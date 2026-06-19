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

  const std::uint64_t read_sequence =
      config_.dynamic_manager ? config_.dynamic_manager->current_sequence() : 0;
  const auto graph_result = index_.search_one(query, search);
  EngineSearchResult output;
  if (config_.dynamic_manager) {
    std::vector<std::uint32_t> base_ids;
    base_ids.reserve(graph_result.topk.size());
    for (const auto& result : graph_result.topk) {
      base_ids.push_back(result.id);
    }
    auto latest_base_records = config_.dynamic_manager->latest_records_for(
        base_ids, read_sequence, true);
    auto delta_records = config_.dynamic_manager->search_delta_l2_at(
        query, metadata().dim, top_k, read_sequence);
    delta_records.reserve(delta_records.size() + latest_base_records.size());
    for (auto& item : latest_base_records) {
      delta_records.push_back(std::move(item.second));
    }
    output.topk = dynamic::merge_base_and_delta_l2(
        graph_result.topk, delta_records, query, metadata().dim, top_k);
  } else {
    output.topk = graph_result.topk;
  }
  output.stats.used_graph_path = true;
  output.stats.dynamic_read_sequence = read_sequence;
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
