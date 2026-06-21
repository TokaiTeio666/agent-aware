#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent_aware/dynamic/compaction.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path test_dir(const std::string& name) {
  const auto dir = std::filesystem::path("build/dynamic_tests") / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

std::filesystem::path base_path(const std::filesystem::path& dir,
                                std::uint64_t id) {
  char name[32] = {};
  std::snprintf(name, sizeof(name), "sst_%06llu",
                static_cast<unsigned long long>(id));
  return dir / name;
}

bool table_files_exist(const std::filesystem::path& base) {
  return std::filesystem::exists(base.string() + ".data") &&
         std::filesystem::exists(base.string() + ".index") &&
         std::filesystem::exists(base.string() + ".meta");
}

agent_aware::DynamicRecord make_record(agent_aware::NodeId node_id,
                                    std::uint64_t sequence_id,
                                    std::vector<float> vector,
                                    bool deleted = false) {
  agent_aware::DynamicRecord record;
  record.sequence_id = sequence_id;
  record.node_id = node_id;
  record.dim = static_cast<std::uint32_t>(vector.size());
  record.vector = std::move(vector);
  record.deleted = deleted;
  return record;
}

std::shared_ptr<agent_aware::dynamic::SSTableReader> write_and_open(
    const std::filesystem::path& dir, std::uint64_t id,
    const std::vector<agent_aware::DynamicRecord>& records) {
  agent_aware::dynamic::SSTableWriter writer(dir, id, 0);
  require(writer.write(records), "write input sstable");

  auto reader =
      std::make_shared<agent_aware::dynamic::SSTableReader>(base_path(dir, id));
  require(reader->open(), "open input sstable");
  return reader;
}

void test_two_sstables_merge_to_one() {
  const auto dir = test_dir("compaction_merge");
  const auto first =
      write_and_open(dir, 1, {make_record(1, 1, {1.0f}),
                              make_record(2, 2, {2.0f})});
  const auto second =
      write_and_open(dir, 2, {make_record(3, 3, {3.0f}),
                              make_record(4, 4, {4.0f})});
  const auto first_base = base_path(dir, 1);
  const auto second_base = base_path(dir, 2);

  agent_aware::dynamic::CompactionInput input;
  input.input_tables = {first, second};
  input.output_dir = dir;
  input.output_sstable_id = 100;
  input.output_level = 1;

  const auto result = agent_aware::dynamic::CompactionJob(input).run();
  require(result.success, "compaction succeeds");
  require(result.input_record_count == 4, "input record count");
  require(result.output_record_count == 4, "output record count");
  require(result.output_base_path == base_path(dir, 100), "output base path");
  require(table_files_exist(result.output_base_path), "output files exist");

  agent_aware::dynamic::SSTableReader output(result.output_base_path);
  require(output.open(), "open output sstable");
  require(output.level() == 1, "output level");
  require(output.record_count() == 4, "output record count from meta");

  agent_aware::DynamicRecord out;
  require(output.get(3, out), "output get existing id");
  require(out.sequence_id == 3, "output get sequence");
  require(table_files_exist(first_base), "first input still exists");
  require(table_files_exist(second_base), "second input still exists");
}

void test_duplicate_node_id_keeps_newest_sequence() {
  const auto dir = test_dir("compaction_duplicate");
  const auto first =
      write_and_open(dir, 1, {make_record(7, 10, {10.0f}),
                              make_record(8, 11, {8.0f})});
  const auto second =
      write_and_open(dir, 2, {make_record(7, 12, {12.0f, 13.0f}),
                              make_record(9, 9, {9.0f})});

  agent_aware::dynamic::CompactionInput input;
  input.input_tables = {first, second};
  input.output_dir = dir;
  input.output_sstable_id = 200;
  input.output_level = 1;

  const auto result = agent_aware::dynamic::CompactionJob(input).run();
  require(result.success, "duplicate compaction succeeds");
  require(result.input_record_count == 4, "duplicate input record count");
  require(result.output_record_count == 3, "duplicate output record count");

  agent_aware::dynamic::SSTableReader output(result.output_base_path);
  require(output.open(), "open duplicate output");
  agent_aware::DynamicRecord out;
  require(output.get(7, out), "get duplicate id from output");
  require(out.sequence_id == 12, "duplicate keeps newest sequence");
  require(out.vector == std::vector<float>({12.0f, 13.0f}),
          "duplicate keeps newest vector");
  require(output.get(8, out), "non duplicate first table kept");
  require(output.get(9, out), "non duplicate second table kept");
}

void test_deleted_record_is_preserved_when_newest() {
  const auto dir = test_dir("compaction_tombstone");
  const auto first =
      write_and_open(dir, 1, {make_record(5, 5, {5.0f}, false)});
  const auto second =
      write_and_open(dir, 2, {make_record(5, 6, {5.0f}, true)});

  agent_aware::dynamic::CompactionInput input;
  input.input_tables = {first, second};
  input.output_dir = dir;
  input.output_sstable_id = 300;
  input.output_level = 1;

  const auto result = agent_aware::dynamic::CompactionJob(input).run();
  require(result.success, "tombstone compaction succeeds");
  require(result.output_record_count == 1, "tombstone output count");

  agent_aware::dynamic::SSTableReader output(result.output_base_path);
  require(output.open(), "open tombstone output");
  agent_aware::DynamicRecord out;
  require(output.get(5, out), "get tombstone output");
  require(out.sequence_id == 6, "tombstone newest sequence");
  require(out.deleted, "newest tombstone is preserved");
}

}  // namespace

int main() {
  test_two_sstables_merge_to_one();
  test_duplicate_node_id_keeps_newest_sequence();
  test_deleted_record_is_preserved_when_newest();
  return 0;
}
