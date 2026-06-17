#pragma once

#include <cstdint>
#include <vector>

namespace agentmem {

using NodeId = std::uint32_t;

struct DynamicRecord {
  std::uint64_t sequence_id = 0;
  NodeId node_id = 0;
  std::uint32_t dim = 0;
  std::vector<float> vector;
  std::vector<NodeId> neighbors;
  bool deleted = false;
};

}  // namespace agentmem
