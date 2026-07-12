#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "dynamic/dynamic_types.h"

namespace agent_aware::dynamic {

class SSTableWriter {
 public:
  SSTableWriter(const std::filesystem::path& dir, std::uint64_t sstable_id,
                int level);

  bool write(const std::vector<MaterializedRecord>& records);

 private:
  std::filesystem::path dir_;
  std::uint64_t sstable_id_ = 0;
  int level_ = 0;
};

class SSTableReader {
 public:
  explicit SSTableReader(const std::filesystem::path& base_path);

  bool open();

  std::vector<MaterializedRecord> scan_all() const;
  bool read_at(const DynamicLocator& locator, MaterializedRecord& out) const;
  std::vector<std::pair<MaterializedRecord, DynamicLocator>> scan_all_located() const;

  std::uint64_t id() const;
  std::uint64_t max_sequence_id() const;

 private:
  bool get(NodeId node_id, MaterializedRecord& out) const;
  struct IndexEntry {
    NodeId node_id = 0;
    std::uint64_t sequence_id = 0;
    std::uint64_t offset = 0;
  };

  std::filesystem::path base_path_;
  std::filesystem::path data_path_;
  std::filesystem::path index_path_;
  std::filesystem::path meta_path_;
  std::vector<IndexEntry> index_;
  std::uint64_t id_ = 0;
  int level_ = 0;
  std::size_t record_count_ = 0;
  std::uint64_t min_sequence_id_ = 0;
  std::uint64_t max_sequence_id_ = 0;
  bool opened_ = false;
};

}  // namespace agent_aware::dynamic
