#pragma once

#include <filesystem>
#include <vector>

#include "dynamic/dynamic_types.h"

namespace agent_aware::dynamic {

class WalWriter {
 public:
  explicit WalWriter(const std::filesystem::path& path,
                     std::uint64_t segment_id = 0);
  ~WalWriter();

  bool append(const MaterializedRecord& record);
  bool append_batch(const std::vector<MaterializedRecord>& records);
  bool append_batch(const std::vector<MaterializedRecord>& records,
                    std::vector<DynamicLocator>& locators);
  bool sync();
  bool close();
  bool valid() const;

 private:
  std::filesystem::path path_;
  int fd_ = -1;
  bool closed_ = false;
  std::uint64_t segment_id_ = 0;
};

class WalReader {
 public:
  explicit WalReader(const std::filesystem::path& path);

  std::vector<MaterializedRecord> replay();
  bool read_at(const DynamicLocator& locator, MaterializedRecord& out) const;

 private:
  std::filesystem::path path_;
};

}  // namespace agent_aware::dynamic
