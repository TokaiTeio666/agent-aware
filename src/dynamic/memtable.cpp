#include "dynamic/memtable.h"

#include <limits>

namespace agent_aware {

MemTable::MemTable(std::size_t flush_threshold_bytes)
    : flush_threshold_bytes_(flush_threshold_bytes) {}

bool MemTable::insert(const CompactDeltaVersion& version) {
  if (version.pq_code.size() > std::numeric_limits<std::uint16_t>::max() ||
      pq_arena_.size() > std::numeric_limits<std::uint32_t>::max() -
                             version.pq_code.size()) {
    return false;
  }
  const auto found = latest_slots_.find(version.node_id);
  if (found != latest_slots_.end() &&
      version.sequence_id <= sequences_[found->second]) {
    return false;
  }
  const auto slot = static_cast<std::uint32_t>(node_ids_.size());
  latest_slots_[version.node_id] = slot;
  node_ids_.push_back(version.node_id);
  sequences_.push_back(version.sequence_id);
  dims_.push_back(version.dim);
  locators_.push_back(version.locator);
  pq_offsets_.push_back(static_cast<std::uint32_t>(pq_arena_.size()));
  pq_lengths_.push_back(static_cast<std::uint16_t>(version.pq_code.size()));
  deleted_.push_back(version.deleted ? 1 : 0);
  pq_arena_.insert(pq_arena_.end(), version.pq_code.begin(),
                   version.pq_code.end());
  return true;
}

CompactDeltaVersion MemTable::at(std::size_t index) const {
  CompactDeltaVersion version;
  version.node_id = node_ids_.at(index);
  version.sequence_id = sequences_[index];
  version.dim = dims_[index];
  version.locator = locators_[index];
  version.deleted = deleted_[index] != 0;
  const auto offset = pq_offsets_[index];
  const auto length = pq_lengths_[index];
  version.pq_code.assign(pq_arena_.begin() + offset,
                         pq_arena_.begin() + offset + length);
  return version;
}

bool MemTable::is_latest(std::size_t index) const {
  const auto found = latest_slots_.find(node_ids_.at(index));
  return found != latest_slots_.end() && found->second == index;
}

void MemTable::clear() {
  latest_slots_.clear();
  node_ids_.clear();
  sequences_.clear();
  dims_.clear();
  locators_.clear();
  pq_offsets_.clear();
  pq_lengths_.clear();
  deleted_.clear();
  pq_arena_.clear();
}

bool MemTable::should_flush() const {
  return resident_bytes() >= flush_threshold_bytes_;
}

std::size_t MemTable::resident_bytes() const {
  const auto directory_bytes = latest_slots_.bucket_count() * sizeof(void*) +
      latest_slots_.size() *
          (sizeof(decltype(latest_slots_)::value_type) + 3 * sizeof(void*));
  return directory_bytes +
         node_ids_.capacity() * sizeof(NodeId) +
         sequences_.capacity() * sizeof(std::uint64_t) +
         dims_.capacity() * sizeof(std::uint32_t) +
         locators_.capacity() * sizeof(DynamicLocator) +
         pq_offsets_.capacity() * sizeof(std::uint32_t) +
         pq_lengths_.capacity() * sizeof(std::uint16_t) + deleted_.capacity() +
         pq_arena_.capacity();
}

std::size_t MemTable::size() const { return node_ids_.size(); }

}  // namespace agent_aware
