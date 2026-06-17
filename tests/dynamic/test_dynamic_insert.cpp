#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentmem/dynamic/dynamic_write_manager.h"

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

agentmem::dynamic::DynamicWriteOptions options_for(
    const std::filesystem::path& dir) {
  agentmem::dynamic::DynamicWriteOptions options;
  options.dynamic_dir = dir;
  options.memtable_flush_bytes = 1024 * 1024;
  options.enable_wal = true;
  options.enable_auto_flush = false;
  return options;
}

void test_insert_then_get_without_restart() {
  auto manager = agentmem::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_insert_get")));
  require(manager.open(), "open manager");

  const float vector[] = {1.0f, 2.0f};
  require(manager.insert(10, vector, 2), "insert dynamic record");

  agentmem::DynamicRecord out;
  require(manager.get(10, out), "get inserted record");
  require(out.node_id == 10, "get node id");
  require(out.sequence_id == 1, "get sequence id");
  require(out.vector == std::vector<float>({1.0f, 2.0f}), "get vector");
  require(manager.close(), "close manager");
}

void test_restart_replays_wal() {
  const auto dir = test_dir("dynamic_wal_replay");
  {
    auto manager = agentmem::dynamic::DynamicWriteManager(options_for(dir));
    require(manager.open(), "open replay writer manager");
    const float vector[] = {3.0f, 4.0f};
    require(manager.insert(11, vector, 2), "insert before replay");
    require(manager.close(), "close replay writer manager");
  }
  {
    auto manager = agentmem::dynamic::DynamicWriteManager(options_for(dir));
    require(manager.open(), "open replay reader manager");
    agentmem::DynamicRecord out;
    require(manager.get(11, out), "replayed record is visible");
    require(out.sequence_id == 1, "replayed sequence id");
    require(out.vector == std::vector<float>({3.0f, 4.0f}),
            "replayed vector");
    require(manager.close(), "close replay reader manager");
  }
}

void test_flush_keeps_record_readable() {
  const auto dir = test_dir("dynamic_flush");
  auto manager = agentmem::dynamic::DynamicWriteManager(options_for(dir));
  require(manager.open(), "open flush manager");

  const float vector[] = {5.0f, 6.0f};
  require(manager.insert(12, vector, 2), "insert before flush");
  require(manager.flush(), "flush memtable");

  agentmem::DynamicRecord out;
  require(manager.get(12, out), "get flushed record");
  require(out.sequence_id == 1, "flushed sequence id");
  require(std::filesystem::exists(dir / "sstable" / "sst_000001.data"),
          "flush data file exists");
  require(std::filesystem::exists(dir / "manifest.json"),
          "flush manifest exists");
  require(std::filesystem::exists(dir / "wal" / "wal.log"),
          "flush rotates wal");
  require(std::filesystem::file_size(dir / "wal" / "wal.log") == 0,
          "rotated wal is empty");
  require(manager.close(), "close flush manager");
}

void test_newer_node_version_wins() {
  auto manager = agentmem::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_version")));
  require(manager.open(), "open version manager");

  const float old_vector[] = {1.0f, 1.0f};
  const float new_vector[] = {2.0f, 2.0f};
  require(manager.insert(42, old_vector, 2), "insert old version");
  require(manager.insert(42, new_vector, 2), "insert new version");

  agentmem::DynamicRecord out;
  require(manager.get(42, out), "get overwritten id");
  require(out.sequence_id == 2, "new version sequence wins");
  require(out.vector == std::vector<float>({2.0f, 2.0f}),
          "new version vector wins");
  require(manager.close(), "close version manager");
}

void test_search_delta_l2_finds_inserted_vector() {
  auto manager = agentmem::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_search_delta")));
  require(manager.open(), "open search delta manager");

  const float far_vector[] = {10.0f, 10.0f};
  const float near_vector[] = {0.1f, 0.0f};
  const float query[] = {0.0f, 0.0f};
  require(manager.insert(100, far_vector, 2), "insert far vector");
  require(manager.insert(101, near_vector, 2), "insert near vector");

  const auto results = manager.search_delta_l2(query, 2, 1);
  require(results.size() == 1, "delta search top1 count");
  require(results[0].node_id == 101, "delta search nearest id");
  require(manager.close(), "close search delta manager");
}

void test_base_and_delta_merge() {
  auto manager = agentmem::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_merge")));
  require(manager.open(), "open merge manager");

  const float delta_vector[] = {0.0f, 0.05f};
  const float query[] = {0.0f, 0.0f};
  require(manager.insert(500, delta_vector, 2), "insert merge delta vector");

  const std::vector<agentmem::SearchResult> base_results = {
      agentmem::SearchResult{1, 0.25f},
      agentmem::SearchResult{2, 0.50f},
  };
  const auto delta_records = manager.search_delta_l2(query, 2, 2);
  const auto merged = agentmem::dynamic::merge_base_and_delta_l2(
      base_results, delta_records, query, 2, 2);

  require(merged.size() == 2, "merged topk count");
  require(merged[0].id == 500, "merged result includes nearest delta first");
  require(merged[1].id == 1, "merged result keeps best base result");
  require(manager.close(), "close merge manager");
}

void test_flush_then_restart_loads_sstable() {
  const auto dir = test_dir("dynamic_flush_restart");
  {
    auto manager = agentmem::dynamic::DynamicWriteManager(options_for(dir));
    require(manager.open(), "open flush restart writer");
    const float vector[] = {7.0f, 8.0f};
    require(manager.insert(77, vector, 2), "insert before flush restart");
    require(manager.flush(), "flush before restart");
    require(manager.close(), "close flush restart writer");
  }
  {
    auto manager = agentmem::dynamic::DynamicWriteManager(options_for(dir));
    require(manager.open(), "open flush restart reader");
    agentmem::DynamicRecord out;
    require(manager.get(77, out), "sstable record visible after restart");
    require(out.vector == std::vector<float>({7.0f, 8.0f}),
            "sstable restart vector");
    require(manager.close(), "close flush restart reader");
  }
}

}  // namespace

int main() {
  test_insert_then_get_without_restart();
  test_restart_replays_wal();
  test_flush_keeps_record_readable();
  test_newer_node_version_wins();
  test_search_delta_l2_finds_inserted_vector();
  test_base_and_delta_merge();
  test_flush_then_restart_loads_sstable();
  return 0;
}
