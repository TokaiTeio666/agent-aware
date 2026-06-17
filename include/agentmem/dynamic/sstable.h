#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "agentmem/dynamic/dynamic_record.h"

namespace agentmem::dynamic {

class SSTableWriter {
 public:
  SSTableWriter(const std::filesystem::path& dir, std::uint64_t sstable_id,
                int level);

  bool write(const std::vector<DynamicRecord>& records);

 private:
  std::filesystem::path dir_;
  std::uint64_t sstable_id_ = 0;
  int level_ = 0;
};

class SSTableReader {
 public:
  explicit SSTableReader(const std::filesystem::path& base_path);

  bool open();
  bool get(NodeId node_id, DynamicRecord& out) const;

  std::vector<DynamicRecord> scan_all() const;

  std::uint64_t id() const;
  int level() const;
  std::size_t record_count() const;
  std::uint64_t max_sequence_id() const;

 private:
  struct IndexEntry {
    std::uint64_t sequence_id = 0;
    std::uint64_t offset = 0;
  };

  std::filesystem::path base_path_;
  std::filesystem::path data_path_;
  std::filesystem::path index_path_;
  std::filesystem::path meta_path_;
  std::unordered_map<NodeId, IndexEntry> index_;
  std::uint64_t id_ = 0;
  int level_ = 0;
  std::size_t record_count_ = 0;
  std::uint64_t min_sequence_id_ = 0;
  std::uint64_t max_sequence_id_ = 0;
  bool opened_ = false;
};

}  // namespace agentmem::dynamic
