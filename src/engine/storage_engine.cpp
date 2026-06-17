#include "agentmem/engine/storage_engine.h"

#include <stdexcept>
#include <utility>

namespace agentmem {

void StorageEngine::insert(std::uint32_t, const float*) {
  throw std::runtime_error("StorageEngine insert is not implemented");
}

void StorageEngine::update(std::uint32_t, const float*) {
  throw std::runtime_error("StorageEngine update is not implemented");
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

}  // namespace agentmem
