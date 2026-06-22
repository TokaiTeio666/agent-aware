#pragma once

#include <filesystem>
#include <fstream>
#include <vector>

#include "agent_aware/dynamic/dynamic_record.h"

namespace agent_aware::dynamic {

class WalWriter {
 public:
  explicit WalWriter(const std::filesystem::path& path);
  ~WalWriter();

  bool append(const DynamicRecord& record);
  bool sync();
  bool close();

 private:
  std::filesystem::path path_;
  std::ofstream output_;
  bool closed_ = false;
};

class WalReader {
 public:
  explicit WalReader(const std::filesystem::path& path);

  std::vector<DynamicRecord> replay();

 private:
  std::filesystem::path path_;
};

}  // namespace agent_aware::dynamic
