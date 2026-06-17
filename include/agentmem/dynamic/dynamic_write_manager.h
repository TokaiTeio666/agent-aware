#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "agentmem/core/types.h"
#include "agentmem/dynamic/dynamic_record.h"
#include "agentmem/dynamic/manifest.h"
#include "agentmem/dynamic/memtable.h"
#include "agentmem/dynamic/sstable.h"
#include "agentmem/dynamic/wal.h"

namespace agentmem::dynamic {

struct DynamicWriteOptions {
  std::filesystem::path dynamic_dir;
  std::size_t memtable_flush_bytes = 64ULL * 1024ULL * 1024ULL;
  bool enable_wal = true;
  bool enable_auto_flush = true;
};

class DynamicWriteManager {
 public:
  explicit DynamicWriteManager(DynamicWriteOptions options);

  bool open();
  bool insert(NodeId node_id, const float* vector, std::uint32_t dim);
  bool flush();

  bool get(NodeId node_id, DynamicRecord& out) const;

  std::vector<DynamicRecord> collect_all_delta_records() const;

  std::vector<DynamicRecord> search_delta_l2(const float* query,
                                             std::uint32_t dim,
                                             std::size_t topk) const;

  bool close();

 private:
  bool flush_locked();
  bool rotate_wal_locked();
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

}  // namespace agentmem::dynamic
