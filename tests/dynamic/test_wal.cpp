#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentmem/dynamic/wal.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path test_path(const std::string& name) {
  std::filesystem::create_directories("build/dynamic_tests");
  const auto path = std::filesystem::path("build/dynamic_tests") / name;
  std::filesystem::remove(path);
  return path;
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

void append_bytes(const std::filesystem::path& path,
                  const std::vector<unsigned char>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::app);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(output), "append raw bytes");
}

void overwrite_last_byte(const std::filesystem::path& path) {
  auto size = std::filesystem::file_size(path);
  require(size > 0, "file has bytes to corrupt");
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(static_cast<std::streamoff>(size - 1));
  char value = 0;
  file.read(&value, 1);
  value ^= static_cast<char>(0x7f);
  file.seekp(static_cast<std::streamoff>(size - 1));
  file.write(&value, 1);
  require(static_cast<bool>(file), "overwrite last byte");
}

void test_append_one_replay_one() {
  const auto path = test_path("one.wal");
  const auto record = make_record(5, 10, {1.0f, 2.0f}, {7, 8});

  agentmem::dynamic::WalWriter writer(path);
  require(writer.append(record), "append one record");
  require(writer.close(), "close one record wal");

  const auto records = agentmem::dynamic::WalReader(path).replay();
  require(records.size() == 1, "replay one record count");
  require(records[0].node_id == 5, "replay node id");
  require(records[0].sequence_id == 10, "replay sequence id");
  require(records[0].dim == 2, "replay dim");
  require(records[0].vector == std::vector<float>({1.0f, 2.0f}),
          "replay vector");
  require(records[0].neighbors == std::vector<agentmem::NodeId>({7, 8}),
          "replay neighbors");
}

void test_append_multiple_replay_order() {
  const auto path = test_path("multiple.wal");

  agentmem::dynamic::WalWriter writer(path);
  require(writer.append(make_record(1, 1, {1.0f})), "append first");
  require(writer.append(make_record(2, 2, {2.0f, 3.0f})), "append second");
  require(writer.append(make_record(3, 3, {4.0f})), "append third");
  require(writer.close(), "close multiple wal");

  const auto records = agentmem::dynamic::WalReader(path).replay();
  require(records.size() == 3, "replay multiple count");
  require(records[0].node_id == 1, "first replay id");
  require(records[1].node_id == 2, "second replay id");
  require(records[2].node_id == 3, "third replay id");
}

void test_empty_wal() {
  const auto path = test_path("empty.wal");
  {
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "create empty wal");
  }

  const auto records = agentmem::dynamic::WalReader(path).replay();
  require(records.empty(), "empty wal replay is empty");
}

void test_missing_wal() {
  const auto path = test_path("missing.wal");
  std::filesystem::remove(path);

  const auto records = agentmem::dynamic::WalReader(path).replay();
  require(records.empty(), "missing wal replay is empty");
}

void test_broken_tail_stops_after_complete_records() {
  const auto path = test_path("broken_tail.wal");

  agentmem::dynamic::WalWriter writer(path);
  require(writer.append(make_record(1, 1, {1.0f})), "append before tail");
  require(writer.close(), "close before tail corruption");
  append_bytes(path, {0x50, 0x4c});

  const auto records = agentmem::dynamic::WalReader(path).replay();
  require(records.size() == 1, "broken tail keeps complete records");
  require(records[0].node_id == 1, "broken tail first id");
}

void test_checksum_error_stops_without_crash() {
  const auto path = test_path("checksum.wal");

  agentmem::dynamic::WalWriter writer(path);
  require(writer.append(make_record(1, 1, {1.0f})), "append valid record");
  require(writer.append(make_record(2, 2, {2.0f})), "append corrupt target");
  require(writer.close(), "close checksum wal");
  overwrite_last_byte(path);

  const auto records = agentmem::dynamic::WalReader(path).replay();
  require(records.size() == 1, "checksum error keeps previous valid records");
  require(records[0].node_id == 1, "checksum error previous id");
}

void test_append_is_non_destructive() {
  const auto path = test_path("append_mode.wal");
  {
    agentmem::dynamic::WalWriter writer(path);
    require(writer.append(make_record(1, 1, {1.0f})), "append initial");
    require(writer.close(), "close initial");
  }
  {
    agentmem::dynamic::WalWriter writer(path);
    require(writer.append(make_record(2, 2, {2.0f})), "append later");
    require(writer.close(), "close later");
  }

  const auto records = agentmem::dynamic::WalReader(path).replay();
  require(records.size() == 2, "append writer does not truncate");
  require(records[0].node_id == 1, "append mode first id");
  require(records[1].node_id == 2, "append mode second id");
}

}  // namespace

int main() {
  test_append_one_replay_one();
  test_append_multiple_replay_order();
  test_empty_wal();
  test_missing_wal();
  test_broken_tail_stops_after_complete_records();
  test_checksum_error_stops_without_crash();
  test_append_is_non_destructive();
  return 0;
}
