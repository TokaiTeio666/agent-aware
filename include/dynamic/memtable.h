#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "dynamic/dynamic_types.h"

namespace agent_aware {

class MemTable {
 public:
  explicit MemTable(std::size_t flush_threshold_bytes);

  bool insert(const CompactDeltaVersion& version);
  CompactDeltaVersion at(std::size_t index) const;
  bool is_latest(std::size_t index) const;
  void clear();

  bool should_flush() const;
  std::size_t resident_bytes() const;
  std::size_t size() const;

 private:
  std::size_t flush_threshold_bytes_ = 0;
  std::unordered_map<NodeId, std::uint32_t> latest_slots_;
  std::vector<NodeId> node_ids_;
  std::vector<std::uint64_t> sequences_;
  std::vector<std::uint32_t> dims_;
  std::vector<DynamicLocator> locators_;
  std::vector<std::uint32_t> pq_offsets_;
  std::vector<std::uint16_t> pq_lengths_;
  std::vector<std::uint8_t> deleted_;
  std::vector<std::uint8_t> pq_arena_;
};

}  // namespace agent_aware
