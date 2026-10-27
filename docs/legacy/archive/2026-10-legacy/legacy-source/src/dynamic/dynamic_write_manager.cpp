#include "dynamic/dynamic_write_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <queue>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "core/brute_force.h"
#include "dynamic/compaction.h"

namespace agent_aware::dynamic {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
          .count());
}

void atomic_max(std::atomic<std::uint64_t>& target, std::uint64_t value) {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
  }
}

std::string sstable_name(std::uint64_t sstable_id) {
  std::ostringstream name;
  name << "sst_" << std::setw(6) << std::setfill('0') << sstable_id;
  return name.str();
}

std::string wal_name(std::uint64_t segment_id) {
  std::ostringstream name;
  name << "wal_" << std::setw(8) << std::setfill('0') << segment_id
       << ".log";
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

}  // namespace

DynamicWriteManager::DynamicWriteManager(DynamicWriteOptions options)
    : options_(std::move(options)),
      memtable_(std::make_unique<MemTable>(options_.memtable_flush_bytes)) {
  options_.write_queue_capacity =
      std::max<std::size_t>(1, options_.write_queue_capacity);
  options_.wal_group_max_records =
      std::max<std::size_t>(1, options_.wal_group_max_records);
  options_.version_index_shards =
      std::max<std::size_t>(1, options_.version_index_shards);
  options_.neighbor_candidate_limit =
      std::max(options_.dynamic_graph_degree, options_.neighbor_candidate_limit);
  const auto shard_count = std::max<std::size_t>(1, options_.version_index_shards);
  version_shards_.reserve(shard_count);
  for (std::size_t i = 0; i < shard_count; ++i) {
    version_shards_.push_back(std::make_shared<VersionShard>());
  }
}

DynamicWriteManager::~DynamicWriteManager() { (void)close(); }

std::shared_ptr<DynamicWriteManager::VersionShard>
DynamicWriteManager::shard_for(NodeId node_id) const {
  return version_shards_[static_cast<std::size_t>(node_id) %
                         version_shards_.size()];
}

bool DynamicWriteManager::open() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
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

  std::uint64_t max_sequence = 0;
  sstables_.clear();
  for (const auto& entry : manifest_.sstables) {
    const auto base_path = entry.base_path.is_absolute()
                               ? entry.base_path
                               : options_.dynamic_dir / entry.base_path;
    auto reader = std::make_shared<SSTableReader>(base_path);
    if (!reader->open()) {
      return false;
    }
    auto records = reader->scan_all();
    for (const auto& record : records) {
      max_sequence = std::max(max_sequence, record.sequence_id);
    }
    apply_records(records);
    max_sequence = std::max(max_sequence, reader->max_sequence_id());
    sstables_.push_back(std::move(reader));
  }

  std::uint64_t max_segment = 0;
  std::vector<std::filesystem::path> wal_paths;
  const std::regex segment_pattern("wal_([0-9]+)\\.log");
  for (const auto& item : std::filesystem::directory_iterator(wal_dir())) {
    if (!item.is_regular_file()) {
      continue;
    }
    std::smatch match;
    const auto filename = item.path().filename().string();
    if (std::regex_match(filename, match, segment_pattern)) {
      max_segment = std::max(max_segment,
                             static_cast<std::uint64_t>(std::stoull(match[1])));
      wal_paths.push_back(item.path());
    }
  }
  std::sort(wal_paths.begin(), wal_paths.end());
  if (std::filesystem::exists(legacy_wal_path())) {
    wal_paths.insert(wal_paths.begin(), legacy_wal_path());
  }

  memtable_ = std::make_unique<MemTable>(options_.memtable_flush_bytes);
  // Only uncheckpointed WAL records belong in the active MemTable.
  for (const auto& path : wal_paths) {
    const auto records = WalReader(path).replay();
    for (const auto& record : records) {
      if (record.sequence_id <= manifest_.wal_checkpoint) {
        continue;
      }
      max_sequence = std::max(max_sequence, record.sequence_id);
      memtable_->insert(record);
      apply_records(std::vector<DynamicRecord>{record});
    }
  }

  manifest_.next_sequence_id =
      std::max(manifest_.next_sequence_id, max_sequence + 1);
  next_sequence_id_.store(manifest_.next_sequence_id,
                          std::memory_order_release);
  latest_sequence_.store(manifest_.next_sequence_id == 0
                             ? 0
                             : manifest_.next_sequence_id - 1,
                         std::memory_order_release);
  next_sstable_id_.store(manifest_.next_sstable_id,
                         std::memory_order_release);

  active_wal_segment_id_.store(max_segment + 1, std::memory_order_release);
  active_wal_max_sequence_ = max_sequence;
  if (options_.enable_wal) {
    wal_writer_ = std::make_unique<WalWriter>(
        wal_path(active_wal_segment_id_.load(std::memory_order_acquire)));
    if (!wal_writer_->valid() || !wal_writer_->sync()) {
      return false;
    }
  }

  failed_.store(false, std::memory_order_release);
  accepting_writes_.store(true, std::memory_order_release);
  opened_ = true;
  flush_stop_ = false;
  if (options_.enable_background_flush) {
    flush_thread_ = std::thread(&DynamicWriteManager::flush_loop, this);
  }
  commit_thread_ = std::thread(&DynamicWriteManager::commit_loop, this);
  return true;
}

bool DynamicWriteManager::submit_record(DynamicRecord record) {
  if (!accepting_writes_.load(std::memory_order_acquire) ||
      failed_.load(std::memory_order_acquire)) {
    return false;
  }

  auto request = std::make_shared<WriteRequest>();
  request->record = std::move(record);
  request->submitted_at = Clock::now();
  stat_submitted_.fetch_add(1, std::memory_order_relaxed);

  std::unique_lock<std::mutex> queue_lock(queue_mutex_);
  while (write_queue_.size() >= options_.write_queue_capacity &&
         accepting_writes_.load(std::memory_order_acquire) &&
         !failed_.load(std::memory_order_acquire)) {
    stat_backpressure_.fetch_add(1, std::memory_order_relaxed);
    queue_space_condition_.wait(queue_lock);
  }
  if (!accepting_writes_.load(std::memory_order_acquire) ||
      failed_.load(std::memory_order_acquire)) {
    stat_failed_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  write_queue_.push_back(request);
  atomic_max(stat_queue_high_watermark_, write_queue_.size());
  queue_lock.unlock();
  queue_condition_.notify_one();

  std::unique_lock<std::mutex> request_lock(request->mutex);
  request->condition.wait(request_lock,
                          [&] { return request->completed; });
  return request->success;
}

bool DynamicWriteManager::submit_control(RequestKind kind) {
  if (!opened_ || failed_.load(std::memory_order_acquire)) {
    return false;
  }
  auto request = std::make_shared<WriteRequest>();
  request->kind = kind;
  request->submitted_at = Clock::now();
  {
    std::unique_lock<std::mutex> queue_lock(queue_mutex_);
    while (write_queue_.size() >= options_.write_queue_capacity &&
           !failed_.load(std::memory_order_acquire)) {
      queue_space_condition_.wait(queue_lock);
    }
    if (failed_.load(std::memory_order_acquire)) {
      return false;
    }
    write_queue_.push_back(request);
  }
  queue_condition_.notify_one();
  std::unique_lock<std::mutex> request_lock(request->mutex);
  request->condition.wait(request_lock,
                          [&] { return request->completed; });
  return request->success;
}

void DynamicWriteManager::complete_request(
    const std::shared_ptr<WriteRequest>& request, bool success) {
  if (!request) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->success = success;
    request->completed = true;
  }
  request->condition.notify_one();
}

void DynamicWriteManager::set_failed() {
  failed_.store(true, std::memory_order_release);
  accepting_writes_.store(false, std::memory_order_release);
  queue_condition_.notify_all();
  queue_space_condition_.notify_all();
}

void DynamicWriteManager::fail_pending_requests() {
  std::deque<std::shared_ptr<WriteRequest>> pending;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending.swap(write_queue_);
  }
  queue_space_condition_.notify_all();
  for (const auto& request : pending) {
    stat_failed_.fetch_add(1, std::memory_order_relaxed);
    complete_request(request, false);
  }
}

bool DynamicWriteManager::commit_batch(
    const std::vector<std::shared_ptr<WriteRequest>>& requests) {
  if (requests.empty()) {
    return true;
  }
  std::vector<DynamicRecord> records;
  records.reserve(requests.size());
  auto next = next_sequence_id_.load(std::memory_order_relaxed);
  for (const auto& request : requests) {
    auto record = request->record;
    record.sequence_id = next++;
    records.push_back(std::move(record));
  }

  const auto wal_start = Clock::now();
  if (options_.enable_wal &&
      (!wal_writer_ || !wal_writer_->append_batch(records))) {
    set_failed();
    for (const auto& request : requests) {
      stat_failed_.fetch_add(1, std::memory_order_relaxed);
      complete_request(request, false);
    }
    return false;
  }
  stat_wal_append_ns_.fetch_add(elapsed_ns(wal_start, Clock::now()),
                                std::memory_order_relaxed);
  const auto sync_start = Clock::now();
  if (options_.enable_wal && !wal_writer_->sync()) {
    set_failed();
    for (const auto& request : requests) {
      stat_failed_.fetch_add(1, std::memory_order_relaxed);
      complete_request(request, false);
    }
    return false;
  }
  stat_wal_sync_ns_.fetch_add(elapsed_ns(sync_start, Clock::now()),
                              std::memory_order_relaxed);

  const auto apply_start = Clock::now();
  apply_records(records);
  for (const auto& record : records) {
    if (!memtable_->insert(record)) {
      set_failed();
      break;
    }
    active_wal_max_sequence_ =
        std::max(active_wal_max_sequence_, record.sequence_id);
  }
  stat_memtable_apply_ns_.fetch_add(elapsed_ns(apply_start, Clock::now()),
                                    std::memory_order_relaxed);
  next_sequence_id_.store(next, std::memory_order_release);
  latest_sequence_.store(next - 1, std::memory_order_release);
  stat_batches_.fetch_add(1, std::memory_order_relaxed);
  stat_batch_records_.fetch_add(records.size(), std::memory_order_relaxed);
  stat_committed_.fetch_add(records.size(), std::memory_order_relaxed);
  for (const auto& request : requests) {
    complete_request(request, !failed_.load(std::memory_order_acquire));
  }
  if (options_.enable_auto_flush && memtable_->should_flush()) {
    if (!rotate_memtable_locked()) {
      set_failed();
      return false;
    }
  }
  return !failed_.load(std::memory_order_acquire);
}

void DynamicWriteManager::commit_loop() {
  for (;;) {
    std::shared_ptr<WriteRequest> first;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_condition_.wait(
          lock, [&] { return !write_queue_.empty() || failed_.load(); });
      if (write_queue_.empty()) {
        if (failed_.load()) {
          fail_pending_requests();
          return;
        }
        continue;
      }
      first = write_queue_.front();
      write_queue_.pop_front();
      queue_space_condition_.notify_one();
    }

    if (first->kind == RequestKind::stop) {
      (void)rotate_memtable_locked();
      complete_request(first, !failed_.load());
      return;
    }
    if (first->kind == RequestKind::flush) {
      const bool ok = rotate_memtable_locked(first);
      if (!ok) {
        complete_request(first, false);
      }
      continue;
    }

    std::vector<std::shared_ptr<WriteRequest>> batch;
    batch.push_back(first);
    const auto deadline = Clock::now() +
                          std::chrono::microseconds(options_.wal_group_max_delay_us);
    while (batch.size() < options_.wal_group_max_records) {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (write_queue_.empty()) {
        if (queue_condition_.wait_until(lock, deadline, [&] {
              return !write_queue_.empty() || failed_.load();
            })) {
          // Continue below and consume the next mutation if present.
        } else {
          break;
        }
      }
      if (write_queue_.empty() || write_queue_.front()->kind != RequestKind::mutation) {
        break;
      }
      batch.push_back(write_queue_.front());
      write_queue_.pop_front();
      queue_space_condition_.notify_one();
    }
    for (const auto& request : batch) {
      stat_queue_wait_ns_.fetch_add(
          elapsed_ns(request->submitted_at, Clock::now()),
          std::memory_order_relaxed);
    }
    if (!commit_batch(batch)) {
      fail_pending_requests();
      return;
    }
  }
}

void DynamicWriteManager::apply_records(
    const std::vector<DynamicRecord>& records) {
  for (const auto& record : records) {
    auto shard = shard_for(record.node_id);
    std::unique_lock<std::shared_mutex> lock(shard->mutex);
    shard->versions[record.node_id].push_back(record);
  }
}

bool DynamicWriteManager::rotate_memtable_locked(
    std::shared_ptr<WriteRequest> waiter) {
  auto records = memtable_->snapshot();
  FlushTask task;
  task.records = std::move(records);
  task.max_sequence = latest_sequence_.load(std::memory_order_acquire);
  task.wal_segment_id =
      active_wal_segment_id_.load(std::memory_order_acquire);
  task.waiter = std::move(waiter);
  memtable_ = std::make_unique<MemTable>(options_.memtable_flush_bytes);

  if (options_.enable_wal && wal_writer_) {
    if (!wal_writer_->close()) {
      return false;
    }
    wal_writer_.reset();
    const auto next_segment =
        active_wal_segment_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
    active_wal_max_sequence_ = task.max_sequence;
    wal_writer_ = std::make_unique<WalWriter>(wal_path(next_segment));
    if (!wal_writer_->valid() || !wal_writer_->sync()) {
      return false;
    }
  }

  if (!options_.enable_background_flush) {
    stat_flush_tasks_.fetch_add(1, std::memory_order_relaxed);
    return flush_task(task);
  }

  {
    std::lock_guard<std::mutex> lock(flush_queue_mutex_);
    flush_queue_.push_back(std::move(task));
    atomic_max(stat_flush_queue_high_watermark_, flush_queue_.size());
  }
  stat_flush_tasks_.fetch_add(1, std::memory_order_relaxed);
  flush_queue_condition_.notify_one();
  return true;
}

bool DynamicWriteManager::flush_task(const FlushTask& task) {
  if (task.records.empty()) {
    if (task.waiter) {
      complete_request(task.waiter, true);
    }
    return true;
  }

  const auto id = next_sstable_id_.fetch_add(1, std::memory_order_relaxed);
  SSTableWriter writer(sstable_dir(), id, 0);
  if (!writer.write(task.records)) {
    stat_flush_failures_.fetch_add(1, std::memory_order_relaxed);
    if (task.waiter) {
      complete_request(task.waiter, false);
    }
    return false;
  }
  auto reader = std::make_shared<SSTableReader>(sstable_dir() / sstable_name(id));
  if (!reader->open()) {
    stat_flush_failures_.fetch_add(1, std::memory_order_relaxed);
    if (task.waiter) {
      complete_request(task.waiter, false);
    }
    return false;
  }

  bool committed = false;
  {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    ManifestData next_manifest = manifest_;
    next_manifest.sstables.push_back(
        ManifestSSTableEntry{id, 0, std::filesystem::path("sstable") /
                                      sstable_name(id)});
    next_manifest.next_sstable_id =
        std::max(next_manifest.next_sstable_id, id + 1);
    next_manifest.next_sequence_id =
        next_sequence_id_.load(std::memory_order_acquire);
    next_manifest.wal_checkpoint =
        std::max(next_manifest.wal_checkpoint, task.max_sequence);
    Manifest manifest(manifest_path());
    if (manifest.save(next_manifest)) {
      manifest_ = std::move(next_manifest);
      sstables_.push_back(std::move(reader));
      ++metadata_generation_;
      committed = true;
    }
  }

  if (committed &&
      task.wal_segment_id !=
          active_wal_segment_id_.load(std::memory_order_acquire) &&
      task.wal_segment_id != 0) {
    std::error_code error;
    std::filesystem::remove(wal_path(task.wal_segment_id), error);
  }
  if (task.waiter) {
    complete_request(task.waiter, committed);
  }
  if (!committed) {
    stat_flush_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  return committed;
}

void DynamicWriteManager::flush_loop() {
  for (;;) {
    FlushTask task;
    {
      std::unique_lock<std::mutex> lock(flush_queue_mutex_);
      flush_queue_condition_.wait(lock,
                                   [&] { return flush_stop_ || !flush_queue_.empty(); });
      if (flush_queue_.empty() && flush_stop_) {
        return;
      }
      task = std::move(flush_queue_.front());
      flush_queue_.pop_front();
    }
    (void)flush_task(task);
  }
}

bool DynamicWriteManager::insert(NodeId node_id, const float* vector,
                                 std::uint32_t dim) {
  if (vector == nullptr || dim == 0) {
    return false;
  }
  DynamicRecord record;
  record.node_id = node_id;
  record.dim = dim;
  record.vector.assign(vector, vector + dim);
  const auto prepare_start = Clock::now();
  record.neighbors = select_incremental_neighbors(
      node_id, vector, dim, current_sequence());
  stat_neighbor_prepare_ns_.fetch_add(
      elapsed_ns(prepare_start, Clock::now()), std::memory_order_relaxed);
  return submit_record(std::move(record));
}

bool DynamicWriteManager::update(NodeId node_id, const float* vector,
                                 std::uint32_t dim) {
  return insert(node_id, vector, dim);
}

bool DynamicWriteManager::erase(NodeId node_id) {
  DynamicRecord record;
  record.node_id = node_id;
  record.deleted = true;
  return submit_record(std::move(record));
}

bool DynamicWriteManager::flush() { return submit_control(RequestKind::flush); }

std::uint64_t DynamicWriteManager::current_sequence() const {
  return latest_sequence_.load(std::memory_order_acquire);
}

DynamicWriteStats DynamicWriteManager::write_stats() const {
  return DynamicWriteStats{
      stat_submitted_.load(), stat_committed_.load(), stat_failed_.load(),
      stat_batches_.load(), stat_batch_records_.load(), stat_queue_wait_ns_.load(),
      stat_neighbor_prepare_ns_.load(), stat_wal_append_ns_.load(),
      stat_wal_sync_ns_.load(), stat_memtable_apply_ns_.load(),
      stat_backpressure_.load(), stat_queue_high_watermark_.load(),
      stat_flush_tasks_.load(), stat_flush_failures_.load(),
      stat_flush_queue_high_watermark_.load()};
}

std::unordered_map<NodeId, DynamicRecord>
DynamicWriteManager::latest_records_for(const std::vector<NodeId>& node_ids,
                                        std::uint64_t read_sequence,
                                        bool include_deleted) const {
  std::unordered_set<NodeId> wanted(node_ids.begin(), node_ids.end());
  std::unordered_map<NodeId, DynamicRecord> output;
  output.reserve(node_ids.size());
  for (const auto& shard : version_shards_) {
    std::shared_lock<std::shared_mutex> lock(shard->mutex);
    for (const auto node_id : wanted) {
      if (shard_for(node_id) != shard) {
        continue;
      }
      const auto found = shard->versions.find(node_id);
      if (found == shard->versions.end()) {
        continue;
      }
      for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) {
        if (it->sequence_id <= read_sequence) {
          if (include_deleted || !it->deleted) {
            output[node_id] = *it;
          }
          break;
        }
      }
    }
  }
  return output;
}

std::vector<DynamicRecord>
DynamicWriteManager::collect_all_delta_records_at_internal(
    std::uint64_t read_sequence) const {
  std::vector<DynamicRecord> output;
  for (const auto& shard : version_shards_) {
    std::shared_lock<std::shared_mutex> lock(shard->mutex);
    for (const auto& item : shard->versions) {
      for (auto it = item.second.rbegin(); it != item.second.rend(); ++it) {
        if (it->sequence_id <= read_sequence) {
          output.push_back(*it);
          break;
        }
      }
    }
  }
  std::sort(output.begin(), output.end(),
            [](const DynamicRecord& lhs, const DynamicRecord& rhs) {
              return lhs.node_id < rhs.node_id;
            });
  return output;
}

std::vector<DynamicRecord>
DynamicWriteManager::collect_all_delta_records_at(
    std::uint64_t read_sequence) const {
  return collect_all_delta_records_at_internal(read_sequence);
}

std::vector<DynamicRecord>
DynamicWriteManager::collect_all_delta_records() const {
  return collect_all_delta_records_at_internal(
      std::numeric_limits<std::uint64_t>::max());
}

DynamicSnapshot DynamicWriteManager::snapshot(std::uint64_t read_sequence) const {
  DynamicSnapshot output;
  output.read_sequence = read_sequence;
  output.records = collect_all_delta_records_at_internal(read_sequence);
  for (const auto& record : output.records) {
    if (record.deleted) {
      ++output.deleted_count;
    }
  }
  return output;
}

std::vector<NodeId> DynamicWriteManager::select_incremental_neighbors(
    NodeId node_id, const float* vector, std::uint32_t dim,
    std::uint64_t read_sequence) const {
  if (options_.dynamic_graph_degree == 0 || vector == nullptr) {
    return {};
  }
  auto records = collect_all_delta_records_at_internal(read_sequence);
  records.erase(std::remove_if(records.begin(), records.end(),
                               [&](const DynamicRecord& record) {
                                 return record.deleted || record.node_id == node_id ||
                                        record.dim != dim || record.vector.size() != dim;
                               }),
                records.end());
  if (records.size() > options_.neighbor_candidate_limit) {
    const auto limit = options_.neighbor_candidate_limit;
    std::nth_element(
        records.begin(), records.begin() + limit, records.end(),
        [node_id](const DynamicRecord& lhs, const DynamicRecord& rhs) {
          const auto lh = std::hash<NodeId>{}(lhs.node_id ^ node_id);
          const auto rh = std::hash<NodeId>{}(rhs.node_id ^ node_id);
          return lh < rh;
        });
    records.resize(limit);
  }
  std::vector<ScoredRecord> candidates;
  candidates.reserve(records.size());
  for (const auto& record : records) {
    candidates.push_back(
        ScoredRecord{record, squared_l2(vector, record.vector.data(), dim)});
  }
  const auto degree = std::min(options_.dynamic_graph_degree, candidates.size());
  if (degree < candidates.size()) {
    std::nth_element(candidates.begin(), candidates.begin() + degree,
                     candidates.end(), scored_less);
    candidates.resize(degree);
  }
  std::sort(candidates.begin(), candidates.end(), scored_less);
  std::vector<NodeId> neighbors;
  neighbors.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    neighbors.push_back(candidate.record.node_id);
  }
  return neighbors;
}

std::vector<DynamicRecord> DynamicWriteManager::search_delta_l2_at(
    const float* query, std::uint32_t dim, std::size_t topk,
    std::uint64_t read_sequence) const {
  if (query == nullptr || dim == 0 || topk == 0) {
    return {};
  }
  const auto records = collect_all_delta_records_at_internal(read_sequence);
  std::vector<ScoredRecord> scored;
  scored.reserve(records.size());
  for (const auto& record : records) {
    if (record.deleted || record.dim != dim || record.vector.size() != dim) {
      continue;
    }
    scored.push_back(
        ScoredRecord{record, squared_l2(query, record.vector.data(), dim)});
  }
  const auto count = std::min(topk, scored.size());
  if (count < scored.size()) {
    std::nth_element(scored.begin(), scored.begin() + count, scored.end(),
                     scored_less);
    scored.resize(count);
  }
  std::sort(scored.begin(), scored.end(), scored_less);
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
  std::vector<std::shared_ptr<SSTableReader>> input_tables;
  std::uint64_t generation = 0;
  std::uint64_t output_id = 0;
  {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    if (!opened_ || sstables_.size() < 2) {
      if (stats) {
        *stats = local;
      }
      return false;
    }
    input_tables = sstables_;
    generation = metadata_generation_;
    output_id = next_sstable_id_.fetch_add(1, std::memory_order_relaxed);
  }

  local.input_table_count = input_tables.size();
  local.output_sstable_id = output_id;
  CompactionInput input{input_tables, sstable_dir(), output_id, 1};
  const auto result = CompactionJob(std::move(input)).run();
  local.input_record_count = result.input_record_count;
  local.output_record_count = result.output_record_count;
  local.success = result.success;
  if (!result.success) {
    if (stats) {
      *stats = local;
    }
    return false;
  }

  auto reader = std::make_shared<SSTableReader>(result.output_base_path);
  if (!reader->open()) {
    if (stats) {
      *stats = local;
    }
    return false;
  }

  std::vector<std::shared_ptr<SSTableReader>> old_tables;
  bool committed = false;
  {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    if (metadata_generation_ == generation) {
      ManifestData next_manifest = manifest_;
      next_manifest.sstables.clear();
      next_manifest.sstables.push_back(
          ManifestSSTableEntry{output_id, 1,
                               std::filesystem::path("sstable") /
                                   sstable_name(output_id)});
      next_manifest.next_sstable_id =
          std::max(next_manifest.next_sstable_id, output_id + 1);
      next_manifest.next_sequence_id =
          next_sequence_id_.load(std::memory_order_acquire);
      Manifest manifest(manifest_path());
      if (manifest.save(next_manifest)) {
        old_tables = std::move(sstables_);
        sstables_.clear();
        sstables_.push_back(std::move(reader));
        manifest_ = std::move(next_manifest);
        ++metadata_generation_;
        committed = true;
      }
    }
  }
  if (committed) {
    for (const auto& table : old_tables) {
      if (!table || table->id() == output_id) {
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
  } else {
    std::error_code error;
    std::filesystem::remove(result.output_base_path.string() + ".data", error);
    error.clear();
    std::filesystem::remove(result.output_base_path.string() + ".index", error);
    error.clear();
    std::filesystem::remove(result.output_base_path.string() + ".meta", error);
  }
  local.success = committed;
  if (stats) {
    *stats = local;
  }
  return committed;
}

bool DynamicWriteManager::close() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!opened_) {
    return true;
  }
  bool ok = !failed_.load(std::memory_order_acquire);
  accepting_writes_.store(false, std::memory_order_release);
  if (commit_thread_.joinable()) {
    if (failed_.load(std::memory_order_acquire)) {
      queue_condition_.notify_all();
      commit_thread_.join();
    } else {
      auto stop_request = std::make_shared<WriteRequest>();
      stop_request->kind = RequestKind::stop;
      {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        write_queue_.push_back(stop_request);
      }
      queue_condition_.notify_one();
      commit_thread_.join();
      std::unique_lock<std::mutex> request_lock(stop_request->mutex);
      stop_request->condition.wait(request_lock,
                                   [&] { return stop_request->completed; });
    }
  }
  {
    std::lock_guard<std::mutex> lock(flush_queue_mutex_);
    flush_stop_ = true;
  }
  flush_queue_condition_.notify_all();
  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }
  if (wal_writer_) {
    ok = wal_writer_->close() && ok;
    wal_writer_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    manifest_.next_sequence_id =
        next_sequence_id_.load(std::memory_order_acquire);
    Manifest manifest(manifest_path());
    ok = manifest.save(manifest_) && ok;
  }
  opened_ = false;
  accepting_writes_.store(false, std::memory_order_release);
  return ok;
}

std::filesystem::path DynamicWriteManager::manifest_path() const {
  return options_.dynamic_dir / "manifest.json";
}

std::filesystem::path DynamicWriteManager::wal_dir() const {
  return options_.dynamic_dir / "wal";
}

std::filesystem::path DynamicWriteManager::legacy_wal_path() const {
  return wal_dir() / "wal.log";
}

std::filesystem::path DynamicWriteManager::wal_path(
    std::uint64_t segment_id) const {
  return wal_dir() / wal_name(segment_id);
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
  const auto count = std::min(topk, merged.size());
  if (count < merged.size()) {
    std::nth_element(merged.begin(), merged.begin() + count, merged.end(),
                     search_result_less);
    merged.resize(count);
  }
  std::sort(merged.begin(), merged.end(), search_result_less);
  return merged;
}

}  // namespace agent_aware::dynamic
