#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agentmem/core/brute_force.h"
#include "agentmem/core/types.h"
#include "agentmem/graph/disk_graph_index.h"

namespace agentmem {

struct EngineSearchStats {
  bool used_graph_path = false;
  DiskGraphSearchStats graph;
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
  virtual void insert(std::uint32_t id, const float* vector);
  virtual void update(std::uint32_t id, const float* vector);
};

class ExactMemoryEngine final : public StorageEngine {
 public:
  explicit ExactMemoryEngine(const VectorSet& base);

  EngineSearchResult search_one(const float* query,
                                std::size_t top_k) override;

 private:
  BruteForceIndex index_;
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
  DiskGraphSearchConfig search;
};

class PackedGraphEngine final : public StorageEngine {
 public:
  explicit PackedGraphEngine(PackedGraphEngineConfig config);

  EngineSearchResult search_one(const float* query,
                                std::size_t top_k) override;

  const DiskGraphMetadata& metadata() const {
    return index_.metadata();
  }

  const DiskGraphIoStatus& io_status() const {
    return index_.io_status();
  }

 private:
  PackedGraphEngineConfig config_;
  PackedDiskGraphIndex index_;
};

}  // namespace agentmem
