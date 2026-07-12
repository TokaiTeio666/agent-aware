#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <thread>
#include <string>
#include <vector>

#include "core/types.h"
#include "dynamic/dynamic_write_manager.h"
#include "graph/disk_graph_index.h"
#include "graph/packed_graph_generation.h"

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
  std::uint64_t base_generation = 0;
  std::uint64_t base_sequence = 0;
  dynamic::DeltaSearchStats delta;
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
  bool enable_auto_graph_merge = false;
  std::size_t graph_merge_records = 10000;
  std::uint64_t graph_merge_persisted_bytes = 64ULL * 1024ULL * 1024ULL;
  double graph_merge_delete_ratio = 0.05;
  std::size_t graph_merge_interval_ms = 300000;
  std::shared_ptr<class SharedPackedGraphRuntime> shared_runtime;
};

class SharedPackedGraphRuntime {
 public:
  explicit SharedPackedGraphRuntime(const PackedGraphEngineConfig& config);
  PackedDiskGraphIndex& index() { return *index_; }
  const PackedDiskGraphIndex& index() const { return *index_; }

 private:
  std::shared_ptr<PackedDiskGraphIndex> index_;
};

class PackedGraphEngine final : public StorageEngine {
 public:
  explicit PackedGraphEngine(PackedGraphEngineConfig config);
  ~PackedGraphEngine() override;

  EngineSearchResult search_one(const float* query,
                                std::size_t top_k) override;
  void insert(std::uint32_t id, const float* vector) override;
  void update(std::uint32_t id, const float* vector) override;
  void erase(std::uint32_t id) override;
  void configure_runtime(std::size_t cache_pages, std::size_t io_depth,
                         bool prefetch_enabled,
                         std::size_t prefetch_top_k);
  bool trigger_merge();
  bool cancel_merge();
  void set_auto_merge_enabled(bool enabled);

  struct MergeStatus {
    enum class Phase { kIdle, kSnapshot, kWriting, kPublishing, kComplete, kFailed, kCancelled };
    Phase phase = Phase::kIdle;
    std::uint64_t generation = 0;
    std::uint64_t from_sequence = 0;
    std::uint64_t to_sequence = 0;
    std::size_t processed_records = 0;
    std::uint64_t reclaimed_bytes = 0;
    std::uint64_t workspace_bytes = 0;
    std::string error;
    bool cancellable = false;
  };
  MergeStatus merge_status() const;
  std::uint64_t base_generation() const;
  std::uint64_t base_sequence() const;

  const DiskGraphMetadata& metadata() const {
    return index().metadata();
  }

  const DiskGraphIoStatus& io_status() const {
    return index().io_status();
  }
  std::size_t cache_capacity_pages() const {
    return index().cache_capacity_pages();
  }
  std::size_t cache_resident_pages() const {
    return index().cache_resident_pages();
  }
  const PqAdcModel& pq_model() const { return index().pq_model(); }

 private:
  void merge_loop();
  PackedDiskGraphIndex& index() { return runtime_->index(); }
  const PackedDiskGraphIndex& index() const { return runtime_->index(); }
  PackedGraphEngineConfig config_;
  std::shared_ptr<SharedPackedGraphRuntime> runtime_;
  std::shared_ptr<const PackedGraphGeneration> generation_;
  mutable std::mutex config_mutex_;
  mutable std::mutex merge_mutex_;
  std::condition_variable merge_condition_;
  std::thread merge_thread_;
  MergeStatus merge_status_;
  bool merge_requested_ = false;
  std::atomic<bool> merge_stop_{false};
  std::atomic<bool> merge_cancel_{false};
  bool auto_merge_enabled_ = false;
  std::atomic<std::uint32_t> max_known_node_id_{0};
};

}  // namespace agent_aware
