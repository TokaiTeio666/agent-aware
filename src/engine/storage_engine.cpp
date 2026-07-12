#include "engine/storage_engine.h"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <algorithm>

namespace agent_aware {

SharedPackedGraphRuntime::SharedPackedGraphRuntime(
    const PackedGraphEngineConfig& config)
    : index_(std::make_shared<PackedDiskGraphIndex>(config.index_path,
                                                    config.pq_model)) {
  index_->configure_cache(config.cache_policy, config.cache_pages,
                          config.protect_hot_pages,
                          config.hot_degree_threshold);
  index_->configure_io(config.io_mode, config.io_batch_size, config.io_depth);
}

PackedGraphEngine::PackedGraphEngine(PackedGraphEngineConfig config)
    : config_(std::move(config)),
      runtime_(config_.shared_runtime
                   ? config_.shared_runtime
                   : std::make_shared<SharedPackedGraphRuntime>(config_)) {
  config_.search.pq_model =
      config_.pq_model ? config_.pq_model.get() : &index().pq_model();
  config_.search.adc_enable = true;
  generation_ = PackedGraphGeneration::open_or_create(
      config_.index_path, index().shared_pq_model());
  const std::uint32_t base_max = metadata().vector_count == 0
                                     ? 0
                                     : static_cast<std::uint32_t>(metadata().vector_count - 1);
  max_known_node_id_.store(std::max(base_max, generation_->max_node_id()));
  if (config_.dynamic_manager && generation_->incorporated_sequence() > 0) {
    config_.dynamic_manager->checkpoint_versions(
        generation_->incorporated_sequence());
  }
  auto_merge_enabled_ = config_.enable_auto_graph_merge;
  merge_thread_ = std::thread(&PackedGraphEngine::merge_loop, this);
}

PackedGraphEngine::~PackedGraphEngine() {
  {
    std::lock_guard<std::mutex> lock(merge_mutex_);
    merge_stop_.store(true, std::memory_order_release);
  }
  merge_condition_.notify_all();
  if (merge_thread_.joinable()) merge_thread_.join();
}

EngineSearchResult PackedGraphEngine::search_one(const float* query,
                                                 std::size_t top_k) {
  DiskGraphSearchConfig search;
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    search = config_.search;
  }
  search.top_k = top_k;

  EngineSearchResult output;
  const auto generation = std::atomic_load(&generation_);
  output.stats.base_generation = generation ? generation->id() : 0;
  output.stats.base_sequence =
      generation ? generation->incorporated_sequence() : 0;
  // 访问 base graph 前先捕获动态读序列；后续 delta lookup 和 merge 必须使用同一个逻辑快照。
  const auto snapshot_start = std::chrono::steady_clock::now();
  const std::uint64_t read_sequence =
      config_.dynamic_manager ? config_.dynamic_manager->current_sequence() : 0;
  output.stats.timing.dynamic_snapshot_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - snapshot_start)
          .count();

  const auto base_start = std::chrono::steady_clock::now();
  const auto graph_result = index().search_one(query, search);
  output.stats.timing.base_search_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - base_start)
          .count();
  if (config_.dynamic_manager) {
    std::vector<std::uint32_t> base_ids;
    base_ids.reserve(graph_result.topk.size());
    for (const auto& result : graph_result.topk) {
      base_ids.push_back(result.id);
    }
    // base id 可能已有更新版本或 tombstone，合并新增 id 前需要显式查一次动态层。
    const auto latest_start = std::chrono::steady_clock::now();
    std::unordered_map<NodeId, MaterializedRecord> latest_base_records;
    if (generation) {
      for (const auto id : base_ids) {
        MaterializedRecord record;
        if (generation->resolve(id, record)) {
          latest_base_records[id] = std::move(record);
        }
      }
    }
    auto active_base_records = config_.dynamic_manager->latest_records_for(
        base_ids, read_sequence, true);
    for (auto& item : active_base_records) {
      if (item.second.sequence_id > output.stats.base_sequence) {
        latest_base_records[item.first] = std::move(item.second);
      }
    }
    output.stats.timing.latest_record_lookup_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - latest_start)
            .count();

    const auto delta_start = std::chrono::steady_clock::now();
    std::vector<MaterializedRecord> delta_records;
    if (generation) {
      delta_records = generation->search(
          query, metadata().dim, top_k,
          std::min<std::size_t>(256, std::max<std::size_t>(64, top_k * 8)));
    }
    auto active_delta = config_.dynamic_manager->search_delta_pq_at(
        query, metadata().dim, top_k, read_sequence,
        output.stats.base_sequence, &output.stats.delta);
    delta_records.insert(delta_records.end(),
                         std::make_move_iterator(active_delta.begin()),
                         std::make_move_iterator(active_delta.end()));
    output.stats.timing.delta_search_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - delta_start)
            .count();
    delta_records.reserve(delta_records.size() + latest_base_records.size());
    for (auto& item : latest_base_records) {
      delta_records.push_back(std::move(item.second));
    }
    // 最终 top-k 会用所有可见 delta 记录的精确向量计算，保证删除和更新覆盖近似 base 结果。
    const auto merge_start = std::chrono::steady_clock::now();
    output.topk = dynamic::merge_base_and_delta_candidates(
        graph_result.topk, delta_records, query, metadata().dim, top_k);
    output.stats.timing.merge_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - merge_start)
            .count();
  } else {
    output.topk = graph_result.topk;
  }
  output.stats.used_graph_path = true;
  output.stats.dynamic_read_sequence = read_sequence;
  output.stats.graph = graph_result.stats;
  return output;
}

bool PackedGraphEngine::trigger_merge() {
  std::lock_guard<std::mutex> lock(merge_mutex_);
  if (merge_status_.phase == MergeStatus::Phase::kSnapshot ||
      merge_status_.phase == MergeStatus::Phase::kWriting ||
      merge_status_.phase == MergeStatus::Phase::kPublishing) return false;
  merge_requested_ = true;
  merge_cancel_.store(false, std::memory_order_release);
  merge_condition_.notify_one();
  return true;
}

bool PackedGraphEngine::cancel_merge() {
  std::lock_guard<std::mutex> lock(merge_mutex_);
  if (!merge_status_.cancellable) return false;
  merge_cancel_.store(true, std::memory_order_release);
  return true;
}

void PackedGraphEngine::set_auto_merge_enabled(bool enabled) {
  {
    std::lock_guard<std::mutex> lock(merge_mutex_);
    auto_merge_enabled_ = enabled;
  }
  merge_condition_.notify_all();
}

PackedGraphEngine::MergeStatus PackedGraphEngine::merge_status() const {
  std::lock_guard<std::mutex> lock(merge_mutex_);
  return merge_status_;
}

std::uint64_t PackedGraphEngine::base_generation() const {
  const auto value = std::atomic_load(&generation_);
  return value ? value->id() : 0;
}

std::uint64_t PackedGraphEngine::base_sequence() const {
  const auto value = std::atomic_load(&generation_);
  return value ? value->incorporated_sequence() : 0;
}

void PackedGraphEngine::merge_loop() {
  auto last_success = std::chrono::steady_clock::now();
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(merge_mutex_);
      if (auto_merge_enabled_) {
        merge_condition_.wait_for(
            lock, std::chrono::seconds(1),
            [&] {
              return merge_stop_.load(std::memory_order_acquire) ||
                     merge_requested_;
            });
        if (!merge_requested_ &&
            !merge_stop_.load(std::memory_order_acquire) &&
            config_.dynamic_manager) {
          const auto current = std::atomic_load(&generation_);
          const auto visible = config_.dynamic_manager->visible_stats(
              current ? current->incorporated_sequence() : 0,
              config_.dynamic_manager->current_sequence());
          const bool interval_due =
              visible.unique_records > 0 &&
              std::chrono::steady_clock::now() - last_success >=
                  std::chrono::milliseconds(config_.graph_merge_interval_ms);
          const double delete_ratio = visible.unique_records == 0
                                          ? 0.0
                                          : static_cast<double>(visible.deleted_records) /
                                                visible.unique_records;
          merge_requested_ =
              visible.unique_records >= config_.graph_merge_records ||
              visible.persisted_logical_bytes >=
                  config_.graph_merge_persisted_bytes ||
              delete_ratio >= config_.graph_merge_delete_ratio || interval_due;
        }
      } else {
        merge_condition_.wait(lock,
                              [&] {
                                return merge_stop_.load(std::memory_order_acquire) ||
                                       merge_requested_;
                              });
      }
      if (merge_stop_.load(std::memory_order_acquire)) return;
      if (!merge_requested_) continue;
      merge_requested_ = false;
      merge_status_ = {};
      merge_status_.phase = MergeStatus::Phase::kSnapshot;
      merge_status_.cancellable = true;
    }
    try {
      const auto previous = std::atomic_load(&generation_);
      const std::uint64_t cutoff = config_.dynamic_manager
                                       ? config_.dynamic_manager->current_sequence()
                                       : 0;
      const auto snapshot = config_.dynamic_manager
                                ? config_.dynamic_manager->snapshot(cutoff)
                                : dynamic::DynamicSnapshot{};
      {
        std::lock_guard<std::mutex> lock(merge_mutex_);
        merge_status_.generation = previous ? previous->id() + 1 : 1;
        merge_status_.from_sequence = previous
                                          ? previous->incorporated_sequence()
                                          : 0;
        merge_status_.to_sequence = cutoff;
        merge_status_.processed_records = snapshot.records.size();
        for (const auto& record : snapshot.records) {
          merge_status_.workspace_bytes += sizeof(MaterializedRecord) +
              record.vector.capacity() * sizeof(float);
        }
        if (merge_cancel_.load(std::memory_order_acquire)) {
          merge_status_.phase = MergeStatus::Phase::kCancelled;
          merge_status_.cancellable = false;
          continue;
        }
        merge_status_.phase = MergeStatus::Phase::kWriting;
      }
      auto next = PackedGraphGeneration::publish(
          config_.index_path, *previous, snapshot.records, cutoff,
          index().shared_pq_model(), &merge_cancel_);
      {
        std::lock_guard<std::mutex> lock(merge_mutex_);
        merge_status_.phase = MergeStatus::Phase::kPublishing;
        merge_status_.cancellable = false;
      }
      std::atomic_store(&generation_, std::move(next));
      while (previous.use_count() > 1 &&
             !merge_stop_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const auto current_generation = std::atomic_load(&generation_);
      if (current_generation) current_generation->cleanup_obsolete_segments();
      const std::uint64_t reclaimed = config_.dynamic_manager
                                          ? config_.dynamic_manager->checkpoint_versions(cutoff)
                                          : 0;
      {
        std::lock_guard<std::mutex> lock(merge_mutex_);
        merge_status_.reclaimed_bytes = reclaimed;
        merge_status_.phase = MergeStatus::Phase::kComplete;
        last_success = std::chrono::steady_clock::now();
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(merge_mutex_);
      merge_status_.phase =
          merge_cancel_.load(std::memory_order_acquire)
              ? MergeStatus::Phase::kCancelled
              : MergeStatus::Phase::kFailed;
      merge_status_.cancellable = false;
      merge_status_.error = error.what();
    }
  }
}

void PackedGraphEngine::insert(std::uint32_t id, const float* vector) {
  if (!config_.dynamic_manager) {
    throw std::runtime_error("PackedGraphEngine insert requires DynamicWriteManager");
  }
  const auto maximum = max_known_node_id_.load(std::memory_order_acquire);
  if (static_cast<std::uint64_t>(id) >
      static_cast<std::uint64_t>(maximum) + 1025ULL) {
    throw std::runtime_error("PackedGraphEngine insert node id gap exceeds 1024");
  }
  if (!config_.dynamic_manager->insert(id, vector, metadata().dim)) {
    throw std::runtime_error("PackedGraphEngine insert failed");
  }
  auto current = maximum;
  while (current < id && !max_known_node_id_.compare_exchange_weak(current, id)) {
  }
}

void PackedGraphEngine::update(std::uint32_t id, const float* vector) {
  if (!config_.dynamic_manager) {
    throw std::runtime_error("PackedGraphEngine update requires DynamicWriteManager");
  }
  if (!config_.dynamic_manager->update(id, vector, metadata().dim)) {
    throw std::runtime_error("PackedGraphEngine update failed");
  }
}

void PackedGraphEngine::erase(std::uint32_t id) {
  if (!config_.dynamic_manager) {
    throw std::runtime_error("PackedGraphEngine erase requires DynamicWriteManager");
  }
  if (!config_.dynamic_manager->erase(id)) {
    throw std::runtime_error("PackedGraphEngine erase failed");
  }
}

void PackedGraphEngine::configure_runtime(std::size_t cache_pages,
                                          std::size_t io_depth,
                                          bool prefetch_enabled,
                                          std::size_t prefetch_top_k) {
  std::lock_guard<std::mutex> lock(config_mutex_);
  config_.cache_pages = cache_pages;
  config_.io_depth = io_depth;
  config_.search.prefetch_policy = prefetch_enabled ? "topology" : "none";
  config_.search.prefetch_top_k = prefetch_enabled ? prefetch_top_k : 0;
  config_.search.prefetch_max_inflight = prefetch_enabled ? io_depth : 0;
  index().configure_cache(config_.cache_policy, cache_pages,
                         config_.protect_hot_pages,
                         config_.hot_degree_threshold);
  index().configure_io(config_.io_mode, config_.io_batch_size, io_depth);
}

}  // namespace agent_aware
