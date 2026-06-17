#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentmem/dynamic/sstable.h"

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

agentmem::DynamicRecord make_record(agentmem::NodeId node_id,
                                    std::uint64_t sequence_id,
                                    std::vector<float> vector,
                                    std::vector<agentmem::NodeId> neighbors =
                                        {}) {
  agentmem::DynamicRecord record;
  record.sequence_id = sequence_id;
  record.node_id = node_id;
  record.dim = static_cast<std::uint32_t>(vector.size());
  record.vector = std::move(vector);
  record.neighbors = std::move(neighbors);
  return record;
}

void test_write_generates_files_and_reopen_get() {
  const auto dir = test_dir("sstable_basic");
  const std::uint64_t id = 1;
  const std::vector<agentmem::DynamicRecord> records = {
      make_record(10, 1, {1.0f, 2.0f}, {11, 12}),
      make_record(20, 2, {3.0f, 4.0f}, {21}),
  };

  agentmem::dynamic::SSTableWriter writer(dir, id, 0);
  require(writer.write(records), "sstable write succeeds");

  const auto base = base_path(dir, id);
  require(std::filesystem::exists(base.string() + ".data"),
          "data file exists");
  require(std::filesystem::exists(base.string() + ".index"),
          "index file exists");
  require(std::filesystem::exists(base.string() + ".meta"),
          "meta file exists");

  agentmem::dynamic::SSTableReader reader(base);
  require(reader.open(), "sstable reader opens");
  require(reader.id() == id, "reader id");
  require(reader.level() == 0, "reader level");
  require(reader.record_count() == 2, "reader record count");
  require(reader.max_sequence_id() == 2, "reader max sequence");

  agentmem::DynamicRecord out;
  require(reader.get(10, out), "get existing id");
  require(out.node_id == 10, "get node id");
  require(out.sequence_id == 1, "get sequence id");
  require(out.vector == std::vector<float>({1.0f, 2.0f}), "get vector");
  require(out.neighbors == std::vector<agentmem::NodeId>({11, 12}),
          "get neighbors");
}

void test_get_missing_returns_false() {
  const auto dir = test_dir("sstable_missing");
  agentmem::dynamic::SSTableWriter writer(dir, 2, 0);
  require(writer.write({make_record(1, 1, {1.0f})}), "write missing test");

  agentmem::dynamic::SSTableReader reader(base_path(dir, 2));
  require(reader.open(), "open missing test");
  agentmem::DynamicRecord out;
  require(!reader.get(99, out), "missing id returns false");
}

void test_duplicate_node_id_keeps_newest_sequence() {
  const auto dir = test_dir("sstable_duplicate");
  std::vector<agentmem::DynamicRecord> records = {
      make_record(7, 10, {10.0f}),
      make_record(7, 9, {9.0f}),
      make_record(7, 12, {12.0f, 13.0f}),
      make_record(8, 11, {8.0f}),
  };

  agentmem::dynamic::SSTableWriter writer(dir, 3, 1);
  require(writer.write(records), "write duplicate records");

  agentmem::dynamic::SSTableReader reader(base_path(dir, 3));
  require(reader.open(), "open duplicate reader");
  require(reader.level() == 1, "duplicate reader level");
  require(reader.record_count() == 2, "duplicate record count");
  require(reader.max_sequence_id() == 12, "duplicate max sequence");

  agentmem::DynamicRecord out;
  require(reader.get(7, out), "get duplicate id");
  require(out.sequence_id == 12, "duplicate keeps newest sequence");
  require(out.vector == std::vector<float>({12.0f, 13.0f}),
          "duplicate keeps newest vector");
}

void test_scan_all_returns_all_latest_records() {
  const auto dir = test_dir("sstable_scan");
  agentmem::dynamic::SSTableWriter writer(dir, 4, 0);
  require(writer.write({
              make_record(3, 3, {3.0f}),
              make_record(1, 1, {1.0f}),
              make_record(2, 2, {2.0f}),
          }),
          "write scan records");

  agentmem::dynamic::SSTableReader reader(base_path(dir, 4));
  require(reader.open(), "open scan reader");
  const auto records = reader.scan_all();
  require(records.size() == 3, "scan all count");
  require(records[0].node_id == 1, "scan first id sorted");
  require(records[1].node_id == 2, "scan second id sorted");
  require(records[2].node_id == 3, "scan third id sorted");
}

void test_reader_accepts_file_path_as_base() {
  const auto dir = test_dir("sstable_file_path");
  agentmem::dynamic::SSTableWriter writer(dir, 5, 2);
  require(writer.write({make_record(1, 5, {5.0f})}), "write file path test");

  agentmem::dynamic::SSTableReader reader(base_path(dir, 5).string() + ".data");
  require(reader.open(), "reader opens from data file path");
  require(reader.id() == 5, "file path reader id");
  require(reader.level() == 2, "file path reader level");
}

}  // namespace

int main() {
  test_write_generates_files_and_reopen_get();
  test_get_missing_returns_false();
  test_duplicate_node_id_keeps_newest_sequence();
  test_scan_all_returns_all_latest_records();
  test_reader_accepts_file_path_as_base();
  return 0;
}
