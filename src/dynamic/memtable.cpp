#include "agent_aware/dynamic/memtable.h"

#include <mutex>

namespace agent_aware {
namespace {

std::size_t estimate_record_bytes(const DynamicRecord& record) {
  return sizeof(DynamicRecord) + record.vector.size() * sizeof(float) +
         record.neighbors.size() * sizeof(NodeId);
}

}  // namespace

MemTable::MemTable(std::size_t flush_threshold_bytes)
    : flush_threshold_bytes_(flush_threshold_bytes) {}

bool MemTable::insert(const DynamicRecord& record) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto existing = records_.find(record.node_id);
  if (existing != records_.end()) {
    if (record.sequence_id <= existing->second.sequence_id) {
      return false;
    }
    current_bytes_ -= estimate_record_bytes(existing->second);
    existing->second = record;
    current_bytes_ += estimate_record_bytes(existing->second);
    return true;
  }

  const auto inserted = records_.emplace(record.node_id, record);
  current_bytes_ += estimate_record_bytes(inserted.first->second);
  return true;
}

bool MemTable::get(NodeId node_id, DynamicRecord& out) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto found = records_.find(node_id);
  if (found == records_.end()) {
    return false;
  }
  out = found->second;
  return true;
}

bool MemTable::contains(NodeId node_id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return records_.find(node_id) != records_.end();
}

std::vector<DynamicRecord> MemTable::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<DynamicRecord> records;
  records.reserve(records_.size());
  for (const auto& item : records_) {
    records.push_back(item.second);
  }
  return records;
}

void MemTable::clear() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  records_.clear();
  current_bytes_ = 0;
}

std::size_t MemTable::size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return records_.size();
}

std::size_t MemTable::bytes() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return current_bytes_;
}

bool MemTable::should_flush() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return current_bytes_ >= flush_threshold_bytes_;
}

}  // namespace agent_aware
