#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "agent_aware/core/types.h"
#include "agent_aware/dynamic/dynamic_record.h"
#include "agent_aware/dynamic/manifest.h"
#include "agent_aware/dynamic/memtable.h"
#include "agent_aware/dynamic/sstable.h"
#include "agent_aware/dynamic/wal.h"

namespace agent_aware::dynamic {

struct DynamicWriteOptions {
  std::filesystem::path dynamic_dir;
  std::size_t memtable_flush_bytes = 64ULL * 1024ULL * 1024ULL;
  std::size_t dynamic_graph_degree = 16;
  bool enable_wal = true;
  bool enable_auto_flush = true;
};

class DynamicWriteManager {
 public:
  explicit DynamicWriteManager(DynamicWriteOptions options);

  bool open();
  bool insert(NodeId node_id, const float* vector, std::uint32_t dim);
  bool update(NodeId node_id, const float* vector, std::uint32_t dim);
  bool erase(NodeId node_id);
  bool flush();

  bool get(NodeId node_id, DynamicRecord& out) const;

  std::vector<DynamicRecord> collect_all_delta_records() const;

  std::vector<DynamicRecord> search_delta_l2(const float* query,
                                             std::uint32_t dim,
                                             std::size_t topk) const;

  bool close();

 private:
  bool append_record_locked(DynamicRecord record);
  bool flush_locked();
  bool rotate_wal_locked();
  std::vector<NodeId> select_incremental_neighbors_locked(
      NodeId node_id, const float* vector, std::uint32_t dim) const;
  std::vector<DynamicRecord> collect_all_delta_records_locked() const;
  std::filesystem::path manifest_path() const;
  std::filesystem::path wal_dir() const;
  std::filesystem::path wal_path() const;
  std::filesystem::path sstable_dir() const;

  DynamicWriteOptions options_;
  mutable std::mutex mutex_;
  ManifestData manifest_;
  std::unique_ptr<MemTable> memtable_;
  std::unique_ptr<WalWriter> wal_writer_;
  std::vector<std::shared_ptr<SSTableReader>> sstables_;
  bool opened_ = false;
};

std::vector<SearchResult> merge_base_and_delta_l2(
    const std::vector<SearchResult>& base_results,
    const std::vector<DynamicRecord>& delta_records, const float* query,
    std::uint32_t dim, std::size_t topk);

}  // namespace agent_aware::dynamic
