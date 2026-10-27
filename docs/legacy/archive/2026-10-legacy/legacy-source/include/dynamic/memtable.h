#pragma once

#include <cstddef>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "dynamic/dynamic_record.h"

namespace agent_aware {

class MemTable {
 public:
  explicit MemTable(std::size_t flush_threshold_bytes);

  bool insert(const DynamicRecord& record);
  std::vector<DynamicRecord> snapshot() const;
  void clear();

  bool should_flush() const;

 private:
  std::size_t flush_threshold_bytes_ = 0;
  std::size_t current_bytes_ = 0;
  std::unordered_map<NodeId, DynamicRecord> records_;
  mutable std::shared_mutex mutex_;
};

}  // namespace agent_aware
