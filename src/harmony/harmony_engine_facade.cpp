#include "harmony/harmony_engine_facade.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace agent_aware::harmony {
namespace {

bool same_pq(const PqAdcModel& lhs, const PqAdcModel& rhs) {
  return lhs.dim() == rhs.dim() && lhs.subspaces() == rhs.subspaces() &&
         lhs.centroids() == rhs.centroids() &&
         lhs.vector_count() == rhs.vector_count() &&
         lhs.offsets() == rhs.offsets() && lhs.codebooks() == rhs.codebooks() &&
         lhs.codes() == rhs.codes();
}

}  // namespace

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk: return "E_OK";
    case ErrorCode::kNotInitialized: return "E_NOT_INITIALIZED";
    case ErrorCode::kAlreadyInitialized: return "E_ALREADY_INITIALIZED";
    case ErrorCode::kBusy: return "E_BUSY";
    case ErrorCode::kInvalidArgument: return "E_INVALID_ARGUMENT";
    case ErrorCode::kDimensionMismatch: return "E_DIMENSION_MISMATCH";
    case ErrorCode::kIndexNotFound: return "E_INDEX_NOT_FOUND";
    case ErrorCode::kIndexIncompatible: return "E_INDEX_INCOMPATIBLE";
    case ErrorCode::kIndexCorrupted: return "E_INDEX_CORRUPTED";
    case ErrorCode::kChecksumMismatch: return "E_CHECKSUM_MISMATCH";
    case ErrorCode::kIoError: return "E_IO";
    case ErrorCode::kWalError: return "E_WAL";
    case ErrorCode::kCancelled: return "E_CANCELLED";
    case ErrorCode::kResourceExhausted: return "E_RESOURCE_EXHAUSTED";
    case ErrorCode::kInternal: return "E_INTERNAL";
  }
  return "E_INTERNAL";
}

HarmonyEngineFacade::~HarmonyEngineFacade() { (void)close(); }

StatusOr<NativeEngineInfo> HarmonyEngineFacade::open(
    const HarmonyEngineConfig& config) {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (state_ == State::kReady || state_ == State::kOpening) {
    return {{ErrorCode::kAlreadyInitialized, "Engine is already open", false}, {}};
  }
  if (config.expected_dimension == 0 || config.base_index_path.empty() ||
      config.dynamic_dir.empty() ||
      config.io_batch_size == 0 || config.io_depth == 0 ||
      config.search_width == 0 || config.entry_count == 0 ||
      config.delta_candidate_top_l == 0 || config.memtable_flush_bytes == 0 ||
      config.delta_soft_limit_bytes == 0 ||
      config.delta_soft_limit_bytes > config.delta_high_limit_bytes ||
      config.delta_high_limit_bytes > config.delta_hard_limit_bytes) {
    return {{ErrorCode::kInvalidArgument, "Invalid engine configuration", false}, {}};
  }
  if (!std::filesystem::is_regular_file(config.base_index_path) ||
      (!config.pq_model_path.empty() &&
       !std::filesystem::is_regular_file(config.pq_model_path))) {
    return {{ErrorCode::kIndexNotFound, "Index resource is unavailable", false}, {}};
  }

  state_ = State::kOpening;
  try {
    std::shared_ptr<const PqAdcModel> external_pq;
    if (!config.pq_model_path.empty()) {
      external_pq = std::make_shared<const PqAdcModel>(
          PqAdcModel::read_file(config.pq_model_path));
      if (external_pq->dim() != config.expected_dimension) {
        throw std::invalid_argument("dimension mismatch");
      }
    }
    std::filesystem::create_directories(config.dynamic_dir);
    // Dynamic recovery needs the immutable packed PQ codebook so recovered
    // versions can rebuild compact PQ codes without retaining raw vectors.
    PackedDiskGraphIndex packed_probe(config.base_index_path, external_pq);
    const auto shared_pq = packed_probe.shared_pq_model();
    dynamic::DynamicWriteOptions dynamic_options;
    dynamic_options.dynamic_dir = config.dynamic_dir;
    dynamic_options.pq_model = shared_pq;
    dynamic_options.memtable_flush_bytes = config.memtable_flush_bytes;
    dynamic_options.memtable_flush_records = config.memtable_flush_records;
    dynamic_options.delta_candidate_top_l = config.delta_candidate_top_l;
    dynamic_options.delta_soft_limit_bytes = config.delta_soft_limit_bytes;
    dynamic_options.delta_high_limit_bytes = config.delta_high_limit_bytes;
    dynamic_options.delta_hard_limit_bytes = config.delta_hard_limit_bytes;
    auto manager =
        std::make_shared<dynamic::DynamicWriteManager>(dynamic_options);
    if (!manager->open()) throw std::runtime_error("WAL open failed");

    PackedGraphEngineConfig engine_config;
    engine_config.index_path = config.base_index_path;
    engine_config.cache_policy = config.cache_policy;
    engine_config.cache_pages = config.cache_pages;
    engine_config.io_mode = config.io_mode;
    engine_config.io_batch_size = config.io_batch_size;
    engine_config.io_depth = config.io_depth;
    engine_config.pq_model = shared_pq;
    engine_config.dynamic_manager = manager;
    engine_config.enable_auto_graph_merge = config.enable_auto_graph_merge;
    engine_config.graph_merge_records = config.graph_merge_records;
    engine_config.graph_merge_persisted_bytes =
        config.graph_merge_persisted_bytes;
    engine_config.graph_merge_delete_ratio = config.graph_merge_delete_ratio;
    engine_config.graph_merge_interval_ms = config.graph_merge_interval_ms;
    engine_config.search.search_width = config.search_width;
    engine_config.search.beam_width = config.beam_width;
    engine_config.search.entry_count = config.entry_count;
    engine_config.search.rerank_topk = config.rerank_top_k;
    engine_config.search.prefetch_policy =
        config.prefetch_policy == "pq" ? "topology" : config.prefetch_policy;
    engine_config.search.prefetch_top_k = config.prefetch_submit_top_k;
    engine_config.search.prefetch_max_inflight = config.prefetch_max_inflight;
    auto engine = std::make_unique<PackedGraphEngine>(std::move(engine_config));
    if (engine->metadata().dim != config.expected_dimension) {
      throw std::invalid_argument("dimension mismatch");
    }
    if (external_pq && !same_pq(*external_pq, engine->pq_model())) {
      throw std::runtime_error("PQ companion does not match packed index");
    }

    config_ = config;
    dynamic_manager_ = std::move(manager);
    engine_ = std::move(engine);
    metrics_ = {};
    metrics_.io = engine_->io_status();
    metrics_.dynamic_sequence = dynamic_manager_->current_sequence();
    metrics_.cache_capacity_pages = engine_->cache_capacity_pages();
    metrics_.cache_resident_pages = engine_->cache_resident_pages();
    metrics_.memory_budget_bytes =
        metrics_.cache_capacity_pages * engine_->metadata().page_size;
    metrics_.base_graph_cache_bytes = metrics_.memory_budget_bytes;
    metrics_.base_generation = engine_->base_generation();
    metrics_.base_sequence = engine_->base_sequence();
    profile_ = ResourceProfile::kNormal;
    state_ = State::kReady;

    NativeEngineInfo info;
    info.dimension = engine_->metadata().dim;
    info.base_vector_count = engine_->metadata().vector_count;
    info.base_page_count = engine_->metadata().page_count;
    info.dynamic_sequence = dynamic_manager_->current_sequence();
    info.cache_policy = config.cache_policy;
    info.cache_pages = engine_->cache_capacity_pages();
    info.requested_io_mode = engine_->io_status().requested_mode;
    info.effective_io_mode = engine_->io_status().effective_mode;
    info.io_fallback_reason = engine_->io_status().fallback_reason;
    info.pq_enabled = engine_->pq_model().enabled();
    info.pq_subspaces = engine_->pq_model().subspaces();
    info.index_format_version = engine_->metadata().format_version;
    return {Status::Ok(), std::move(info)};
  } catch (const std::exception& error) {
    if (dynamic_manager_) (void)dynamic_manager_->close();
    engine_.reset();
    dynamic_manager_.reset();
    state_ = State::kFailed;
    return {from_exception(error), {}};
  }
}

Status HarmonyEngineFacade::validate_vector(const float* vector,
                                            std::uint32_t dim) const {
  if (vector == nullptr) {
    return {ErrorCode::kInvalidArgument, "Vector is required", false};
  }
  if (dim != config_.expected_dimension) {
    return {ErrorCode::kDimensionMismatch, "Vector dimension mismatch", false};
  }
  for (std::uint32_t i = 0; i < dim; ++i) {
    if (!std::isfinite(vector[i])) {
      return {ErrorCode::kInvalidArgument, "Vector contains a non-finite value", false};
    }
  }
  return Status::Ok();
}

StatusOr<EngineSearchResult> HarmonyEngineFacade::search(
    const float* query, std::uint32_t dim, std::size_t top_k) {
  std::shared_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !engine_) {
    return {{ErrorCode::kNotInitialized, "Engine is not ready", false}, {}};
  }
  const Status valid = validate_vector(query, dim);
  if (!valid.ok()) return {valid, {}};
  if (top_k == 0 || top_k > 1000) {
    return {{ErrorCode::kInvalidArgument, "topK must be in 1..1000", false}, {}};
  }
  try {
    auto result = engine_->search_one(query, top_k);
    add_search_stats(result.stats.graph);
    return {Status::Ok(), std::move(result)};
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.total_io_errors;
    return {from_exception(error), {}};
  }
}

Status HarmonyEngineFacade::upsert(std::uint32_t id, const float* vector,
                                   std::uint32_t dim, WriteMode mode) {
  std::shared_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !engine_) {
    return {ErrorCode::kNotInitialized, "Engine is not ready", false};
  }
  const Status valid = validate_vector(vector, dim);
  if (!valid.ok()) return valid;
  try {
    if (mode == WriteMode::kInsert) engine_->insert(id, vector);
    else engine_->update(id, vector);
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.total_writes;
    metrics_.dynamic_sequence = dynamic_manager_->current_sequence();
    return Status::Ok();
  } catch (const std::exception& error) {
    return from_exception(error);
  }
}

Status HarmonyEngineFacade::erase(std::uint32_t id) {
  std::shared_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !engine_) {
    return {ErrorCode::kNotInitialized, "Engine is not ready", false};
  }
  try {
    engine_->erase(id);
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.total_deletes;
    metrics_.dynamic_sequence = dynamic_manager_->current_sequence();
    return Status::Ok();
  } catch (const std::exception& error) {
    return from_exception(error);
  }
}

Status HarmonyEngineFacade::set_resource_profile(ResourceProfile profile) {
  std::unique_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !engine_) {
    return {ErrorCode::kNotInitialized, "Engine is not ready", false};
  }
  if (profile == profile_) return Status::Ok();
  const auto& metadata = engine_->metadata();
  const std::size_t raw_bytes = static_cast<std::size_t>(metadata.vector_count) *
                                metadata.dim * sizeof(float);
  std::size_t percent = 20;
  std::size_t depth = 8;
  std::size_t prefetch = 4;
  if (profile == ResourceProfile::kConstrained) {
    percent = 10; depth = 4; prefetch = 2;
  } else if (profile == ResourceProfile::kCritical) {
    percent = 4; depth = 1; prefetch = 0;
  }
  const std::size_t pages = std::max<std::size_t>(
      1, (raw_bytes * percent / 100) / metadata.page_size);
  try {
    engine_->configure_runtime(pages, depth, prefetch > 0, prefetch);
    engine_->set_auto_merge_enabled(
        profile != ResourceProfile::kCritical && config_.enable_auto_graph_merge);
    profile_ = profile;
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.io = engine_->io_status();
    metrics_.cache_capacity_pages = engine_->cache_capacity_pages();
    metrics_.cache_resident_pages = engine_->cache_resident_pages();
    metrics_.memory_budget_bytes = pages * metadata.page_size;
    return Status::Ok();
  } catch (const std::exception& error) {
    return from_exception(error);
  }
}

Status HarmonyEngineFacade::flush() {
  std::shared_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !dynamic_manager_) {
    return {ErrorCode::kNotInitialized, "Engine is not ready", false};
  }
  if (!dynamic_manager_->flush()) {
    return {ErrorCode::kWalError, "Dynamic flush failed", true};
  }
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  ++metrics_.total_flushes;
  metrics_.dynamic_sequence = dynamic_manager_->current_sequence();
  return Status::Ok();
}

Status HarmonyEngineFacade::trigger_merge() {
  std::shared_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !engine_) {
    return {ErrorCode::kNotInitialized, "Engine is not ready", false};
  }
  if (!engine_->trigger_merge()) {
    return {ErrorCode::kBusy, "Graph merge is already running", true};
  }
  return Status::Ok();
}

StatusOr<PackedGraphEngine::MergeStatus>
HarmonyEngineFacade::merge_status() const {
  std::shared_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !engine_) {
    return {{ErrorCode::kNotInitialized, "Engine is not ready", false}, {}};
  }
  return {Status::Ok(), engine_->merge_status()};
}

Status HarmonyEngineFacade::cancel_merge() {
  std::shared_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !engine_) {
    return {ErrorCode::kNotInitialized, "Engine is not ready", false};
  }
  if (!engine_->cancel_merge()) {
    return {ErrorCode::kBusy, "Graph merge cannot be cancelled", false};
  }
  return Status::Ok();
}

StatusOr<EngineMetricsSnapshot> HarmonyEngineFacade::metrics() const {
  std::shared_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  if (state_ != State::kReady || !engine_) {
    return {{ErrorCode::kNotInitialized, "Engine is not ready", false}, {}};
  }
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  EngineMetricsSnapshot snapshot = metrics_;
  snapshot.io = engine_->io_status();
  snapshot.dynamic_sequence = dynamic_manager_->current_sequence();
  snapshot.cache_capacity_pages = engine_->cache_capacity_pages();
  snapshot.cache_resident_pages = engine_->cache_resident_pages();
  const auto budget = dynamic_manager_->memory_budget_snapshot();
  snapshot.delta_controlled_bytes =
      budget.bytes[static_cast<std::size_t>(MemoryDomain::kDeltaControlled)];
  snapshot.base_graph_cache_bytes =
      engine_->cache_resident_pages() * engine_->metadata().page_size;
  snapshot.base_generation = engine_->base_generation();
  snapshot.base_sequence = engine_->base_sequence();
  const auto merge = engine_->merge_status();
  snapshot.merge_workspace_bytes = merge.workspace_bytes;
  snapshot.total_graph_merges =
      merge.phase == PackedGraphEngine::MergeStatus::Phase::kComplete ? 1 : 0;
  snapshot.failed_graph_merges =
      merge.phase == PackedGraphEngine::MergeStatus::Phase::kFailed ? 1 : 0;
  return {Status::Ok(), std::move(snapshot)};
}

Status HarmonyEngineFacade::close() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (state_ == State::kClosed) return Status::Ok();
  std::unique_lock<std::shared_mutex> boundary(query_boundary_mutex_);
  state_ = State::kClosing;
  engine_.reset();
  Status status = Status::Ok();
  if (dynamic_manager_ && !dynamic_manager_->close()) {
    status = {ErrorCode::kWalError, "Dynamic close failed", true};
  }
  dynamic_manager_.reset();
  state_ = State::kClosed;
  return status;
}

Status HarmonyEngineFacade::from_exception(const std::exception& error) {
  const std::string text = error.what();
  if (text.find("dimension") != std::string::npos) {
    return {ErrorCode::kDimensionMismatch, "Vector dimension mismatch", false};
  }
  if (text.find("version") != std::string::npos ||
      text.find("obsolete") != std::string::npos) {
    return {ErrorCode::kIndexIncompatible, "Index format is incompatible", false};
  }
  if (text.find("magic") != std::string::npos ||
      text.find("PQ companion") != std::string::npos ||
      text.find("metadata") != std::string::npos ||
      text.find("Truncated") != std::string::npos) {
    return {ErrorCode::kIndexCorrupted, "Index resource is corrupted", false};
  }
  if (text.find("WAL") != std::string::npos ||
      text.find("flush") != std::string::npos) {
    return {ErrorCode::kWalError, "Dynamic storage operation failed", true};
  }
  if (text.find("I/O") != std::string::npos ||
      text.find("pread") != std::string::npos ||
      text.find("read") != std::string::npos ||
      text.find("open") != std::string::npos) {
    return {ErrorCode::kIoError, "Native I/O operation failed", true};
  }
  return {ErrorCode::kInternal, "Native engine operation failed", false};
}

void HarmonyEngineFacade::add_search_stats(const DiskGraphSearchStats& stats) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  ++metrics_.total_searches;
#define ADD_FIELD(name) metrics_.aggregate_search.name += stats.name
  ADD_FIELD(node_reads); ADD_FIELD(expanded); ADD_FIELD(visited);
  ADD_FIELD(page_requests); ADD_FIELD(cache_hits); ADD_FIELD(demand_reads);
  ADD_FIELD(prefetch_submitted); ADD_FIELD(prefetch_useful_pages);
  ADD_FIELD(prefetch_wasted_pages); ADD_FIELD(prefetch_ready_hit);
  ADD_FIELD(prefetch_pending_hit); ADD_FIELD(io_submits);
  ADD_FIELD(io_completions); ADD_FIELD(demand_read_wait_us);
#undef ADD_FIELD
  metrics_.dynamic_sequence = dynamic_manager_->current_sequence();
}

}  // namespace agent_aware::harmony
