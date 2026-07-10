#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "agent_aware/dynamic/sstable.h"

namespace agent_aware::dynamic {

struct CompactionInput {
  std::vector<std::shared_ptr<SSTableReader>> input_tables;
  std::filesystem::path output_dir;
  std::uint64_t output_sstable_id = 0;
  int output_level = 1;
};

struct CompactionResult {
  bool success = false;
  std::filesystem::path output_base_path;
  std::size_t input_record_count = 0;
  std::size_t output_record_count = 0;
};

class CompactionJob {
 public:
  explicit CompactionJob(CompactionInput input);

  CompactionResult run();

 private:
  CompactionInput input_;
};

}  // namespace agent_aware::dynamic
