#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace agent_aware::dynamic {

struct ManifestSSTableEntry {
  std::uint64_t id = 0;
  int level = 0;
  std::filesystem::path base_path;
};

struct ManifestData {
  std::uint32_t version = 1;
  std::uint64_t next_sequence_id = 1;
  std::uint64_t next_sstable_id = 1;
  std::vector<ManifestSSTableEntry> sstables;
  std::uint64_t wal_checkpoint = 0;
};

class Manifest {
 public:
  explicit Manifest(std::filesystem::path path);

  bool load(ManifestData& data) const;
  bool save(const ManifestData& data) const;

 private:
  std::filesystem::path path_;
};

}  // namespace agent_aware::dynamic
