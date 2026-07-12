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

std::vector<MaterializedRecord> deduplicate_newest(
    const std::vector<MaterializedRecord>& records) {
  std::unordered_map<NodeId, MaterializedRecord> latest;
  latest.reserve(records.size());
  for (const auto& record : records) {
    const auto found = latest.find(record.node_id);
    if (found == latest.end() ||
        record.sequence_id > found->second.sequence_id) {
      latest[record.node_id] = record;
    }
  }

  std::vector<MaterializedRecord> output;
  output.reserve(latest.size());
  for (const auto& item : latest) {
    output.push_back(item.second);
  }
  std::sort(output.begin(), output.end(),
            [](const MaterializedRecord& lhs, const MaterializedRecord& rhs) {
              return lhs.node_id < rhs.node_id;
            });
  return output;
}

struct ScoredRecord {
  MaterializedRecord record;
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
      memory_budget_(options_.delta_soft_limit_bytes,
                     options_.delta_high_limit_bytes,
                     options_.delta_hard_limit_bytes),
      memtable_(std::make_unique<MemTable>(options_.memtable_flush_bytes)) {
  options_.write_queue_capacity =
      std::max<std::size_t>(1, options_.write_queue_capacity);
  options_.wal_group_max_records =
      std::max<std::size_t>(1, options_.wal_group_max_records);
  options_.version_index_shards =
      std::max<std::size_t>(1, options_.version_index_shards);
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
    auto located = reader->scan_all_located();
    std::vector<MaterializedRecord> records;
    records.reserve(located.size());
    for (auto& item : located) {
      item.first.locator = item.second;
      records.push_back(std::move(item.first));
    }
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
      CompactDeltaVersion compact;
      compact.sequence_id = record.sequence_id;
      compact.node_id = record.node_id;
      compact.dim = record.dim;
      compact.locator = record.locator;
      compact.deleted = record.deleted;
      if (!record.deleted && options_.pq_model && options_.pq_model->enabled()) {
        compact.pq_code = options_.pq_model->encode(record.vector.data());
      }
      memtable_->insert(compact);
      apply_records(std::vector<MaterializedRecord>{record});
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
  wal_writer_ = std::make_unique<WalWriter>(
      wal_path(active_wal_segment_id_.load(std::memory_order_acquire)),
      active_wal_segment_id_.load(std::memory_order_acquire));
  if (!wal_writer_->valid() || !wal_writer_->sync()) {
    return false;
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

bool DynamicWriteManager::submit_record(WritePayload payload) {
  if (!accepting_writes_.load(std::memory_order_acquire) ||
      failed_.load(std::memory_order_acquire)) {
    return false;
  }

  auto request = std::make_shared<WriteRequest>();
  request->payload = std::move(payload);
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
  std::vector<MaterializedRecord> records;
  records.reserve(requests.size());
  auto next = next_sequence_id_.load(std::memory_order_relaxed);
  for (const auto& request : requests) {
    MaterializedRecord record;
    record.node_id = request->payload.node_id;
    record.dim = request->payload.dim;
    record.vector = std::move(request->payload.vector);
    record.deleted = request->payload.deleted;
    record.sequence_id = next++;
    records.push_back(std::move(record));
  }

  std::uint64_t reserved_bytes = 0;
  for (const auto& record : records) reserved_bytes += estimate_version_bytes(record);
  if (!memory_budget_.try_reserve(MemoryDomain::kDeltaControlled,
                                  reserved_bytes)) {
    stat_backpressure_.fetch_add(1, std::memory_order_relaxed);
    for (const auto& request : requests) {
      stat_failed_.fetch_add(1, std::memory_order_relaxed);
      complete_request(request, false);
    }
    return true;
  }

  const auto wal_start = Clock::now();
  std::vector<DynamicLocator> locators;
  if (!wal_writer_ || !wal_writer_->append_batch(records, locators)) {
    memory_budget_.release(MemoryDomain::kDeltaControlled, reserved_bytes);
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
  if (!wal_writer_->sync()) {
    memory_budget_.release(MemoryDomain::kDeltaControlled, reserved_bytes);
    set_failed();
    for (const auto& request : requests) {
      stat_failed_.fetch_add(1, std::memory_order_relaxed);
      complete_request(request, false);
    }
    return false;
  }
  stat_wal_sync_ns_.fetch_add(elapsed_ns(sync_start, Clock::now()),
                              std::memory_order_relaxed);
  for (std::size_t i = 0; i < records.size(); ++i) {
    records[i].locator = locators[i];
  }

  const auto apply_start = Clock::now();
  apply_records(records, true);
  for (const auto& record : records) {
    CompactDeltaVersion compact;
    compact.sequence_id = record.sequence_id;
    compact.node_id = record.node_id;
    compact.dim = record.dim;
    compact.locator = record.locator;
    compact.deleted = record.deleted;
    if (!record.deleted && options_.pq_model && options_.pq_model->enabled()) {
      compact.pq_code = options_.pq_model->encode(record.vector.data());
    }
    if (!memtable_->insert(compact)) {
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
  if (options_.enable_auto_flush &&
      (memtable_->should_flush() ||
       memtable_->size() >= options_.memtable_flush_records)) {
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
    const std::vector<MaterializedRecord>& records, bool pre_reserved) {
  for (const auto& record : records) {
    if (!pre_reserved) {
      memory_budget_.account_existing(MemoryDomain::kDeltaControlled,
                                      estimate_version_bytes(record));
    }
    VersionShard::Version version;
    version.sequence_id = record.sequence_id;
    version.node_id = record.node_id;
    version.locator = record.locator;
    version.dim = record.dim;
    version.deleted = record.deleted;
    if (!record.deleted && options_.pq_model && options_.pq_model->enabled() &&
        record.vector.size() == options_.pq_model->dim()) {
      version.pq_code = options_.pq_model->encode(record.vector.data());
    }
    auto shard = shard_for(record.node_id);
    std::unique_lock<std::shared_mutex> lock(shard->mutex);
    shard->versions[record.node_id].push_back(std::move(version));
  }
}

std::uint64_t DynamicWriteManager::estimate_version_bytes(
    const MaterializedRecord& record) const {
  std::uint64_t bytes = sizeof(VersionShard::Version);
  if (!record.deleted && options_.pq_model && options_.pq_model->enabled()) {
    bytes += options_.pq_model->subspaces();
  }
  return bytes;
}

void DynamicWriteManager::redirect_records(
    const std::vector<MaterializedRecord>& records) {
  for (const auto& record : records) {
    auto shard = shard_for(record.node_id);
    std::unique_lock<std::shared_mutex> lock(shard->mutex);
    const auto found = shard->versions.find(record.node_id);
    if (found == shard->versions.end()) continue;
    for (auto& version : found->second) {
      if (version.sequence_id == record.sequence_id) {
        version.locator = record.locator;
        break;
      }
    }
  }
}

bool DynamicWriteManager::rotate_memtable_locked(
    std::shared_ptr<WriteRequest> waiter) {
  FlushTask task;
  task.table = std::move(memtable_);
  task.max_sequence = latest_sequence_.load(std::memory_order_acquire);
  task.wal_segment_id =
      active_wal_segment_id_.load(std::memory_order_acquire);
  task.waiter = std::move(waiter);
  memtable_ = std::make_unique<MemTable>(options_.memtable_flush_bytes);

  if (wal_writer_) {
    if (!wal_writer_->close()) {
      return false;
    }
    wal_writer_.reset();
    const auto next_segment =
        active_wal_segment_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
    active_wal_max_sequence_ = task.max_sequence;
    wal_writer_ = std::make_unique<WalWriter>(wal_path(next_segment),
                                              next_segment);
    if (!wal_writer_->valid() || !wal_writer_->sync()) {
      return false;
    }
  }

  if (!options_.enable_background_flush) {
    stat_flush_tasks_.fetch_add(1, std::memory_order_relaxed);
    return flush_task(task);
  }

  {
    std::unique_lock<std::mutex> lock(flush_queue_mutex_);
    flush_queue_space_condition_.wait(lock, [&] {
      return flush_stop_ || flush_queue_.size() < 1;
    });
    if (flush_stop_) return false;
    const auto bytes = task.table ? task.table->resident_bytes() : 0;
    frozen_resident_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    flush_queue_.push_back(std::move(task));
    atomic_max(stat_flush_queue_high_watermark_, flush_queue_.size());
  }
  stat_flush_tasks_.fetch_add(1, std::memory_order_relaxed);
  flush_queue_condition_.notify_one();
  return true;
}

bool DynamicWriteManager::flush_task(const FlushTask& task) {
  if (!task.table || task.table->size() == 0) {
    if (task.waiter) {
      complete_request(task.waiter, true);
    }
    return true;
  }

  std::vector<MaterializedRecord> materialized;
  materialized.reserve(task.table->size());
  for (std::size_t i = 0; i < task.table->size(); ++i) {
    if (!task.table->is_latest(i)) continue;
    const auto compact = task.table->at(i);
    if (compact.deleted) {
      MaterializedRecord record;
      record.sequence_id = compact.sequence_id;
      record.node_id = compact.node_id;
      record.dim = compact.dim;
      record.locator = compact.locator;
      record.deleted = true;
      materialized.push_back(std::move(record));
      continue;
    }
    MaterializedRecord record;
    if (compact.locator.kind != DynamicStorageKind::kWal ||
        !WalReader(wal_path(compact.locator.file_id))
             .read_at(compact.locator, record)) {
      stat_flush_failures_.fetch_add(1, std::memory_order_relaxed);
      if (task.waiter) complete_request(task.waiter, false);
      return false;
    }
    materialized.push_back(std::move(record));
  }

  const auto id = next_sstable_id_.fetch_add(1, std::memory_order_relaxed);
  SSTableWriter writer(sstable_dir(), id, 0);
  if (!writer.write(materialized)) {
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

  if (committed) {
    std::shared_ptr<SSTableReader> published;
    {
      std::lock_guard<std::mutex> lock(metadata_mutex_);
      for (const auto& table : sstables_) {
        if (table && table->id() == id) published = table;
      }
    }
    auto located = published ? published->scan_all_located()
                             : std::vector<std::pair<MaterializedRecord,
                                                     DynamicLocator>>{};
    std::vector<MaterializedRecord> redirected;
    redirected.reserve(located.size());
    for (auto& item : located) {
      item.first.locator = item.second;
      redirected.push_back(std::move(item.first));
    }
    redirect_records(redirected);
  }
  // Historical snapshot versions may still point into this WAL. Segment
  // reclamation is deferred to the packed-generation checkpoint, after all
  // readers below its incorporated sequence have drained.
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
      flush_queue_space_condition_.notify_one();
    }
    (void)flush_task(task);
    const auto bytes = task.table ? task.table->resident_bytes() : 0;
    frozen_resident_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
  }
}

bool DynamicWriteManager::insert(NodeId node_id, const float* vector,
                                 std::uint32_t dim) {
  if (vector == nullptr || dim == 0) {
    return false;
  }
  WritePayload payload;
  payload.node_id = node_id;
  payload.dim = dim;
  payload.vector.assign(vector, vector + dim);
  return submit_record(std::move(payload));
}

bool DynamicWriteManager::update(NodeId node_id, const float* vector,
                                 std::uint32_t dim) {
  return insert(node_id, vector, dim);
}

bool DynamicWriteManager::erase(NodeId node_id) {
  WritePayload payload;
  payload.node_id = node_id;
  payload.deleted = true;
  return submit_record(std::move(payload));
}

bool DynamicWriteManager::flush() { return submit_control(RequestKind::flush); }

std::uint64_t DynamicWriteManager::current_sequence() const {
  return latest_sequence_.load(std::memory_order_acquire);
}

DynamicWriteStats DynamicWriteManager::write_stats() const {
  return DynamicWriteStats{
      stat_submitted_.load(), stat_committed_.load(), stat_failed_.load(),
      stat_batches_.load(), stat_batch_records_.load(), stat_queue_wait_ns_.load(),
      stat_wal_append_ns_.load(),
      stat_wal_sync_ns_.load(), stat_memtable_apply_ns_.load(),
      stat_backpressure_.load(), stat_queue_high_watermark_.load(),
      stat_flush_tasks_.load(), stat_flush_failures_.load(),
      stat_flush_queue_high_watermark_.load()};
}

bool DynamicWriteManager::materialize(
    NodeId node_id, const VersionShard::Version& version,
    MaterializedRecord& out) const {
  MaterializedRecord record;
  record.node_id = node_id;
  record.sequence_id = version.sequence_id;
  record.dim = version.dim;
  record.deleted = version.deleted;
  record.locator = version.locator;
  if (record.deleted) {
    out = std::move(record);
    return true;
  }
  if (version.locator.kind == DynamicStorageKind::kWal) {
    if (!WalReader(wal_path(version.locator.file_id))
             .read_at(version.locator, record)) {
      if (version.locator.file_id != 0 ||
          !WalReader(legacy_wal_path()).read_at(version.locator, record)) {
        return false;
      }
    }
    out = std::move(record);
    return true;
  }
  if (version.locator.kind == DynamicStorageKind::kSSTable) {
    std::shared_ptr<SSTableReader> reader;
    {
      std::lock_guard<std::mutex> lock(metadata_mutex_);
      for (const auto& table : sstables_) {
        if (table && table->id() == version.locator.file_id) {
          reader = table;
          break;
        }
      }
    }
    return reader && reader->read_at(version.locator, out);
  }
  return false;
}

std::unordered_map<NodeId, MaterializedRecord>
DynamicWriteManager::latest_records_for(const std::vector<NodeId>& node_ids,
                                        std::uint64_t read_sequence,
                                        bool include_deleted) const {
  std::unordered_set<NodeId> wanted(node_ids.begin(), node_ids.end());
  std::vector<std::pair<NodeId, VersionShard::Version>> selected;
  selected.reserve(node_ids.size());
  std::unordered_map<NodeId, MaterializedRecord> output;
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
            selected.emplace_back(node_id, *it);
          }
          break;
        }
      }
    }
  }
  for (const auto& item : selected) {
    MaterializedRecord record;
    if (materialize(item.first, item.second, record)) {
      output[item.first] = std::move(record);
    }
  }
  return output;
}

std::vector<MaterializedRecord>
DynamicWriteManager::collect_all_delta_records_at_internal(
    std::uint64_t read_sequence) const {
  std::vector<std::pair<NodeId, VersionShard::Version>> visible;
  for (const auto& shard : version_shards_) {
    std::shared_lock<std::shared_mutex> lock(shard->mutex);
    for (const auto& item : shard->versions) {
      for (auto it = item.second.rbegin(); it != item.second.rend(); ++it) {
        if (it->sequence_id <= read_sequence) {
          visible.emplace_back(item.first, *it);
          break;
        }
      }
    }
  }
  std::vector<MaterializedRecord> output;
  output.reserve(visible.size());
  for (const auto& item : visible) {
    MaterializedRecord record;
    if (materialize(item.first, item.second, record)) {
      output.push_back(std::move(record));
    }
  }
  std::sort(output.begin(), output.end(),
            [](const MaterializedRecord& lhs, const MaterializedRecord& rhs) {
              return lhs.node_id < rhs.node_id;
            });
  return output;
}

std::vector<MaterializedRecord>
DynamicWriteManager::collect_all_delta_records_at(
    std::uint64_t read_sequence) const {
  return collect_all_delta_records_at_internal(read_sequence);
}

std::vector<MaterializedRecord>
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

std::vector<MaterializedRecord> DynamicWriteManager::search_delta_pq_at(
    const float* query, std::uint32_t dim, std::size_t topk,
    std::uint64_t read_sequence, std::uint64_t exclusive_lower_sequence,
    DeltaSearchStats* stats) const {
  if (query == nullptr || dim == 0 || topk == 0 || !options_.pq_model ||
      !options_.pq_model->enabled() || options_.pq_model->dim() != dim) {
    return {};
  }
  struct Candidate {
    NodeId node_id = 0;
    VersionShard::Version version;
    float distance = 0.0f;
  };
  std::vector<Candidate> visible;
  for (const auto& shard : version_shards_) {
    std::shared_lock<std::shared_mutex> lock(shard->mutex);
    for (const auto& item : shard->versions) {
      for (auto it = item.second.rbegin(); it != item.second.rend(); ++it) {
        if (it->sequence_id <= read_sequence) {
          if (it->sequence_id > exclusive_lower_sequence && !it->deleted &&
              it->dim == dim) {
            visible.push_back(Candidate{item.first, *it, 0.0f});
          }
          break;
        }
      }
    }
  }
  if (stats) stats->visible_versions = visible.size();
  const auto adc_table = options_.pq_model->build_adc_table(query);
  for (auto& candidate : visible) {
    if (candidate.version.pq_code.size() == options_.pq_model->subspaces()) {
      candidate.distance = options_.pq_model->adc_distance_code(
          candidate.version.pq_code.data(), adc_table);
      if (stats) ++stats->pq_scanned;
    } else {
      candidate.distance = std::numeric_limits<float>::max();
    }
  }
  const std::size_t candidate_limit = std::min<std::size_t>(
      visible.size(), std::min<std::size_t>(256, std::max(
          options_.delta_candidate_top_l, topk * std::size_t{8})));
  const auto candidate_less = [](const Candidate& lhs, const Candidate& rhs) {
    return lhs.distance == rhs.distance ? lhs.node_id < rhs.node_id
                                        : lhs.distance < rhs.distance;
  };
  if (candidate_limit < visible.size()) {
    std::nth_element(visible.begin(), visible.begin() + candidate_limit,
                     visible.end(), candidate_less);
    visible.resize(candidate_limit);
  }
  std::sort(visible.begin(), visible.end(), candidate_less);
  std::vector<ScoredRecord> scored;
  scored.reserve(visible.size());
  for (const auto& candidate : visible) {
    MaterializedRecord record;
    if (!materialize(candidate.node_id, candidate.version, record) ||
        record.vector.size() != dim) {
      continue;
    }
    if (stats) {
      ++stats->candidates_reranked;
      ++stats->vector_reads;
      stats->bytes_read += record.vector.size() * sizeof(float);
    }
    const float exact_distance = squared_l2(query, record.vector.data(), dim);
    scored.push_back(ScoredRecord{std::move(record), exact_distance});
  }
  const auto count = std::min(topk, scored.size());
  if (count < scored.size()) {
    std::nth_element(scored.begin(), scored.begin() + count, scored.end(),
                     scored_less);
    scored.resize(count);
  }
  std::sort(scored.begin(), scored.end(), scored_less);
  std::vector<MaterializedRecord> output;
  output.reserve(scored.size());
  for (auto& item : scored) {
    output.push_back(std::move(item.record));
  }
  return output;
}

MemoryBudgetSnapshot DynamicWriteManager::memory_budget_snapshot() const {
  auto output = memory_budget_.snapshot();
  output.bytes[static_cast<std::size_t>(MemoryDomain::kDeltaControlled)] +=
      frozen_resident_bytes_.load(std::memory_order_relaxed) +
      (memtable_ ? memtable_->resident_bytes() : 0);
  return output;
}

DynamicVisibleStats DynamicWriteManager::visible_stats(
    std::uint64_t exclusive_lower_sequence,
    std::uint64_t read_sequence) const {
  DynamicVisibleStats output;
  for (const auto& shard : version_shards_) {
    std::shared_lock<std::shared_mutex> lock(shard->mutex);
    for (const auto& item : shard->versions) {
      for (auto it = item.second.rbegin(); it != item.second.rend(); ++it) {
        if (it->sequence_id > read_sequence) continue;
        if (it->sequence_id <= exclusive_lower_sequence) break;
        ++output.unique_records;
        if (it->deleted) ++output.deleted_records;
        output.persisted_logical_bytes +=
            it->locator.length != 0
                ? it->locator.length
                : static_cast<std::uint64_t>(it->dim) * sizeof(float);
        break;
      }
    }
  }
  return output;
}

std::uint64_t DynamicWriteManager::checkpoint_versions(
    std::uint64_t incorporated_sequence) {
  std::uint64_t released = 0;
  for (const auto& shard : version_shards_) {
    std::unique_lock<std::shared_mutex> lock(shard->mutex);
    for (auto it = shard->versions.begin(); it != shard->versions.end();) {
      auto& versions = it->second;
      auto keep = std::remove_if(
          versions.begin(), versions.end(), [&](const VersionShard::Version& version) {
            if (version.sequence_id > incorporated_sequence) return false;
            released += sizeof(VersionShard::Version) +
                        version.pq_code.capacity();
            return true;
          });
      versions.erase(keep, versions.end());
      if (versions.empty()) it = shard->versions.erase(it);
      else ++it;
    }
  }
  memory_budget_.release(MemoryDomain::kDeltaControlled, released);
  return released;
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
    auto located = sstables_.front()->scan_all_located();
    std::vector<MaterializedRecord> redirected;
    redirected.reserve(located.size());
    for (auto& item : located) {
      item.first.locator = item.second;
      redirected.push_back(std::move(item.first));
    }
    redirect_records(redirected);
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
  flush_queue_space_condition_.notify_all();
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

std::vector<SearchResult> merge_base_and_delta_candidates(
    const std::vector<SearchResult>& base_results,
    const std::vector<MaterializedRecord>& delta_records, const float* query,
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
