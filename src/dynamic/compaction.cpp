#include "agent_aware/dynamic/compaction.h"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace agent_aware::dynamic {
namespace {

std::string sstable_name(std::uint64_t sstable_id) {
  std::ostringstream name;
  name << "sst_" << std::setw(6) << std::setfill('0') << sstable_id;
  return name.str();
}

std::vector<DynamicRecord> deduplicate_newest(
    const std::vector<DynamicRecord>& records) {
  std::unordered_map<NodeId, DynamicRecord> latest;
  latest.reserve(records.size());
  for (const auto& record : records) {
    const auto found = latest.find(record.node_id);
    if (found == latest.end() ||
        record.sequence_id > found->second.sequence_id) {
      latest[record.node_id] = record;
    }
  }

  std::vector<DynamicRecord> output;
  output.reserve(latest.size());
  for (const auto& item : latest) {
    output.push_back(item.second);
  }
  std::sort(output.begin(), output.end(),
            [](const DynamicRecord& lhs, const DynamicRecord& rhs) {
              return lhs.node_id < rhs.node_id;
            });
  return output;
}

}  // namespace

CompactionJob::CompactionJob(CompactionInput input)
    : input_(std::move(input)) {}

CompactionResult CompactionJob::run() {
  CompactionResult result;
  result.output_base_path =
      input_.output_dir / sstable_name(input_.output_sstable_id);

  if (input_.input_tables.empty()) {
    return result;
  }

  std::vector<DynamicRecord> records;
  for (const auto& table : input_.input_tables) {
    if (!table) {
      return result;
    }
    auto table_records = table->scan_all();
    result.input_record_count += table_records.size();
    records.insert(records.end(),
                   std::make_move_iterator(table_records.begin()),
                   std::make_move_iterator(table_records.end()));
  }

  auto compacted = deduplicate_newest(records);
  result.output_record_count = compacted.size();

  SSTableWriter writer(input_.output_dir, input_.output_sstable_id,
                       input_.output_level);
  result.success = writer.write(compacted);
  return result;
}

}  // namespace agent_aware::dynamic
