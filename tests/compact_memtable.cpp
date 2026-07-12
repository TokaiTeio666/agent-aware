#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

#include "dynamic/memtable.h"

using namespace agent_aware;

CompactDeltaVersion version(std::uint64_t sequence, NodeId node,
                            std::vector<std::uint8_t> code,
                            bool deleted = false) {
  CompactDeltaVersion value;
  value.sequence_id = sequence;
  value.node_id = node;
  value.dim = 16;
  value.locator = {DynamicStorageKind::kWal, 3, sequence * 100, 64, 0};
  value.pq_code = std::move(code);
  value.deleted = deleted;
  return value;
}

int main() {
  auto active = std::make_unique<MemTable>(1 << 20);
  assert(active->insert(version(1, 7, {1, 2, 3, 4})));
  assert(active->insert(version(2, 8, {5, 6, 7, 8})));
  assert(active->insert(version(3, 7, {9, 10, 11, 12})));
  assert(!active->insert(version(2, 7, {0})));
  assert(active->size() == 3);
  assert(!active->is_latest(0));
  assert(active->is_latest(1));
  assert(active->is_latest(2));
  assert(active->at(2).pq_code == std::vector<std::uint8_t>({9, 10, 11, 12}));
  assert(active->resident_bytes() < 4096);

  auto frozen = std::move(active);
  active = std::make_unique<MemTable>(1 << 20);
  assert(frozen->size() == 3);
  assert(active->size() == 0);
  assert(frozen->at(1).node_id == 8);
  return 0;
}
