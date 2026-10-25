#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "dynamic/dynamic_record.h"
#include "dynamic/manifest.h"
#include "dynamic/memtable.h"
#include "dynamic/sstable.h"
#include "dynamic/wal.h"

namespace agent_aware::dynamic {

struct DynamicWriteOptions {
  std::filesystem::path dynamic_dir;
  std::size_t memtable_flush_bytes = 64ULL * 1024ULL * 1024ULL;
  std::size_t dynamic_graph_degree = 16;
  std::size_t write_queue_capacity = 4096;
  std::size_t wal_group_max_records = 64;
  std::size_t wal_group_max_delay_us = 1000;
  std::size_t version_index_shards = 64;
  std::size_t neighbor_candidate_limit = 1024;
  bool enable_wal = true;
  bool enable_auto_flush = true;
  bool enable_background_flush = true;
};

struct DynamicWriteStats {
  std::uint64_t submitted = 0;
  std::uint64_t committed = 0;
  std::uint64_t failed = 0;
  std::uint64_t batches = 0;
  std::uint64_t batch_records = 0;
  std::uint64_t queue_wait_ns = 0;
  std::uint64_t neighbor_prepare_ns = 0;
  std::uint64_t wal_append_ns = 0;
  std::uint64_t wal_sync_ns = 0;
  std::uint64_t memtable_apply_ns = 0;
  std::uint64_t backpressure_events = 0;
  std::uint64_t queue_high_watermark = 0;
  std::uint64_t flush_tasks = 0;
  std::uint64_t flush_failures = 0;
  std::uint64_t flush_queue_high_watermark = 0;
};

struct DynamicSnapshot {
  std::uint64_t read_sequence = 0;
  std::vector<DynamicRecord> records;
  std::size_t deleted_count = 0;
};

struct DynamicCompactionStats {
  bool attempted = false;
  bool success = false;
  std::size_t input_table_count = 0;
  std::size_t input_record_count = 0;
  std::size_t output_record_count = 0;
  std::uint64_t output_sstable_id = 0;
};

class DynamicWriteManager {
 public:
  explicit DynamicWriteManager(DynamicWriteOptions options);
  ~DynamicWriteManager();

  bool open();
  bool insert(NodeId node_id, const float* vector, std::uint32_t dim);
  bool update(NodeId node_id, const float* vector, std::uint32_t dim);
  bool erase(NodeId node_id);
  bool flush();

  std::uint64_t current_sequence() const;
  DynamicWriteStats write_stats() const;
  std::unordered_map<NodeId, DynamicRecord> latest_records_for(
      const std::vector<NodeId>& node_ids, std::uint64_t read_sequence,
      bool include_deleted = true) const;
  DynamicSnapshot snapshot(std::uint64_t read_sequence) const;

  std::vector<DynamicRecord> collect_all_delta_records() const;
  std::vector<DynamicRecord> collect_all_delta_records_at(
      std::uint64_t read_sequence) const;

  std::vector<DynamicRecord> search_delta_l2_at(
      const float* query, std::uint32_t dim, std::size_t topk,
      std::uint64_t read_sequence) const;

  bool compact_once(DynamicCompactionStats* stats = nullptr);

  bool close();

 private:
  enum class RequestKind { mutation, flush, stop };

  struct WriteRequest {
    RequestKind kind = RequestKind::mutation;
    DynamicRecord record;
    std::chrono::steady_clock::time_point submitted_at;
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
    bool success = false;
  };

  struct VersionShard {
    mutable std::shared_mutex mutex;
    std::unordered_map<NodeId, std::vector<DynamicRecord>> versions;
  };

  struct FlushTask {
    std::vector<DynamicRecord> records;
    std::uint64_t max_sequence = 0;
    std::uint64_t wal_segment_id = 0;
    std::shared_ptr<WriteRequest> waiter;
  };

  bool submit_record(DynamicRecord record);
  bool submit_control(RequestKind kind);
  void commit_loop();
  void flush_loop();
  bool commit_batch(const std::vector<std::shared_ptr<WriteRequest>>& batch);
  bool rotate_memtable_locked(std::shared_ptr<WriteRequest> waiter = nullptr);
  bool flush_task(const FlushTask& task);
  void apply_records(const std::vector<DynamicRecord>& records);
  void complete_request(const std::shared_ptr<WriteRequest>& request,
                        bool success);
  void fail_pending_requests();
  void set_failed();
  std::shared_ptr<VersionShard> shard_for(NodeId node_id) const;
  std::vector<NodeId> select_incremental_neighbors(
      NodeId node_id, const float* vector, std::uint32_t dim,
      std::uint64_t read_sequence) const;
  std::vector<DynamicRecord> collect_all_delta_records_at_internal(
      std::uint64_t read_sequence) const;
  std::filesystem::path manifest_path() const;
  std::filesystem::path wal_dir() const;
  std::filesystem::path legacy_wal_path() const;
  std::filesystem::path wal_path(std::uint64_t segment_id) const;
  std::filesystem::path sstable_dir() const;

  DynamicWriteOptions options_;
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex metadata_mutex_;
  ManifestData manifest_;
  std::unique_ptr<MemTable> memtable_;
  std::unique_ptr<WalWriter> wal_writer_;
  std::atomic<std::uint64_t> active_wal_segment_id_{0};
  std::uint64_t active_wal_max_sequence_ = 0;
  std::vector<std::shared_ptr<SSTableReader>> sstables_;
  std::uint64_t metadata_generation_ = 0;

  std::vector<std::shared_ptr<VersionShard>> version_shards_;
  std::atomic<std::uint64_t> latest_sequence_{0};
  std::atomic<std::uint64_t> next_sequence_id_{1};
  std::atomic<std::uint64_t> next_sstable_id_{1};

  std::mutex queue_mutex_;
  std::condition_variable queue_condition_;
  std::condition_variable queue_space_condition_;
  std::deque<std::shared_ptr<WriteRequest>> write_queue_;

  std::mutex flush_queue_mutex_;
  std::condition_variable flush_queue_condition_;
  std::deque<FlushTask> flush_queue_;
  bool flush_stop_ = false;

  std::thread commit_thread_;
  std::thread flush_thread_;
  std::atomic<bool> accepting_writes_{false};
  std::atomic<bool> failed_{false};
  bool opened_ = false;

  std::atomic<std::uint64_t> stat_submitted_{0};
  std::atomic<std::uint64_t> stat_committed_{0};
  std::atomic<std::uint64_t> stat_failed_{0};
  std::atomic<std::uint64_t> stat_batches_{0};
  std::atomic<std::uint64_t> stat_batch_records_{0};
  std::atomic<std::uint64_t> stat_queue_wait_ns_{0};
  std::atomic<std::uint64_t> stat_neighbor_prepare_ns_{0};
  std::atomic<std::uint64_t> stat_wal_append_ns_{0};
  std::atomic<std::uint64_t> stat_wal_sync_ns_{0};
  std::atomic<std::uint64_t> stat_memtable_apply_ns_{0};
  std::atomic<std::uint64_t> stat_backpressure_{0};
  std::atomic<std::uint64_t> stat_queue_high_watermark_{0};
  std::atomic<std::uint64_t> stat_flush_tasks_{0};
  std::atomic<std::uint64_t> stat_flush_failures_{0};
  std::atomic<std::uint64_t> stat_flush_queue_high_watermark_{0};
};

std::vector<SearchResult> merge_base_and_delta_l2(
    const std::vector<SearchResult>& base_results,
    const std::vector<DynamicRecord>& delta_records, const float* query,
    std::uint32_t dim, std::size_t topk);

}  // namespace agent_aware::dynamic
