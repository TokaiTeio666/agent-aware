#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent_aware/dynamic/dynamic_write_manager.h"

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

agent_aware::dynamic::DynamicWriteOptions options_for(
    const std::filesystem::path& dir) {
  agent_aware::dynamic::DynamicWriteOptions options;
  options.dynamic_dir = dir;
  options.memtable_flush_bytes = 1024 * 1024;
  options.enable_wal = true;
  options.enable_auto_flush = false;
  return options;
}

void test_insert_then_get_without_restart() {
  auto manager = agent_aware::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_insert_get")));
  require(manager.open(), "open manager");

  const float vector[] = {1.0f, 2.0f};
  require(manager.insert(10, vector, 2), "insert dynamic record");

  agent_aware::DynamicRecord out;
  require(manager.get(10, out), "get inserted record");
  require(out.node_id == 10, "get node id");
  require(out.sequence_id == 1, "get sequence id");
  require(out.vector == std::vector<float>({1.0f, 2.0f}), "get vector");
  require(manager.close(), "close manager");
}

void test_restart_replays_wal() {
  const auto dir = test_dir("dynamic_wal_replay");
  {
    auto manager = agent_aware::dynamic::DynamicWriteManager(options_for(dir));
    require(manager.open(), "open replay writer manager");
    const float vector[] = {3.0f, 4.0f};
    require(manager.insert(11, vector, 2), "insert before replay");
    require(manager.close(), "close replay writer manager");
  }
  {
    auto manager = agent_aware::dynamic::DynamicWriteManager(options_for(dir));
    require(manager.open(), "open replay reader manager");
    agent_aware::DynamicRecord out;
    require(manager.get(11, out), "replayed record is visible");
    require(out.sequence_id == 1, "replayed sequence id");
    require(out.vector == std::vector<float>({3.0f, 4.0f}),
            "replayed vector");
    require(manager.close(), "close replay reader manager");
  }
}

void test_flush_keeps_record_readable() {
  const auto dir = test_dir("dynamic_flush");
  auto manager = agent_aware::dynamic::DynamicWriteManager(options_for(dir));
  require(manager.open(), "open flush manager");

  const float vector[] = {5.0f, 6.0f};
  require(manager.insert(12, vector, 2), "insert before flush");
  require(manager.flush(), "flush memtable");

  agent_aware::DynamicRecord out;
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
  auto manager = agent_aware::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_version")));
  require(manager.open(), "open version manager");

  const float old_vector[] = {1.0f, 1.0f};
  const float new_vector[] = {2.0f, 2.0f};
  require(manager.insert(42, old_vector, 2), "insert old version");
  require(manager.insert(42, new_vector, 2), "insert new version");

  agent_aware::DynamicRecord out;
  require(manager.get(42, out), "get overwritten id");
  require(out.sequence_id == 2, "new version sequence wins");
  require(out.vector == std::vector<float>({2.0f, 2.0f}),
          "new version vector wins");
  require(manager.close(), "close version manager");
}

void test_update_and_erase_api() {
  auto manager = agent_aware::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_update_erase")));
  require(manager.open(), "open update erase manager");

  const float first[] = {1.0f, 1.0f};
  const float second[] = {2.0f, 2.0f};
  require(manager.insert(64, first, 2), "insert before update");
  require(manager.update(64, second, 2), "update dynamic record");

  agent_aware::DynamicRecord out;
  require(manager.get(64, out), "get updated record");
  require(out.sequence_id == 2, "update advances sequence");
  require(out.vector == std::vector<float>({2.0f, 2.0f}),
          "update vector wins");
  require(manager.erase(64), "erase dynamic record");
  require(!manager.get(64, out), "erased record is hidden");
  require(manager.close(), "close update erase manager");
}

void test_insert_records_incremental_neighbors() {
  agent_aware::dynamic::DynamicWriteOptions options =
      options_for(test_dir("dynamic_incremental_neighbors"));
  options.dynamic_graph_degree = 1;
  auto manager = agent_aware::dynamic::DynamicWriteManager(options);
  require(manager.open(), "open neighbor manager");

  const float first[] = {0.0f, 0.0f};
  const float second[] = {0.1f, 0.0f};
  require(manager.insert(1, first, 2), "insert first neighbor seed");
  require(manager.insert(2, second, 2), "insert second with neighbor");

  agent_aware::DynamicRecord out;
  require(manager.get(2, out), "get second record");
  require(out.neighbors.size() == 1, "incremental neighbor count");
  require(out.neighbors[0] == 1, "incremental neighbor id");
  require(manager.close(), "close neighbor manager");
}

void test_search_delta_l2_finds_inserted_vector() {
  auto manager = agent_aware::dynamic::DynamicWriteManager(
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
  auto manager = agent_aware::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_merge")));
  require(manager.open(), "open merge manager");

  const float delta_vector[] = {0.0f, 0.05f};
  const float query[] = {0.0f, 0.0f};
  require(manager.insert(500, delta_vector, 2), "insert merge delta vector");

  const std::vector<agent_aware::SearchResult> base_results = {
      agent_aware::SearchResult{1, 0.25f},
      agent_aware::SearchResult{2, 0.50f},
  };
  const auto delta_records = manager.search_delta_l2(query, 2, 2);
  const auto merged = agent_aware::dynamic::merge_base_and_delta_l2(
      base_results, delta_records, query, 2, 2);

  require(merged.size() == 2, "merged topk count");
  require(merged[0].id == 500, "merged result includes nearest delta first");
  require(merged[1].id == 1, "merged result keeps best base result");
  require(manager.close(), "close merge manager");
}

void test_sequence_snapshot_and_latest_record() {
  auto manager = agent_aware::dynamic::DynamicWriteManager(
      options_for(test_dir("dynamic_sequence_snapshot")));
  require(manager.open(), "open sequence manager");

  const float first[] = {1.0f, 0.0f};
  const float second[] = {2.0f, 0.0f};
  const float query[] = {1.0f, 0.0f};
  require(manager.insert(90, first, 2), "insert snapshot first");
  const auto seq_first = manager.current_sequence();
  require(manager.update(90, second, 2), "update snapshot second");
  const auto seq_second = manager.current_sequence();
  require(manager.erase(90), "erase snapshot record");
  const auto seq_delete = manager.current_sequence();

  const auto first_record = manager.latest_record(90, seq_first, true);
  require(first_record.has_value(), "latest first exists");
  require(first_record->sequence_id == seq_first, "latest first sequence");
  require(first_record->vector == std::vector<float>({1.0f, 0.0f}),
          "latest first vector");

  const auto second_record = manager.latest_record(90, seq_second, true);
  require(second_record.has_value(), "latest second exists");
  require(second_record->sequence_id == seq_second, "latest second sequence");
  require(second_record->vector == std::vector<float>({2.0f, 0.0f}),
          "latest second vector");

  const auto deleted_record = manager.latest_record(90, seq_delete, true);
  require(deleted_record.has_value(), "latest delete exists");
  require(deleted_record->deleted, "latest delete tombstone");
  require(!manager.latest_record(90, seq_delete, false).has_value(),
          "latest delete hidden when deleted excluded");

  const auto before_delete = manager.search_delta_l2_at(query, 2, 1, seq_first);
  require(before_delete.size() == 1, "delta search at first sequence");
  require(before_delete[0].node_id == 90, "delta search first id");
  const auto after_delete = manager.search_delta_l2_at(query, 2, 1, seq_delete);
  require(after_delete.empty(), "delta search hides deleted sequence");

  const auto snapshot = manager.snapshot(seq_delete);
  require(snapshot.records.size() == 1, "snapshot deduplicates by id");
  require(snapshot.deleted_count == 1, "snapshot counts tombstone");
  require(manager.close(), "close sequence manager");
}

void test_merge_applies_update_and_tombstone() {
  const float query[] = {0.0f, 0.0f};
  const std::vector<agent_aware::SearchResult> base_results = {
      agent_aware::SearchResult{7, 0.25f},
      agent_aware::SearchResult{8, 0.50f},
  };

  agent_aware::DynamicRecord update;
  update.sequence_id = 1;
  update.node_id = 7;
  update.dim = 2;
  update.vector = {0.0f, 0.0f};
  auto merged = agent_aware::dynamic::merge_base_and_delta_l2(
      base_results, {update}, query, 2, 2);
  require(merged.size() == 2, "update merge result count");
  require(merged[0].id == 7, "update keeps base id");
  require(merged[0].distance == 0.0f, "update recomputes distance");

  agent_aware::DynamicRecord tombstone;
  tombstone.sequence_id = 2;
  tombstone.node_id = 7;
  tombstone.deleted = true;
  merged = agent_aware::dynamic::merge_base_and_delta_l2(
      base_results, {tombstone}, query, 2, 2);
  require(merged.size() == 1, "tombstone removes base id");
  require(merged[0].id == 8, "tombstone leaves other base result");
}

void test_manager_compact_once_publishes_manifest() {
  const auto dir = test_dir("dynamic_manager_compact_once");
  auto manager = agent_aware::dynamic::DynamicWriteManager(options_for(dir));
  require(manager.open(), "open compact manager");

  const float first[] = {1.0f, 1.0f};
  const float second[] = {2.0f, 2.0f};
  require(manager.insert(1, first, 2), "compact insert first");
  require(manager.flush(), "compact flush first");
  require(manager.insert(2, second, 2), "compact insert second");
  require(manager.flush(), "compact flush second");

  agent_aware::dynamic::DynamicCompactionStats stats;
  require(manager.compact_once(&stats), "compact once succeeds");
  require(stats.success, "compact stats success");
  require(stats.input_table_count == 2, "compact input table count");
  require(stats.output_record_count == 2, "compact output record count");

  const auto records = manager.collect_all_delta_records();
  require(records.size() == 2, "compact records remain visible");
  agent_aware::dynamic::ManifestData manifest_data;
  agent_aware::dynamic::Manifest manifest(dir / "manifest.json");
  require(manifest.load(manifest_data), "compact manifest loads");
  require(manifest_data.sstables.size() == 1, "compact manifest table count");
  require(manager.close(), "close compact manager");
}

void test_flush_then_restart_loads_sstable() {
  const auto dir = test_dir("dynamic_flush_restart");
  {
    auto manager = agent_aware::dynamic::DynamicWriteManager(options_for(dir));
    require(manager.open(), "open flush restart writer");
    const float vector[] = {7.0f, 8.0f};
    require(manager.insert(77, vector, 2), "insert before flush restart");
    require(manager.flush(), "flush before restart");
    require(manager.close(), "close flush restart writer");
  }
  {
    auto manager = agent_aware::dynamic::DynamicWriteManager(options_for(dir));
    require(manager.open(), "open flush restart reader");
    agent_aware::DynamicRecord out;
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
  test_update_and_erase_api();
  test_insert_records_incremental_neighbors();
  test_search_delta_l2_finds_inserted_vector();
  test_base_and_delta_merge();
  test_sequence_snapshot_and_latest_record();
  test_merge_applies_update_and_tombstone();
  test_manager_compact_once_publishes_manifest();
  test_flush_then_restart_loads_sstable();
  return 0;
}
