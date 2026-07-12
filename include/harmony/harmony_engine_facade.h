#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "engine/storage_engine.h"

namespace agent_aware::harmony {

enum class ErrorCode : std::uint32_t {
  kOk = 0,
  kNotInitialized,
  kAlreadyInitialized,
  kBusy,
  kInvalidArgument,
  kDimensionMismatch,
  kIndexNotFound,
  kIndexIncompatible,
  kIndexCorrupted,
  kChecksumMismatch,
  kIoError,
  kWalError,
  kCancelled,
  kResourceExhausted,
  kInternal,
};

enum class WriteMode : std::uint8_t { kInsert = 0, kUpdate = 1 };
enum class ResourceProfile : std::uint8_t {
  kNormal = 0,
  kConstrained = 1,
  kCritical = 2,
};

struct Status {
  ErrorCode code = ErrorCode::kOk;
  std::string message;
  bool retryable = false;
  int native_code = 0;

  bool ok() const { return code == ErrorCode::kOk; }
  static Status Ok() { return {}; }
};

template <typename T>
struct StatusOr {
  Status status;
  T value{};
};

struct HarmonyEngineConfig {
  std::string base_index_path;
  std::string pq_model_path;
  std::string dynamic_dir;
  std::uint32_t expected_dimension = 512;
  std::string cache_policy = "graph-aware-2q";
  std::size_t cache_pages = 0;
  std::string io_mode = "thread_pool_pread";
  std::size_t io_batch_size = 1;
  std::size_t io_depth = 8;
  std::size_t search_width = 64;
  std::size_t beam_width = 0;
  std::size_t entry_count = 1;
  std::size_t rerank_top_k = 0;
  std::string prefetch_policy = "pq";
  std::size_t prefetch_submit_top_k = 4;
  std::size_t prefetch_max_inflight = 8;
  std::uint64_t delta_soft_limit_bytes = 5ULL * 1024ULL * 1024ULL;
  std::uint64_t delta_high_limit_bytes = 6ULL * 1024ULL * 1024ULL;
  std::uint64_t delta_hard_limit_bytes = 7ULL * 1024ULL * 1024ULL;
  std::size_t memtable_flush_records = 4096;
  std::size_t memtable_flush_bytes = 512ULL * 1024ULL;
  std::size_t delta_candidate_top_l = 64;
  bool enable_auto_graph_merge = true;
  std::size_t graph_merge_records = 10000;
  std::uint64_t graph_merge_persisted_bytes = 64ULL * 1024ULL * 1024ULL;
  double graph_merge_delete_ratio = 0.05;
  std::size_t graph_merge_interval_ms = 300000;
  std::size_t graph_merge_max_segments = 8;
};

struct NativeEngineInfo {
  std::string api_version = "1.0";
  std::string engine_version = "0.1.0";
  std::uint32_t dimension = 0;
  std::uint64_t base_vector_count = 0;
  std::uint64_t base_page_count = 0;
  std::uint64_t dynamic_sequence = 0;
  std::string cache_policy;
  std::size_t cache_pages = 0;
  std::string requested_io_mode;
  std::string effective_io_mode;
  std::string io_fallback_reason;
  bool pq_enabled = false;
  std::size_t pq_subspaces = 0;
  std::uint32_t index_format_version = 0;
};

struct EngineMetricsSnapshot {
  DiskGraphIoStatus io;
  DiskGraphSearchStats aggregate_search;
  std::uint64_t dynamic_sequence = 0;
  std::uint64_t total_searches = 0;
  std::uint64_t total_writes = 0;
  std::uint64_t total_deletes = 0;
  std::uint64_t total_flushes = 0;
  std::uint64_t total_io_errors = 0;
  std::size_t cache_capacity_pages = 0;
  std::size_t cache_resident_pages = 0;
  std::size_t memory_budget_bytes = 0;
  std::uint64_t delta_controlled_bytes = 0;
  std::uint64_t base_graph_cache_bytes = 0;
  std::uint64_t merge_workspace_bytes = 0;
  std::uint64_t base_generation = 0;
  std::uint64_t base_sequence = 0;
  std::uint64_t total_graph_merges = 0;
  std::uint64_t failed_graph_merges = 0;
};

class HarmonyEngineFacade {
 public:
  HarmonyEngineFacade() = default;
  ~HarmonyEngineFacade();

  StatusOr<NativeEngineInfo> open(const HarmonyEngineConfig& config);
  StatusOr<EngineSearchResult> search(const float* query, std::uint32_t dim,
                                      std::size_t top_k);
  Status upsert(std::uint32_t id, const float* vector, std::uint32_t dim,
                WriteMode mode);
  Status erase(std::uint32_t id);
  Status set_resource_profile(ResourceProfile profile);
  Status flush();
  Status trigger_merge();
  StatusOr<PackedGraphEngine::MergeStatus> merge_status() const;
  Status cancel_merge();
  StatusOr<EngineMetricsSnapshot> metrics() const;
  Status close();

 private:
  enum class State { kClosed, kOpening, kReady, kClosing, kFailed };
  Status validate_vector(const float* vector, std::uint32_t dim) const;
  static Status from_exception(const std::exception& error);
  void add_search_stats(const DiskGraphSearchStats& stats);

  mutable std::mutex lifecycle_mutex_;
  mutable std::shared_mutex query_boundary_mutex_;
  mutable std::mutex metrics_mutex_;
  State state_ = State::kClosed;
  HarmonyEngineConfig config_;
  ResourceProfile profile_ = ResourceProfile::kNormal;
  std::shared_ptr<dynamic::DynamicWriteManager> dynamic_manager_;
  std::unique_ptr<PackedGraphEngine> engine_;
  EngineMetricsSnapshot metrics_;
};

const char* error_code_name(ErrorCode code);

}  // namespace agent_aware::harmony
