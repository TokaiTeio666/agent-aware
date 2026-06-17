#include <algorithm>
#include <stdexcept>
#include <vector>

#include "agentmem/dynamic/memtable.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

agentmem::DynamicRecord make_record(agentmem::NodeId node_id,
                                    std::uint64_t sequence_id,
                                    std::vector<float> vector) {
  agentmem::DynamicRecord record;
  record.node_id = node_id;
  record.sequence_id = sequence_id;
  record.dim = static_cast<std::uint32_t>(vector.size());
  record.vector = std::move(vector);
  return record;
}

void test_insert_and_get() {
  agentmem::MemTable table(1024);
  const auto record = make_record(7, 1, {1.0f, 2.0f, 3.0f});

  require(table.insert(record), "insert returns true for new record");
  require(table.contains(7), "contains sees inserted id");
  require(table.size() == 1, "size after insert");

  agentmem::DynamicRecord out;
  require(table.get(7, out), "get finds inserted record");
  require(out.node_id == 7, "get node id");
  require(out.sequence_id == 1, "get sequence id");
  require(out.vector == record.vector, "get vector");
}

void test_get_missing() {
  agentmem::MemTable table(1024);
  agentmem::DynamicRecord out;
  require(!table.get(99, out), "get missing returns false");
  require(!table.contains(99), "contains missing returns false");
}

void test_version_overwrite() {
  agentmem::MemTable table(1024);
  require(table.insert(make_record(3, 10, {10.0f})), "insert initial version");
  require(!table.insert(make_record(3, 9, {9.0f})),
          "older version is ignored");

  agentmem::DynamicRecord out;
  require(table.get(3, out), "get after old overwrite attempt");
  require(out.sequence_id == 10, "older version does not replace newer");
  require(out.vector == std::vector<float>{10.0f},
          "older version keeps vector");

  require(table.insert(make_record(3, 11, {11.0f, 12.0f})),
          "newer version replaces old");
  require(table.get(3, out), "get after newer overwrite");
  require(out.sequence_id == 11, "newer version sequence");
  require(out.vector == std::vector<float>({11.0f, 12.0f}),
          "newer version vector");
  require(table.size() == 1, "overwrite keeps one logical record");
}

void test_snapshot_is_stable() {
  agentmem::MemTable table(4096);
  require(table.insert(make_record(1, 1, {1.0f})), "insert before snapshot");
  const auto snapshot = table.snapshot();

  require(table.insert(make_record(2, 1, {2.0f})), "insert after snapshot");
  require(snapshot.size() == 1, "snapshot size stays stable");
  require(snapshot[0].node_id == 1, "snapshot keeps original record");
  require(table.size() == 2, "table sees later insert");
}

void test_clear() {
  agentmem::MemTable table(4096);
  require(table.insert(make_record(1, 1, {1.0f, 2.0f})), "insert before clear");
  require(table.bytes() > 0, "bytes before clear");

  table.clear();
  require(table.size() == 0, "size after clear");
  require(table.bytes() == 0, "bytes after clear");
  require(!table.contains(1), "contains after clear");
}

void test_should_flush() {
  agentmem::MemTable table(1);
  require(!table.should_flush(), "empty table does not reach threshold");
  require(table.insert(make_record(1, 1, {1.0f})), "insert for flush");
  require(table.bytes() >= 1, "bytes after insert");
  require(table.should_flush(), "flush threshold reached");
}

}  // namespace

int main() {
  test_insert_and_get();
  test_get_missing();
  test_version_overwrite();
  test_snapshot_is_stable();
  test_clear();
  test_should_flush();
  return 0;
}
