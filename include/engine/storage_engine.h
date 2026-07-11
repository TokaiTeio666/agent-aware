#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/types.h"
#include "dynamic/dynamic_write_manager.h"
#include "graph/disk_graph_index.h"

namespace agent_aware {

struct EngineSearchTiming {
  double dynamic_snapshot_ms = 0.0;
  double base_search_ms = 0.0;
  double latest_record_lookup_ms = 0.0;
  double delta_search_ms = 0.0;
  double merge_ms = 0.0;
};

struct EngineSearchStats {
  bool used_graph_path = false;
  std::uint64_t dynamic_read_sequence = 0;
  DiskGraphSearchStats graph;
  EngineSearchTiming timing;
};

struct EngineSearchResult {
  std::vector<SearchResult> topk;
  EngineSearchStats stats;
};

class StorageEngine {
 public:
  virtual ~StorageEngine() = default;

  virtual EngineSearchResult search_one(const float* query,
                                        std::size_t top_k) = 0;
  virtual void insert(std::uint32_t id, const float* vector) = 0;
  virtual void update(std::uint32_t id, const float* vector) = 0;
  virtual void erase(std::uint32_t id) = 0;
};

struct PackedGraphEngineConfig {
  std::string index_path;
  std::string cache_policy = "none";
  std::string io_mode = "pread";
  std::size_t cache_pages = 0;
  std::size_t io_batch_size = 1;
  std::size_t io_depth = 1;
  bool protect_hot_pages = false;
  std::size_t hot_degree_threshold = 0;
  std::shared_ptr<const PqAdcModel> pq_model;
  DiskGraphSearchConfig search;
  std::shared_ptr<dynamic::DynamicWriteManager> dynamic_manager;
};

class PackedGraphEngine final : public StorageEngine {
 public:
  explicit PackedGraphEngine(PackedGraphEngineConfig config);

  EngineSearchResult search_one(const float* query,
                                std::size_t top_k) override;
  void insert(std::uint32_t id, const float* vector) override;
  void update(std::uint32_t id, const float* vector) override;
  void erase(std::uint32_t id) override;
  void configure_runtime(std::size_t cache_pages, std::size_t io_depth,
                         bool prefetch_enabled,
                         std::size_t prefetch_top_k);

  const DiskGraphMetadata& metadata() const {
    return index_.metadata();
  }

  const DiskGraphIoStatus& io_status() const {
    return index_.io_status();
  }
  std::size_t cache_capacity_pages() const {
    return index_.cache_capacity_pages();
  }
  std::size_t cache_resident_pages() const {
    return index_.cache_resident_pages();
  }
  const PqAdcModel& pq_model() const { return index_.pq_model(); }

 private:
  PackedGraphEngineConfig config_;
  PackedDiskGraphIndex index_;
  mutable std::mutex config_mutex_;
};

}  // namespace agent_aware
