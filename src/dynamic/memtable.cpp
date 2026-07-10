#include "agent_aware/dynamic/memtable.h"

#include <mutex>

namespace agent_aware {
namespace {

// MemTable 每个 node 只保留最新记录；完整版本历史由 DynamicWriteManager::recent_records_ 保留给快照读。
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
    // sequence id 单调递增，replay 出来的旧记录或重复记录不能覆盖更新的内存值。
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

bool MemTable::should_flush() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return current_bytes_ >= flush_threshold_bytes_;
}

}  // namespace agent_aware
