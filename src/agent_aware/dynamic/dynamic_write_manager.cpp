#include "agent_aware/dynamic/dynamic_write_manager.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <utility>

#include "agent_aware/core/brute_force.h"
#include "agent_aware/dynamic/compaction.h"

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

struct ScoredRecord {
  DynamicRecord record;
  float distance = 0.0f;
};

bool scored_less(const ScoredRecord& lhs, const ScoredRecord& rhs) {
  if (lhs.distance == rhs.distance) {
    return lhs.record.node_id < rhs.record.node_id;
  }
  return lhs.distance < rhs.distance;
}

bool search_result_less(const SearchResult& lhs, const SearchResult& rhs) {
  if (lhs.distance == rhs.distance) {
    return lhs.id < rhs.id;
  }
  return lhs.distance < rhs.distance;
}

struct RecordVersionKey {
  NodeId node_id = 0;
  std::uint64_t sequence_id = 0;

  bool operator==(const RecordVersionKey& other) const {
    return node_id == other.node_id && sequence_id == other.sequence_id;
  }
};

struct RecordVersionKeyHash {
  std::size_t operator()(const RecordVersionKey& key) const {
    const auto lhs = std::hash<NodeId>{}(key.node_id);
    const auto rhs = std::hash<std::uint64_t>{}(key.sequence_id);
    return lhs ^ (rhs + 0x9e3779b97f4a7c15ULL + (lhs << 6) + (lhs >> 2));
  }
};

std::vector<DynamicRecord> unique_record_versions(
    const std::vector<DynamicRecord>& records) {
  std::vector<DynamicRecord> output;
  output.reserve(records.size());
  std::unordered_set<RecordVersionKey, RecordVersionKeyHash> seen;
  seen.reserve(records.size());
  for (const auto& record : records) {
    if (seen.insert(RecordVersionKey{record.node_id, record.sequence_id})
            .second) {
      output.push_back(record);
    }
  }
  return output;
}

std::vector<DynamicRecord> deduplicate_newest_at(
    const std::vector<DynamicRecord>& records, std::uint64_t read_sequence) {
  std::vector<DynamicRecord> visible;
  visible.reserve(records.size());
  for (const auto& record : records) {
    if (record.sequence_id <= read_sequence) {
      visible.push_back(record);
    }
  }
  return deduplicate_newest(visible);
}

std::optional<DynamicRecord> latest_record_from_history(
    const std::vector<DynamicRecord>& records, NodeId node_id,
    std::uint64_t read_sequence, bool include_deleted) {
  DynamicRecord latest;
  bool found = false;
  for (const auto& record : records) {
    if (record.node_id != node_id || record.sequence_id > read_sequence) {
      continue;
    }
    if (!found || record.sequence_id > latest.sequence_id) {
      latest = record;
      found = true;
    }
  }
  if (!found || (latest.deleted && !include_deleted)) {
    return std::nullopt;
  }
  return latest;
}

std::unordered_map<NodeId, DynamicRecord> latest_records_for_from_history(
    const std::vector<DynamicRecord>& records,
    const std::vector<NodeId>& node_ids, std::uint64_t read_sequence,
    bool include_deleted) {
  std::unordered_set<NodeId> wanted;
  wanted.reserve(node_ids.size());
  for (const auto node_id : node_ids) {
    wanted.insert(node_id);
  }

  std::unordered_map<NodeId, DynamicRecord> latest;
  latest.reserve(node_ids.size());
  for (const auto& record : records) {
    if (record.sequence_id > read_sequence ||
        wanted.find(record.node_id) == wanted.end()) {
      continue;
    }
    const auto found = latest.find(record.node_id);
    if (found == latest.end() ||
        record.sequence_id > found->second.sequence_id) {
      latest[record.node_id] = record;
    }
  }
  if (!include_deleted) {
    for (auto it = latest.begin(); it != latest.end();) {
      if (it->second.deleted) {
        it = latest.erase(it);
      } else {
        ++it;
      }
    }
  }
  return latest;
}
}  // namespace

struct DynamicWriteManager::DynamicReadView {
  bool opened = false;
  std::uint64_t latest_sequence = 0;
  std::vector<DynamicRecord> history_records;
};

DynamicWriteManager::DynamicWriteManager(DynamicWriteOptions options)
    : options_(std::move(options)),
      memtable_(std::make_unique<MemTable>(options_.memtable_flush_bytes)) {}

bool DynamicWriteManager::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (opened_) {
    return true;
  }

  std::error_code error;
  std::filesystem::create_directories(options_.dynamic_dir, error);
  if (error) {
    return false;
  }
  std::filesystem::create_directories(wal_dir(), error);
  if (error) {
    return false;
  }
  std::filesystem::create_directories(sstable_dir(), error);
  if (error) {
    return false;
  }

  Manifest manifest(manifest_path());
  if (!manifest.load(manifest_)) {
    return false;
  }

  sstables_.clear();
  recent_records_.clear();
  std::uint64_t max_loaded_sequence = 0;
  for (const auto& entry : manifest_.sstables) {
    const auto base_path =
        entry.base_path.is_absolute()
            ? entry.base_path
            : options_.dynamic_dir / entry.base_path;
    auto reader = std::make_shared<SSTableReader>(base_path);
    if (!reader->open()) {
      return false;
    }
    max_loaded_sequence =
        std::max(max_loaded_sequence, reader->max_sequence_id());
    for (const auto& record : reader->scan_all()) {
      recent_records_.push_back(record);
      max_loaded_sequence = std::max(max_loaded_sequence, record.sequence_id);
    }
    sstables_.push_back(std::move(reader));
  }

  memtable_ = std::make_unique<MemTable>(options_.memtable_flush_bytes);
  if (options_.enable_wal) {
    std::uint64_t max_wal_sequence = 0;
    for (const auto& record : WalReader(wal_path()).replay()) {
      memtable_->insert(record);
      recent_records_.push_back(record);
      max_wal_sequence = std::max(max_wal_sequence, record.sequence_id);
    }
    max_loaded_sequence = std::max(max_loaded_sequence, max_wal_sequence);
    manifest_.next_sequence_id =
        std::max(manifest_.next_sequence_id, max_loaded_sequence + 1);
    wal_writer_ = std::make_unique<WalWriter>(wal_path());
    if (!wal_writer_->sync()) {
      return false;
    }
  } else {
    manifest_.next_sequence_id =
        std::max(manifest_.next_sequence_id, max_loaded_sequence + 1);
  }

  latest_sequence_.store(manifest_.next_sequence_id == 0
                             ? 0
                             : manifest_.next_sequence_id - 1,
                         std::memory_order_release);
  opened_ = true;
  publish_read_view_locked();
  return true;
}

bool DynamicWriteManager::insert(NodeId node_id, const float* vector,
                                 std::uint32_t dim) {
  if (vector == nullptr || dim == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return false;
  }

  DynamicRecord record;
  record.sequence_id = manifest_.next_sequence_id;
  record.node_id = node_id;
  record.dim = dim;
  record.vector.resize(dim);
  std::memcpy(record.vector.data(), vector, dim * sizeof(float));
  record.neighbors =
      select_incremental_neighbors_locked(node_id, vector, dim);

  return append_record_locked(std::move(record));
}

bool DynamicWriteManager::update(NodeId node_id, const float* vector,
                                 std::uint32_t dim) {
  return insert(node_id, vector, dim);
}

bool DynamicWriteManager::erase(NodeId node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return false;
  }

  DynamicRecord record;
  record.sequence_id = manifest_.next_sequence_id;
  record.node_id = node_id;
  record.deleted = true;
  return append_record_locked(std::move(record));
}

bool DynamicWriteManager::append_record_locked(DynamicRecord record) {
  if (options_.enable_wal) {
    if (!wal_writer_ || !wal_writer_->append(record)) {
      return false;
    }
  }

  if (!memtable_->insert(record)) {
    return false;
  }
  recent_records_.push_back(record);
  ++manifest_.next_sequence_id;
  publish_read_view_locked();

  if (options_.enable_auto_flush && memtable_->should_flush()) {
    return flush_locked();
  }
  return true;
}

bool DynamicWriteManager::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return false;
  }
  return flush_locked();
}

bool DynamicWriteManager::flush_locked() {
  const auto snapshot = memtable_->snapshot();
  if (snapshot.empty()) {
    return true;
  }

  const auto sstable_id = manifest_.next_sstable_id;
  SSTableWriter writer(sstable_dir(), sstable_id, 0);
  if (!writer.write(snapshot)) {
    return false;
  }

  const auto relative_base =
      std::filesystem::path("sstable") / sstable_name(sstable_id);
  ManifestData next_manifest = manifest_;
  next_manifest.sstables.push_back(
      ManifestSSTableEntry{sstable_id, 0, relative_base});
  ++next_manifest.next_sstable_id;
  next_manifest.wal_checkpoint = 0;

  Manifest manifest(manifest_path());
  if (!manifest.save(next_manifest)) {
    return false;
  }

  auto reader = std::make_shared<SSTableReader>(sstable_dir() /
                                                sstable_name(sstable_id));
  if (!reader->open()) {
    return false;
  }

  manifest_ = std::move(next_manifest);
  sstables_.push_back(std::move(reader));
  memtable_->clear();
  return rotate_wal_locked();
}

void DynamicWriteManager::publish_read_view_locked() {
  auto mutable_view = std::make_shared<DynamicReadView>();
  mutable_view->opened = opened_;
  mutable_view->latest_sequence = manifest_.next_sequence_id == 0
                                      ? 0
                                      : manifest_.next_sequence_id - 1;
  mutable_view->history_records = unique_record_versions(recent_records_);
  std::shared_ptr<const DynamicReadView> view = mutable_view;
  std::atomic_store_explicit(&read_view_, view, std::memory_order_release);
  latest_sequence_.store(view->latest_sequence, std::memory_order_release);
}

std::shared_ptr<const DynamicWriteManager::DynamicReadView>
DynamicWriteManager::load_read_view() const {
  return std::atomic_load_explicit(&read_view_, std::memory_order_acquire);
}

bool DynamicWriteManager::get(NodeId node_id, DynamicRecord& out) const {
  const auto record = latest_record(node_id, current_sequence(), false);
  if (!record.has_value()) {
    return false;
  }
  out = *record;
  return true;
}

std::uint64_t DynamicWriteManager::current_sequence() const {
  return latest_sequence_.load(std::memory_order_acquire);
}

std::optional<DynamicRecord> DynamicWriteManager::latest_record(
    NodeId node_id, std::uint64_t read_sequence, bool include_deleted) const {
  const auto view = load_read_view();
  if (!view || !view->opened) {
    return std::nullopt;
  }
  return latest_record_from_history(view->history_records, node_id,
                                    read_sequence, include_deleted);
}

std::unordered_map<NodeId, DynamicRecord>
DynamicWriteManager::latest_records_for(const std::vector<NodeId>& node_ids,
                                        std::uint64_t read_sequence,
                                        bool include_deleted) const {
  const auto view = load_read_view();
  if (!view || !view->opened) {
    return {};
  }
  return latest_records_for_from_history(view->history_records, node_ids,
                                         read_sequence, include_deleted);
}

DynamicSnapshot DynamicWriteManager::snapshot(
    std::uint64_t read_sequence) const {
  DynamicSnapshot output;
  output.read_sequence = read_sequence;
  const auto view = load_read_view();
  if (!view || !view->opened) {
    return output;
  }
  output.records = deduplicate_newest_at(view->history_records, read_sequence);
  for (const auto& record : output.records) {
    if (record.deleted) {
      ++output.deleted_count;
    }
  }
  return output;
}

bool DynamicWriteManager::latest_record_locked(NodeId node_id,
                                               std::uint64_t read_sequence,
                                               bool include_deleted,
                                               DynamicRecord& out) const {
  const auto records = collect_all_delta_records_locked(read_sequence);
  const auto found = std::find_if(records.begin(), records.end(),
                                  [&](const DynamicRecord& record) {
                                    return record.node_id == node_id;
                                  });
  if (found == records.end() || (found->deleted && !include_deleted)) {
    return false;
  }
  out = *found;
  return true;
}

std::vector<DynamicRecord> DynamicWriteManager::collect_all_delta_records()
    const {
  return collect_all_delta_records_at(std::numeric_limits<std::uint64_t>::max());
}

std::vector<DynamicRecord> DynamicWriteManager::collect_all_delta_records_at(
    std::uint64_t read_sequence) const {
  const auto view = load_read_view();
  if (!view || !view->opened) {
    return {};
  }
  return deduplicate_newest_at(view->history_records, read_sequence);
}

std::vector<DynamicRecord> DynamicWriteManager::collect_all_delta_records_locked(
    std::uint64_t read_sequence) const {
  std::vector<DynamicRecord> records;
  if (!opened_) {
    return records;
  }

  for (const auto& record : recent_records_) {
    if (record.sequence_id <= read_sequence) {
      records.push_back(record);
    }
  }
  return deduplicate_newest(records);
}

std::vector<NodeId> DynamicWriteManager::select_incremental_neighbors_locked(
    NodeId node_id, const float* vector, std::uint32_t dim) const {
  if (options_.dynamic_graph_degree == 0) {
    return {};
  }

  const auto records = collect_all_delta_records_locked();
  std::vector<ScoredRecord> candidates;
  candidates.reserve(records.size());
  for (const auto& record : records) {
    if (record.deleted || record.node_id == node_id || record.dim != dim ||
        record.vector.size() != dim) {
      continue;
    }
    candidates.push_back(
        ScoredRecord{record, squared_l2(vector, record.vector.data(), dim)});
  }

  std::sort(candidates.begin(), candidates.end(), scored_less);
  if (candidates.size() > options_.dynamic_graph_degree) {
    candidates.resize(options_.dynamic_graph_degree);
  }

  std::vector<NodeId> neighbors;
  neighbors.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    neighbors.push_back(candidate.record.node_id);
  }
  return neighbors;
}

std::vector<DynamicRecord> DynamicWriteManager::search_delta_l2(
    const float* query, std::uint32_t dim, std::size_t topk) const {
  return search_delta_l2_at(query, dim, topk,
                            std::numeric_limits<std::uint64_t>::max());
}

std::vector<DynamicRecord> DynamicWriteManager::search_delta_l2_at(
    const float* query, std::uint32_t dim, std::size_t topk,
    std::uint64_t read_sequence) const {
  if (query == nullptr || dim == 0 || topk == 0) {
    return {};
  }

  const auto records = collect_all_delta_records_at(read_sequence);
  std::vector<ScoredRecord> scored;
  scored.reserve(records.size());
  for (const auto& record : records) {
    if (record.deleted || record.dim != dim || record.vector.size() != dim) {
      continue;
    }
    scored.push_back(
        ScoredRecord{record, squared_l2(query, record.vector.data(), dim)});
  }

  std::sort(scored.begin(), scored.end(), scored_less);
  if (scored.size() > topk) {
    scored.resize(topk);
  }

  std::vector<DynamicRecord> output;
  output.reserve(scored.size());
  for (auto& item : scored) {
    output.push_back(std::move(item.record));
  }
  return output;
}

bool DynamicWriteManager::compact_once(DynamicCompactionStats* stats) {
  DynamicCompactionStats local;
  local.attempted = true;

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_ || sstables_.size() < 2) {
    if (stats != nullptr) {
      *stats = local;
    }
    return false;
  }

  agent_aware::dynamic::CompactionInput input;
  input.input_tables = sstables_;
  input.output_dir = sstable_dir();
  input.output_sstable_id = manifest_.next_sstable_id;
  input.output_level = 1;
  local.input_table_count = input.input_tables.size();
  local.output_sstable_id = input.output_sstable_id;

  const auto result = CompactionJob(std::move(input)).run();
  local.success = result.success;
  local.input_record_count = result.input_record_count;
  local.output_record_count = result.output_record_count;
  if (!result.success) {
    if (stats != nullptr) {
      *stats = local;
    }
    return false;
  }

  auto reader = std::make_shared<SSTableReader>(result.output_base_path);
  if (!reader->open()) {
    local.success = false;
    if (stats != nullptr) {
      *stats = local;
    }
    return false;
  }

  ManifestData next_manifest = manifest_;
  next_manifest.sstables.clear();
  const auto relative_base =
      std::filesystem::path("sstable") / sstable_name(local.output_sstable_id);
  next_manifest.sstables.push_back(
      ManifestSSTableEntry{local.output_sstable_id, 1, relative_base});
  ++next_manifest.next_sstable_id;

  Manifest manifest(manifest_path());
  if (!manifest.save(next_manifest)) {
    local.success = false;
    if (stats != nullptr) {
      *stats = local;
    }
    return false;
  }

  std::vector<std::shared_ptr<SSTableReader>> old_tables = std::move(sstables_);
  manifest_ = std::move(next_manifest);
  sstables_.clear();
  sstables_.push_back(std::move(reader));

  for (const auto& table : old_tables) {
    if (!table || table->id() == local.output_sstable_id) {
      continue;
    }
    const auto base = sstable_dir() / sstable_name(table->id());
    std::error_code error;
    std::filesystem::remove(base.string() + ".data", error);
    error.clear();
    std::filesystem::remove(base.string() + ".index", error);
    error.clear();
    std::filesystem::remove(base.string() + ".meta", error);
  }

  if (stats != nullptr) {
    *stats = local;
  }
  return true;
}

bool DynamicWriteManager::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  bool ok = true;
  if (opened_) {
    Manifest manifest(manifest_path());
    ok = manifest.save(manifest_) && ok;
  }
  if (wal_writer_) {
    ok = wal_writer_->close() && ok;
    wal_writer_.reset();
  }
  opened_ = false;
  publish_read_view_locked();
  return ok;
}

bool DynamicWriteManager::rotate_wal_locked() {
  if (!options_.enable_wal) {
    return true;
  }
  if (wal_writer_ && !wal_writer_->close()) {
    return false;
  }
  wal_writer_.reset();

  std::error_code error;
  std::filesystem::remove(wal_path(), error);
  wal_writer_ = std::make_unique<WalWriter>(wal_path());
  return wal_writer_->sync();
}

std::filesystem::path DynamicWriteManager::manifest_path() const {
  return options_.dynamic_dir / "manifest.json";
}

std::filesystem::path DynamicWriteManager::wal_dir() const {
  return options_.dynamic_dir / "wal";
}

std::filesystem::path DynamicWriteManager::wal_path() const {
  return wal_dir() / "wal.log";
}

std::filesystem::path DynamicWriteManager::sstable_dir() const {
  return options_.dynamic_dir / "sstable";
}

std::vector<SearchResult> merge_base_and_delta_l2(
    const std::vector<SearchResult>& base_results,
    const std::vector<DynamicRecord>& delta_records, const float* query,
    std::uint32_t dim, std::size_t topk) {
  if (query == nullptr || dim == 0 || topk == 0) {
    return {};
  }

  std::unordered_map<NodeId, SearchResult> by_id;
  by_id.reserve(base_results.size() + delta_records.size());
  for (const auto& result : base_results) {
    by_id[result.id] = result;
  }

  for (const auto& record : delta_records) {
    if (record.deleted) {
      by_id.erase(record.node_id);
      continue;
    }
    if (record.dim != dim || record.vector.size() != dim) {
      continue;
    }
    by_id[record.node_id] =
        SearchResult{record.node_id, squared_l2(query, record.vector.data(), dim)};
  }

  std::vector<SearchResult> merged;
  merged.reserve(by_id.size());
  for (const auto& item : by_id) {
    merged.push_back(item.second);
  }
  std::sort(merged.begin(), merged.end(), search_result_less);
  if (merged.size() > topk) {
    merged.resize(topk);
  }
  return merged;
}

}  // namespace agent_aware::dynamic
