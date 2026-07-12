#pragma once

#include <cstdint>
#include <vector>

namespace agent_aware {

using NodeId = std::uint32_t;

enum class DynamicStorageKind : std::uint8_t { kNone = 0, kWal = 1, kSSTable = 2 };

struct DynamicLocator {
  DynamicStorageKind kind = DynamicStorageKind::kNone;
  std::uint64_t file_id = 0;
  std::uint64_t offset = 0;
  std::uint32_t length = 0;
  std::uint32_t checksum = 0;

  bool valid() const { return kind != DynamicStorageKind::kNone; }
};

struct DynamicRecord {
  std::uint64_t sequence_id = 0;
  NodeId node_id = 0;
  std::uint32_t dim = 0;
  std::vector<float> vector;
  std::vector<NodeId> neighbors;
  DynamicLocator locator;
  bool deleted = false;
};

}  // namespace agent_aware
