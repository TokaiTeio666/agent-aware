#include <node_api.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "harmony/harmony_engine_facade.h"
#include "harmony/operation_scheduler.h"

namespace agent_aware::harmony {
namespace {

enum class AsyncOperation {
  kOpen, kSearch, kUpsert, kDelete, kProfile, kFlush, kTriggerMerge,
  kMergeStatus, kCancelMerge, kMetrics, kClose
};

struct ModuleState {
  HarmonyEngineFacade facade;
  OperationScheduler scheduler{4, 16};
};

struct AsyncContext {
  napi_env env = nullptr;
  napi_async_work work = nullptr;
  napi_deferred deferred = nullptr;
  ModuleState* module = nullptr;
  AsyncOperation operation = AsyncOperation::kSearch;
  std::string operation_name;
  HarmonyEngineConfig config;
  std::vector<float> vector;
  std::uint32_t id = 0;
  std::size_t top_k = 0;
  WriteMode write_mode = WriteMode::kInsert;
  ResourceProfile profile = ResourceProfile::kNormal;
  Status status;
  NativeEngineInfo engine_info;
  EngineSearchResult search_result;
  EngineMetricsSnapshot metrics;
  PackedGraphEngine::MergeStatus merge_status;
};

bool get_named(napi_env env, napi_value object, const char* name,
               napi_value* value) {
  bool present = false;
  return napi_has_named_property(env, object, name, &present) == napi_ok &&
         present && napi_get_named_property(env, object, name, value) == napi_ok;
}

bool get_string(napi_env env, napi_value object, const char* name,
                std::string& output, bool required = false) {
  napi_value value;
  if (!get_named(env, object, name, &value)) return !required;
  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_string) return false;
  std::size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok)
    return false;
  std::vector<char> buffer(length + 1, '\0');
  if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(),
                                 &length) != napi_ok) return false;
  output.assign(buffer.data(), length);
  return true;
}

bool value_string(napi_env env, napi_value value, std::string& output) {
  std::size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok)
    return false;
  std::vector<char> buffer(length + 1, '\0');
  if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(),
                                 &length) != napi_ok) return false;
  output.assign(buffer.data(), length);
  return true;
}

bool get_size(napi_env env, napi_value object, const char* name,
              std::size_t& output) {
  napi_value value;
  if (!get_named(env, object, name, &value)) return true;
  double number = 0;
  if (napi_get_value_double(env, value, &number) != napi_ok ||
      !std::isfinite(number) || number < 0 || std::floor(number) != number)
    return false;
  output = static_cast<std::size_t>(number);
  return true;
}

bool get_double(napi_env env, napi_value object, const char* name,
                double& output) {
  napi_value value;
  if (!get_named(env, object, name, &value)) return true;
  return napi_get_value_double(env, value, &output) == napi_ok &&
         std::isfinite(output);
}

bool get_bool(napi_env env, napi_value object, const char* name, bool& output) {
  napi_value value;
  if (!get_named(env, object, name, &value)) return true;
  return napi_get_value_bool(env, value, &output) == napi_ok;
}

bool parse_config(napi_env env, napi_value value, HarmonyEngineConfig& config) {
  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_object) return false;
  std::size_t dimension = config.expected_dimension;
  return get_string(env, value, "baseIndexPath", config.base_index_path, true) &&
         get_string(env, value, "pqModelPath", config.pq_model_path) &&
         get_string(env, value, "dynamicDir", config.dynamic_dir, true) &&
         get_string(env, value, "cachePolicy", config.cache_policy) &&
         get_string(env, value, "ioMode", config.io_mode) &&
         get_string(env, value, "prefetchPolicy", config.prefetch_policy) &&
         get_size(env, value, "expectedDimension", dimension) &&
         get_size(env, value, "cachePages", config.cache_pages) &&
         get_size(env, value, "ioBatchSize", config.io_batch_size) &&
         get_size(env, value, "ioDepth", config.io_depth) &&
         get_size(env, value, "searchWidth", config.search_width) &&
         get_size(env, value, "beamWidth", config.beam_width) &&
         get_size(env, value, "entryCount", config.entry_count) &&
         get_size(env, value, "rerankTopK", config.rerank_top_k) &&
         get_size(env, value, "prefetchSubmitTopK", config.prefetch_submit_top_k) &&
         get_size(env, value, "prefetchMaxInflight", config.prefetch_max_inflight) &&
         get_size(env, value, "deltaSoftLimitBytes", config.delta_soft_limit_bytes) &&
         get_size(env, value, "deltaHighLimitBytes", config.delta_high_limit_bytes) &&
         get_size(env, value, "deltaHardLimitBytes", config.delta_hard_limit_bytes) &&
         get_size(env, value, "memtableFlushRecords", config.memtable_flush_records) &&
         get_size(env, value, "memtableFlushBytes", config.memtable_flush_bytes) &&
         get_size(env, value, "deltaCandidateTopL", config.delta_candidate_top_l) &&
         get_bool(env, value, "enableAutoGraphMerge", config.enable_auto_graph_merge) &&
         get_size(env, value, "graphMergeRecords", config.graph_merge_records) &&
         get_size(env, value, "graphMergePersistedBytes", config.graph_merge_persisted_bytes) &&
         get_double(env, value, "graphMergeDeleteRatio", config.graph_merge_delete_ratio) &&
         get_size(env, value, "graphMergeIntervalMs", config.graph_merge_interval_ms) &&
         get_size(env, value, "graphMergeMaxSegments", config.graph_merge_max_segments) &&
         dimension <= UINT32_MAX &&
         (config.expected_dimension = static_cast<std::uint32_t>(dimension), true);
}

bool copy_float32_array(napi_env env, napi_value value,
                        std::vector<float>& output) {
  bool typed = false;
  if (napi_is_typedarray(env, value, &typed) != napi_ok || !typed) return false;
  napi_typedarray_type type;
  std::size_t length = 0;
  void* data = nullptr;
  napi_value array_buffer;
  std::size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, value, &type, &length, &data,
                               &array_buffer, &byte_offset) != napi_ok ||
      type != napi_float32_array || data == nullptr)
    return false;
  const auto* floats = static_cast<const float*>(data);
  output.assign(floats, floats + length);
  return true;
}

napi_value string_value(napi_env env, const std::string& value) {
  napi_value result;
  napi_create_string_utf8(env, value.c_str(), value.size(), &result);
  return result;
}

void set_string(napi_env env, napi_value object, const char* name,
                const std::string& value) {
  napi_set_named_property(env, object, name, string_value(env, value));
}

void set_number(napi_env env, napi_value object, const char* name, double value) {
  napi_value number;
  napi_create_double(env, value, &number);
  napi_set_named_property(env, object, name, number);
}

void set_bool(napi_env env, napi_value object, const char* name, bool value) {
  napi_value boolean;
  napi_get_boolean(env, value, &boolean);
  napi_set_named_property(env, object, name, boolean);
}

napi_value create_error(napi_env env, const AsyncContext& context) {
  napi_value message = string_value(env, context.status.message);
  napi_value error;
  napi_create_error(env, nullptr, message, &error);
  set_string(env, error, "name", "MemHarmonyError");
  set_string(env, error, "code", error_code_name(context.status.code));
  set_string(env, error, "operation", context.operation_name);
  set_bool(env, error, "retryable", context.status.retryable);
  set_number(env, error, "nativeCode", context.status.native_code);
  return error;
}

napi_value create_engine_info(napi_env env, const NativeEngineInfo& info) {
  napi_value object;
  napi_create_object(env, &object);
  set_string(env, object, "apiVersion", info.api_version);
  set_string(env, object, "engineVersion", info.engine_version);
  set_number(env, object, "dimension", info.dimension);
  set_number(env, object, "baseVectorCount", info.base_vector_count);
  set_number(env, object, "basePageCount", info.base_page_count);
  set_number(env, object, "dynamicSequence", info.dynamic_sequence);
  set_string(env, object, "cachePolicy", info.cache_policy);
  set_number(env, object, "cachePages", info.cache_pages);
  set_string(env, object, "requestedIoMode", info.requested_io_mode);
  set_string(env, object, "effectiveIoMode", info.effective_io_mode);
  if (!info.io_fallback_reason.empty())
    set_string(env, object, "ioFallbackReason", info.io_fallback_reason);
  set_bool(env, object, "pqEnabled", info.pq_enabled);
  set_number(env, object, "pqSubspaces", info.pq_subspaces);
  set_number(env, object, "indexFormatVersion", info.index_format_version);
  return object;
}

napi_value create_search_response(napi_env env, const EngineSearchResult& result) {
  napi_value object, hits, stats;
  napi_create_object(env, &object);
  napi_create_array_with_length(env, result.topk.size(), &hits);
  for (std::size_t i = 0; i < result.topk.size(); ++i) {
    napi_value hit;
    napi_create_object(env, &hit);
    set_number(env, hit, "id", result.topk[i].id);
    set_number(env, hit, "distance", result.topk[i].distance);
    napi_set_element(env, hits, i, hit);
  }
  napi_set_named_property(env, object, "hits", hits);
  napi_create_object(env, &stats);
  const auto& graph = result.stats.graph;
  set_bool(env, stats, "usedGraphPath", result.stats.used_graph_path);
  set_number(env, stats, "dynamicReadSequence", result.stats.dynamic_read_sequence);
  set_number(env, stats, "baseGeneration", result.stats.base_generation);
  set_number(env, stats, "baseSequence", result.stats.base_sequence);
  set_number(env, stats, "deltaVisibleVersions", result.stats.delta.visible_versions);
  set_number(env, stats, "deltaPqScanned", result.stats.delta.pq_scanned);
  set_number(env, stats, "deltaCandidatesReranked", result.stats.delta.candidates_reranked);
  set_number(env, stats, "deltaVectorReads", result.stats.delta.vector_reads);
  set_number(env, stats, "deltaBytesRead", result.stats.delta.bytes_read);
  set_number(env, stats, "nodeReads", graph.node_reads);
  set_number(env, stats, "expanded", graph.expanded);
  set_number(env, stats, "visited", graph.visited);
  set_number(env, stats, "pageRequests", graph.page_requests);
  set_number(env, stats, "cacheHits", graph.cache_hits);
  set_number(env, stats, "demandReads", graph.demand_reads);
  set_number(env, stats, "prefetchReads", graph.prefetch_submitted);
  set_number(env, stats, "prefetchUsefulPages", graph.prefetch_useful_pages);
  set_number(env, stats, "prefetchWastedPages", graph.prefetch_wasted_pages);
  set_number(env, stats, "prefetchReadyHits", graph.prefetch_ready_hit);
  set_number(env, stats, "prefetchPendingHits", graph.prefetch_pending_hit);
  set_number(env, stats, "ioSubmits", graph.io_submits);
  set_number(env, stats, "ioCompletions", graph.io_completions);
  set_number(env, stats, "demandReadWaitUs", graph.demand_read_wait_us);
  set_number(env, stats, "dynamicSnapshotMs", result.stats.timing.dynamic_snapshot_ms);
  set_number(env, stats, "baseSearchMs", result.stats.timing.base_search_ms);
  set_number(env, stats, "deltaSearchMs", result.stats.timing.delta_search_ms);
  set_number(env, stats, "mergeMs", result.stats.timing.merge_ms);
  napi_set_named_property(env, object, "stats", stats);
  return object;
}

napi_value create_metrics(napi_env env, const EngineMetricsSnapshot& metrics) {
  napi_value object, aggregate;
  napi_create_object(env, &object);
  set_string(env, object, "requestedIoMode", metrics.io.requested_mode);
  set_string(env, object, "effectiveIoMode", metrics.io.effective_mode);
  if (!metrics.io.fallback_reason.empty())
    set_string(env, object, "ioFallbackReason", metrics.io.fallback_reason);
  set_number(env, object, "ioDepth", metrics.io.depth);
  set_number(env, object, "ioBatchSize", metrics.io.batch_size);
  set_number(env, object, "cacheCapacityPages", metrics.cache_capacity_pages);
  set_number(env, object, "cacheResidentPages", metrics.cache_resident_pages);
  set_number(env, object, "memoryBudgetBytes", metrics.memory_budget_bytes);
  set_number(env, object, "deltaControlledBytes", metrics.delta_controlled_bytes);
  set_number(env, object, "baseGraphCacheBytes", metrics.base_graph_cache_bytes);
  set_number(env, object, "mergeWorkspaceBytes", metrics.merge_workspace_bytes);
  set_number(env, object, "baseGeneration", metrics.base_generation);
  set_number(env, object, "baseSequence", metrics.base_sequence);
  set_number(env, object, "totalGraphMerges", metrics.total_graph_merges);
  set_number(env, object, "failedGraphMerges", metrics.failed_graph_merges);
  set_number(env, object, "totalSearches", metrics.total_searches);
  set_number(env, object, "totalWrites", metrics.total_writes);
  set_number(env, object, "totalDeletes", metrics.total_deletes);
  set_number(env, object, "totalFlushes", metrics.total_flushes);
  set_number(env, object, "totalIoErrors", metrics.total_io_errors);
  napi_create_object(env, &aggregate);
  set_number(env, aggregate, "pageRequests", metrics.aggregate_search.page_requests);
  set_number(env, aggregate, "cacheHits", metrics.aggregate_search.cache_hits);
  set_number(env, aggregate, "demandReads", metrics.aggregate_search.demand_reads);
  set_number(env, aggregate, "prefetchReads", metrics.aggregate_search.prefetch_submitted);
  set_number(env, aggregate, "prefetchUsefulPages", metrics.aggregate_search.prefetch_useful_pages);
  set_number(env, aggregate, "prefetchWastedPages", metrics.aggregate_search.prefetch_wasted_pages);
  napi_set_named_property(env, object, "aggregate", aggregate);
  return object;
}

const char* merge_phase_name(PackedGraphEngine::MergeStatus::Phase phase) {
  using Phase = PackedGraphEngine::MergeStatus::Phase;
  switch (phase) {
    case Phase::kIdle: return "IDLE";
    case Phase::kSnapshot: return "SNAPSHOT";
    case Phase::kWriting: return "WRITING";
    case Phase::kPublishing: return "PUBLISHING";
    case Phase::kComplete: return "COMPLETE";
    case Phase::kFailed: return "FAILED";
    case Phase::kCancelled: return "CANCELLED";
  }
  return "FAILED";
}

napi_value create_merge_status(napi_env env,
                               const PackedGraphEngine::MergeStatus& status) {
  napi_value object;
  napi_create_object(env, &object);
  set_string(env, object, "phase", merge_phase_name(status.phase));
  set_number(env, object, "generation", status.generation);
  set_number(env, object, "fromSequence", status.from_sequence);
  set_number(env, object, "toSequence", status.to_sequence);
  set_number(env, object, "processedRecords", status.processed_records);
  set_number(env, object, "reclaimedBytes", status.reclaimed_bytes);
  set_number(env, object, "workspaceBytes", status.workspace_bytes);
  set_string(env, object, "error", status.error);
  set_bool(env, object, "cancellable", status.cancellable);
  return object;
}

OperationScheduler::Kind scheduler_kind(AsyncOperation operation) {
  if (operation == AsyncOperation::kOpen || operation == AsyncOperation::kClose)
    return OperationScheduler::Kind::kLifecycle;
  if (operation == AsyncOperation::kSearch || operation == AsyncOperation::kMetrics)
    return OperationScheduler::Kind::kSearch;
  return OperationScheduler::Kind::kWrite;
}

void execute(napi_env, void* data) {
  auto& context = *static_cast<AsyncContext*>(data);
  auto permit = context.module->scheduler.acquire(scheduler_kind(context.operation));
  if (!permit) {
    context.status = {ErrorCode::kCancelled, "Native operation was cancelled", true};
    return;
  }
  switch (context.operation) {
    case AsyncOperation::kOpen: {
      auto result = context.module->facade.open(context.config);
      context.status = result.status; context.engine_info = std::move(result.value); break;
    }
    case AsyncOperation::kSearch: {
      auto result = context.module->facade.search(
          context.vector.data(), static_cast<std::uint32_t>(context.vector.size()),
          context.top_k);
      context.status = result.status; context.search_result = std::move(result.value); break;
    }
    case AsyncOperation::kUpsert:
      context.status = context.module->facade.upsert(
          context.id, context.vector.data(),
          static_cast<std::uint32_t>(context.vector.size()), context.write_mode); break;
    case AsyncOperation::kDelete:
      context.status = context.module->facade.erase(context.id); break;
    case AsyncOperation::kProfile:
      context.status = context.module->facade.set_resource_profile(context.profile); break;
    case AsyncOperation::kFlush:
      context.status = context.module->facade.flush(); break;
    case AsyncOperation::kTriggerMerge:
      context.status = context.module->facade.trigger_merge(); break;
    case AsyncOperation::kMergeStatus: {
      auto result = context.module->facade.merge_status();
      context.status = result.status;
      context.merge_status = std::move(result.value); break;
    }
    case AsyncOperation::kCancelMerge:
      context.status = context.module->facade.cancel_merge(); break;
    case AsyncOperation::kMetrics: {
      auto result = context.module->facade.metrics();
      context.status = result.status; context.metrics = std::move(result.value); break;
    }
    case AsyncOperation::kClose:
      context.status = context.module->facade.close(); break;
  }
}

void complete(napi_env env, napi_status async_status, void* data) {
  std::unique_ptr<AsyncContext> context(static_cast<AsyncContext*>(data));
  if (async_status != napi_ok && context->status.ok())
    context->status = {ErrorCode::kCancelled, "Native async work was cancelled", true};
  if (!context->status.ok()) {
    napi_reject_deferred(env, context->deferred, create_error(env, *context));
  } else {
    napi_value result;
    if (context->operation == AsyncOperation::kOpen)
      result = create_engine_info(env, context->engine_info);
    else if (context->operation == AsyncOperation::kSearch)
      result = create_search_response(env, context->search_result);
    else if (context->operation == AsyncOperation::kMetrics)
      result = create_metrics(env, context->metrics);
    else if (context->operation == AsyncOperation::kMergeStatus)
      result = create_merge_status(env, context->merge_status);
    else
      napi_get_undefined(env, &result);
    napi_resolve_deferred(env, context->deferred, result);
  }
  napi_delete_async_work(env, context->work);
}

napi_value queue(napi_env env, std::unique_ptr<AsyncContext> context) {
  napi_value promise, resource_name;
  context->env = env;
  napi_create_promise(env, &context->deferred, &promise);
  resource_name = string_value(env, context->operation_name);
  AsyncContext* raw = context.release();
  if (napi_create_async_work(env, nullptr, resource_name, execute, complete,
                             raw, &raw->work) != napi_ok ||
      napi_queue_async_work(env, raw->work) != napi_ok) {
    raw->status = {ErrorCode::kResourceExhausted,
                   "Unable to queue native async work", true};
    napi_reject_deferred(env, raw->deferred, create_error(env, *raw));
    if (raw->work) napi_delete_async_work(env, raw->work);
    delete raw;
  }
  return promise;
}

std::unique_ptr<AsyncContext> make_context(napi_env env,
                                           napi_callback_info info,
                                           AsyncOperation operation,
                                           const char* name,
                                           std::size_t& argc,
                                           napi_value* argv) {
  napi_value self;
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, &data);
  auto context = std::make_unique<AsyncContext>();
  context->module = static_cast<ModuleState*>(data);
  context->operation = operation;
  context->operation_name = name;
  return context;
}

napi_value open(napi_env env, napi_callback_info info) {
  napi_value argv[1]; std::size_t argc = 1;
  auto context = make_context(env, info, AsyncOperation::kOpen, "open", argc, argv);
  if (argc != 1 || !parse_config(env, argv[0], context->config)) {
    context->status = {ErrorCode::kInvalidArgument, "Invalid open configuration", false};
  }
  return queue(env, std::move(context));
}

napi_value search(napi_env env, napi_callback_info info) {
  napi_value argv[2]; std::size_t argc = 2;
  auto context = make_context(env, info, AsyncOperation::kSearch, "searchVector", argc, argv);
  double top_k = 0;
  if (argc != 2 || !copy_float32_array(env, argv[0], context->vector) ||
      napi_get_value_double(env, argv[1], &top_k) != napi_ok ||
      !std::isfinite(top_k) || std::floor(top_k) != top_k || top_k < 1 || top_k > 1000) {
    context->status = {ErrorCode::kInvalidArgument, "Invalid search arguments", false};
  } else context->top_k = static_cast<std::size_t>(top_k);
  return queue(env, std::move(context));
}

bool parse_id(napi_env env, napi_value value, std::uint32_t& id) {
  double number = 0;
  if (napi_get_value_double(env, value, &number) != napi_ok ||
      !std::isfinite(number) || std::floor(number) != number ||
      number < 0 || number > UINT32_MAX) return false;
  id = static_cast<std::uint32_t>(number); return true;
}

napi_value upsert(napi_env env, napi_callback_info info) {
  napi_value argv[3]; std::size_t argc = 3;
  auto context = make_context(env, info, AsyncOperation::kUpsert, "upsertVector", argc, argv);
  std::string mode;
  if (argc == 3) (void)value_string(env, argv[2], mode);
  if (argc != 3 || !parse_id(env, argv[0], context->id) ||
      !copy_float32_array(env, argv[1], context->vector) ||
      (mode != "INSERT" && mode != "UPDATE")) {
    context->status = {ErrorCode::kInvalidArgument, "Invalid upsert arguments", false};
  } else context->write_mode = mode == "INSERT" ? WriteMode::kInsert : WriteMode::kUpdate;
  return queue(env, std::move(context));
}

napi_value erase(napi_env env, napi_callback_info info) {
  napi_value argv[1]; std::size_t argc = 1;
  auto context = make_context(env, info, AsyncOperation::kDelete, "deleteVector", argc, argv);
  if (argc != 1 || !parse_id(env, argv[0], context->id))
    context->status = {ErrorCode::kInvalidArgument, "Invalid delete id", false};
  return queue(env, std::move(context));
}

napi_value profile(napi_env env, napi_callback_info info) {
  napi_value argv[1]; std::size_t argc = 1;
  auto context = make_context(env, info, AsyncOperation::kProfile, "setResourceProfile", argc, argv);
  std::string value;
  if (argc == 1) (void)value_string(env, argv[0], value);
  if (value == "NORMAL") context->profile = ResourceProfile::kNormal;
  else if (value == "CONSTRAINED") context->profile = ResourceProfile::kConstrained;
  else if (value == "CRITICAL") context->profile = ResourceProfile::kCritical;
  else context->status = {ErrorCode::kInvalidArgument, "Invalid resource profile", false};
  return queue(env, std::move(context));
}

napi_value no_args(napi_env env, napi_callback_info info,
                   AsyncOperation operation, const char* name) {
  napi_value argv[1]; std::size_t argc = 0;
  return queue(env, make_context(env, info, operation, name, argc, argv));
}
napi_value flush(napi_env e, napi_callback_info i) { return no_args(e, i, AsyncOperation::kFlush, "flush"); }
napi_value trigger_merge(napi_env e, napi_callback_info i) { return no_args(e, i, AsyncOperation::kTriggerMerge, "triggerMerge"); }
napi_value merge_status(napi_env e, napi_callback_info i) { return no_args(e, i, AsyncOperation::kMergeStatus, "getMergeStatus"); }
napi_value cancel_merge(napi_env e, napi_callback_info i) { return no_args(e, i, AsyncOperation::kCancelMerge, "cancelMerge"); }
napi_value metrics(napi_env e, napi_callback_info i) { return no_args(e, i, AsyncOperation::kMetrics, "getMetrics"); }
napi_value close(napi_env e, napi_callback_info i) { return no_args(e, i, AsyncOperation::kClose, "close"); }

void cleanup(void* data) {
  auto* module = static_cast<ModuleState*>(data);
  module->scheduler.close();
  (void)module->facade.close();
  delete module;
}

napi_value init(napi_env env, napi_value exports) {
  auto* state = new ModuleState();
  napi_add_env_cleanup_hook(env, cleanup, state);
  napi_property_descriptor properties[] = {
      {"open", nullptr, open, nullptr, nullptr, nullptr, napi_default, state},
      {"searchVector", nullptr, search, nullptr, nullptr, nullptr, napi_default, state},
      {"upsertVector", nullptr, upsert, nullptr, nullptr, nullptr, napi_default, state},
      {"deleteVector", nullptr, erase, nullptr, nullptr, nullptr, napi_default, state},
      {"setResourceProfile", nullptr, profile, nullptr, nullptr, nullptr, napi_default, state},
      {"flush", nullptr, flush, nullptr, nullptr, nullptr, napi_default, state},
      {"triggerMerge", nullptr, trigger_merge, nullptr, nullptr, nullptr, napi_default, state},
      {"getMergeStatus", nullptr, merge_status, nullptr, nullptr, nullptr, napi_default, state},
      {"cancelMerge", nullptr, cancel_merge, nullptr, nullptr, nullptr, napi_default, state},
      {"getMetrics", nullptr, metrics, nullptr, nullptr, nullptr, napi_default, state},
      {"close", nullptr, close, nullptr, nullptr, nullptr, napi_default, state},
#ifndef NDEBUG
      {"search", nullptr, search, nullptr, nullptr, nullptr, napi_default, state},
      {"upsert", nullptr, upsert, nullptr, nullptr, nullptr, napi_default, state},
      {"erase", nullptr, erase, nullptr, nullptr, nullptr, napi_default, state},
      {"stats", nullptr, metrics, nullptr, nullptr, nullptr, napi_default, state},
#endif
  };
  napi_define_properties(env, exports,
                         sizeof(properties) / sizeof(properties[0]), properties);
  return exports;
}

}  // namespace
}  // namespace agent_aware::harmony

NAPI_MODULE(agentAwareNative, agent_aware::harmony::init)
