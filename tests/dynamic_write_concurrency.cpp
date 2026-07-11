#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>
#include <vector>

#include "dynamic/dynamic_write_manager.h"
#include "dynamic/wal.h"

namespace {

std::vector<float> vector_for(std::uint32_t value) {
  return {static_cast<float>(value), static_cast<float>(value + 1),
          static_cast<float>(value + 2), static_cast<float>(value + 3)};
}

agent_aware::dynamic::DynamicWriteOptions options_for(
    const std::filesystem::path& path) {
  agent_aware::dynamic::DynamicWriteOptions options;
  options.dynamic_dir = path;
  options.memtable_flush_bytes = 4096;
  options.dynamic_graph_degree = 0;
  options.write_queue_capacity = 128;
  options.wal_group_max_records = 16;
  options.wal_group_max_delay_us = 2000;
  options.version_index_shards = 8;
  options.enable_wal = true;
  options.enable_auto_flush = true;
  options.enable_background_flush = true;
  return options;
}

void test_concurrent_crud_and_recovery(const std::filesystem::path& root) {
  const auto path = root / "concurrent";
  std::filesystem::remove_all(path);
  agent_aware::dynamic::DynamicWriteManager manager(options_for(path));
  assert(manager.open());

  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kRecordsPerThread = 100;
  std::array<std::thread, kThreads> workers;
  for (std::size_t thread_id = 0; thread_id < kThreads; ++thread_id) {
    workers[thread_id] = std::thread([&, thread_id] {
      for (std::size_t i = 0; i < kRecordsPerThread; ++i) {
        const auto id = static_cast<agent_aware::NodeId>(
            thread_id * kRecordsPerThread + i);
        const auto vector = vector_for(id);
        assert(manager.insert(id, vector.data(), vector.size()));
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto updated = vector_for(9999);
  assert(manager.update(7, updated.data(), updated.size()));
  assert(manager.erase(8));
  const auto committed_sequence = manager.current_sequence();
  assert(committed_sequence == kThreads * kRecordsPerThread + 2);
  assert(manager.flush());

  const auto stats = manager.write_stats();
  assert(stats.submitted == kThreads * kRecordsPerThread + 2);
  assert(stats.committed == stats.submitted);
  assert(stats.failed == 0);
  assert(stats.batches > 0);
  assert(stats.batch_records == stats.committed);
  assert(manager.close());

  agent_aware::dynamic::DynamicWriteManager recovered(options_for(path));
  assert(recovered.open());
  assert(recovered.current_sequence() == committed_sequence);
  const auto records = recovered.collect_all_delta_records();
  assert(records.size() == kThreads * kRecordsPerThread);
  const auto seven = recovered.latest_records_for({7}, committed_sequence);
  assert(seven.at(7).vector == updated);
  const auto eight = recovered.latest_records_for({8}, committed_sequence);
  assert(eight.at(8).deleted);

  std::unordered_set<std::uint64_t> sequences;
  const auto snapshot = recovered.snapshot(committed_sequence);
  for (const auto& record : snapshot.records) {
    assert(record.sequence_id > 0);
    sequences.insert(record.sequence_id);
  }
  assert(sequences.size() == snapshot.records.size());
  assert(recovered.close());
}

void test_wal_torn_tail(const std::filesystem::path& root) {
  const auto path = root / "torn.log";
  std::filesystem::create_directories(root);
  std::filesystem::remove(path);
  agent_aware::dynamic::WalWriter writer(path);
  assert(writer.valid());
  std::vector<agent_aware::DynamicRecord> records;
  for (std::uint32_t i = 1; i <= 3; ++i) {
    agent_aware::DynamicRecord record;
    record.sequence_id = i;
    record.node_id = i;
    record.dim = 4;
    record.vector = vector_for(i);
    records.push_back(std::move(record));
  }
  assert(writer.append_batch(records));
  assert(writer.sync());
  assert(writer.close());
  {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    const std::array<char, 7> torn = {'b', 'a', 'd', 't', 'a', 'i', 'l'};
    output.write(torn.data(), torn.size());
  }
  const auto replayed = agent_aware::dynamic::WalReader(path).replay();
  assert(replayed.size() == records.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    assert(replayed[i].sequence_id == records[i].sequence_id);
  }
}

void test_legacy_wal_recovery(const std::filesystem::path& root) {
  const auto path = root / "legacy";
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path / "wal");
  agent_aware::DynamicRecord record;
  record.sequence_id = 17;
  record.node_id = 42;
  record.dim = 4;
  record.vector = vector_for(42);
  agent_aware::dynamic::WalWriter writer(path / "wal" / "wal.log");
  assert(writer.append(record));
  assert(writer.close());

  auto options = options_for(path);
  options.enable_auto_flush = false;
  agent_aware::dynamic::DynamicWriteManager manager(options);
  assert(manager.open());
  assert(manager.current_sequence() == 17);
  const auto latest = manager.latest_records_for({42}, 17);
  assert(latest.at(42).vector == record.vector);
  assert(manager.close());
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "agent_aware_dynamic_write_concurrency";
  std::filesystem::remove_all(root);
  test_concurrent_crud_and_recovery(root);
  test_wal_torn_tail(root);
  test_legacy_wal_recovery(root);
  std::filesystem::remove_all(root);
  return 0;
}
