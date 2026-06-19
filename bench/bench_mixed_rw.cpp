#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agent_aware/core/brute_force.h"
#include "agent_aware/data/dataset.h"
#include "agent_aware/dynamic/dynamic_write_manager.h"
#include "agent_aware/dynamic/manifest.h"
#include "agent_aware/engine/storage_engine.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Args {
  std::string data_path;
  std::string index_path;
  std::filesystem::path dynamic_dir = "build/p5_mixed_rw_dynamic";
  std::filesystem::path output = "build/p5_mixed_rw.csv";
  std::size_t num_operations = 1000;
  double duration_sec = 0.0;
  std::size_t read_threads = 0;
  std::size_t write_threads = 0;
  double read_ratio = -1.0;
  double write_ratio = -1.0;
  std::size_t topk = 10;
  std::size_t insert_batch_size = 1;
  bool enable_flush = true;
  bool enable_compaction = false;
  bool compaction_background = false;
  std::size_t compaction_interval_ms = 1000;
  double recall_sample_rate = 1.0;
  std::size_t base_count = 1000;
  std::size_t query_count = 256;
  std::size_t dim = 32;
  std::size_t memtable_flush_bytes = 256 * 1024;
  std::size_t dynamic_graph_degree = 16;
  std::size_t search_width = 350;
  std::size_t entry_count = 64;
  std::size_t beam_width = 16;
  std::string io_mode = "pread";
  std::size_t io_batch = 16;
  std::size_t io_depth = 32;
  std::string cache_policy = "graph-aware-2q";
  std::size_t cache_pages = 0;
  std::uint32_t seed = 42;
};

struct Scenario {
  std::string name;
  double read_ratio = 0.0;
  double write_ratio = 0.0;
};

struct Percentiles {
  double avg = 0.0;
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
};

struct CompactionAggregate {
  std::size_t attempted = 0;
  std::size_t success = 0;
  std::size_t input_record_count = 0;
  std::size_t output_record_count = 0;
  double duration_ms = 0.0;
};

struct ScenarioResult {
  std::string scenario;
  double read_ratio = 0.0;
  double write_ratio = 0.0;
  std::size_t read_threads = 0;
  std::size_t write_threads = 0;
  double duration_sec = 0.0;
  std::size_t read_ops = 0;
  std::size_t write_ops = 0;
  double read_qps = 0.0;
  double write_qps = 0.0;
  double total_qps = 0.0;
  double insert_throughput = 0.0;
  Percentiles latency;
  Percentiles read_latency;
  Percentiles write_latency;
  double recall_at_10 = 0.0;
  std::size_t recall_samples = 0;
  double recall_sample_rate = 0.0;
  std::uint64_t recall_read_sequence_min = 0;
  std::uint64_t recall_read_sequence_max = 0;
  double exact_delta_visible_count_avg = 0.0;
  double exact_deleted_count_avg = 0.0;
  double memtable_insert_avg_us = 0.0;
  double flush_duration_ms = 0.0;
  std::size_t sstable_count = 0;
  std::size_t delta_record_count = 0;
  double memory_usage_mb = 0.0;
  double disk_usage_mb = 0.0;
  double recovery_time_ms = 0.0;
  std::string search_mode = "memory_exact";
  double graph_reads_per_read = 0.0;
  double cache_hit_rate = 0.0;
  CompactionAggregate compaction;
};

struct ThreadMetrics {
  std::vector<double> op_latencies_ms;
  std::vector<double> read_latencies_ms;
  std::vector<double> write_latencies_ms;
  std::size_t read_ops = 0;
  std::size_t write_records = 0;
  double recall_sum = 0.0;
  std::size_t recall_samples = 0;
  std::uint64_t recall_sequence_min = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t recall_sequence_max = 0;
  std::size_t exact_delta_visible_sum = 0;
  std::size_t exact_deleted_sum = 0;
  agent_aware::DiskGraphSearchStats graph_stats;
};

std::string require_value(int& index, int argc, char** argv,
                          const std::string& option) {
  const std::string current = argv[index];
  const auto eq = current.find('=');
  if (eq != std::string::npos) {
    return current.substr(eq + 1);
  }
  if (index + 1 >= argc) {
    throw std::runtime_error("Missing value for " + option);
  }
  ++index;
  return argv[index];
}

bool parse_bool(const std::string& value) {
  return value == "1" || value == "true" || value == "on" ||
         value == "yes";
}

void print_help() {
  std::cout
      << "Usage: bench_mixed_rw [options]\n"
      << "  --data_path PATH             Optional fvecs base vectors\n"
      << "  --index_path PATH            Optional SSD packed graph index\n"
      << "  --dynamic_dir PATH           Dynamic WAL/SSTable directory\n"
      << "  --output PATH                CSV or JSON output path\n"
      << "  --num_operations N           Operation events per scenario\n"
      << "  --duration_sec N             Enable timed concurrent mode\n"
      << "  --read_threads N             Reader threads for concurrent mode\n"
      << "  --write_threads N            Writer threads for concurrent mode\n"
      << "  --read_ratio R               Run one custom scenario\n"
      << "  --write_ratio R              Custom scenario write ratio\n"
      << "  --recall_sample_rate R       Dynamic recall sampling rate, default 1\n"
      << "  --topk N                     Search top-k, default 10\n"
      << "  --insert_batch_size N        Inserts per write event\n"
      << "  --dynamic_graph_degree N     Delta graph neighbor count\n"
      << "  --search_width N             SSD graph expansion budget\n"
      << "  --entry_count N              SSD graph entry points\n"
      << "  --beam_width N               SSD graph beam batch width\n"
      << "  --io_mode NAME               pread, odirect, or io_uring\n"
      << "  --cache_policy NAME          none/lru/2q/graph-aware-2q\n"
      << "  --cache_pages N              SSD graph cache pages\n"
      << "  --enable_flush 0|1           Flush at end and allow auto flush\n"
      << "  --enable_compaction 0|1      Run compaction\n"
      << "  --compaction_background 0|1  Run compaction while workload runs\n"
      << "  --compaction_interval_ms N   Background compaction interval\n";
}

Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    const auto name = option.substr(0, option.find('='));
    if (name == "--data_path") {
      args.data_path = require_value(i, argc, argv, name);
    } else if (name == "--index_path") {
      args.index_path = require_value(i, argc, argv, name);
    } else if (name == "--dynamic_dir") {
      args.dynamic_dir = require_value(i, argc, argv, name);
    } else if (name == "--output") {
      args.output = require_value(i, argc, argv, name);
    } else if (name == "--num_operations") {
      args.num_operations = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--duration_sec") {
      args.duration_sec = std::stod(require_value(i, argc, argv, name));
    } else if (name == "--read_threads") {
      args.read_threads = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--write_threads") {
      args.write_threads = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--read_ratio") {
      args.read_ratio = std::stod(require_value(i, argc, argv, name));
    } else if (name == "--write_ratio") {
      args.write_ratio = std::stod(require_value(i, argc, argv, name));
    } else if (name == "--recall_sample_rate") {
      args.recall_sample_rate = std::stod(require_value(i, argc, argv, name));
    } else if (name == "--topk") {
      args.topk = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--insert_batch_size") {
      args.insert_batch_size = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--enable_flush") {
      args.enable_flush = parse_bool(require_value(i, argc, argv, name));
    } else if (name == "--enable_compaction") {
      args.enable_compaction = parse_bool(require_value(i, argc, argv, name));
    } else if (name == "--compaction_background") {
      args.compaction_background =
          parse_bool(require_value(i, argc, argv, name));
    } else if (name == "--compaction_interval_ms") {
      args.compaction_interval_ms = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--base_count") {
      args.base_count = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--query_count") {
      args.query_count = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--dim") {
      args.dim = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--memtable_flush_bytes") {
      args.memtable_flush_bytes = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--dynamic_graph_degree") {
      args.dynamic_graph_degree = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--search_width") {
      args.search_width = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--entry_count") {
      args.entry_count = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--beam_width") {
      args.beam_width = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--io_mode") {
      args.io_mode = require_value(i, argc, argv, name);
    } else if (name == "--io_batch") {
      args.io_batch = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--io_depth") {
      args.io_depth = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--cache_policy") {
      args.cache_policy = require_value(i, argc, argv, name);
    } else if (name == "--cache_pages") {
      args.cache_pages = static_cast<std::size_t>(
          std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--seed") {
      args.seed = static_cast<std::uint32_t>(
          std::stoul(require_value(i, argc, argv, name)));
    } else if (name == "--help" || name == "-h") {
      print_help();
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown option: " + option);
    }
  }
  if (args.num_operations == 0 || args.topk == 0 ||
      args.insert_batch_size == 0 || args.search_width == 0 ||
      args.entry_count == 0 || args.beam_width == 0 || args.io_batch == 0 ||
      args.io_depth == 0) {
    throw std::runtime_error(
        "num_operations, topk, insert_batch_size, and SSD search parameters must be positive");
  }
  if (args.duration_sec < 0.0 || args.recall_sample_rate < 0.0 ||
      args.recall_sample_rate > 1.0) {
    throw std::runtime_error(
        "duration_sec must be non-negative and recall_sample_rate must be in [0,1]");
  }
  return args;
}

bool concurrent_mode(const Args& args) {
  return args.duration_sec > 0.0 || args.read_threads > 0 ||
         args.write_threads > 0;
}

agent_aware::VectorSet make_dataset(const Args& args) {
  if (!args.data_path.empty()) {
    return agent_aware::load_fvecs(args.data_path, args.base_count);
  }

  agent_aware::SyntheticConfig config;
  config.base_count = args.base_count;
  config.query_count = args.query_count;
  config.dim = args.dim;
  config.clusters =
      std::max<std::size_t>(1, std::min<std::size_t>(64, args.base_count));
  config.seed = args.seed;
  return agent_aware::generate_synthetic(config).base;
}

std::vector<std::vector<float>> make_queries(const agent_aware::VectorSet& base,
                                             std::size_t query_count,
                                             std::uint32_t seed) {
  std::vector<std::vector<float>> queries;
  queries.reserve(query_count);
  std::mt19937 rng(seed ^ 0xa53u);
  std::normal_distribution<float> noise(0.0f, 0.01f);
  for (std::size_t i = 0; i < query_count; ++i) {
    const float* source = base.row(i % base.size());
    std::vector<float> query(source, source + base.dim);
    for (float& value : query) {
      value += noise(rng);
    }
    queries.push_back(std::move(query));
  }
  return queries;
}

std::vector<float> make_insert_vector(std::size_t index, std::size_t dim,
                                      std::mt19937& rng) {
  std::normal_distribution<float> distribution(0.0f, 1.0f);
  std::vector<float> vector(dim);
  const float bias = static_cast<float>((index % 17) - 8) * 0.05f;
  for (float& value : vector) {
    value = distribution(rng) + bias;
  }
  return vector;
}

Percentiles summarize(std::vector<double> values) {
  Percentiles output;
  if (values.empty()) {
    return output;
  }
  std::sort(values.begin(), values.end());
  const double sum = std::accumulate(values.begin(), values.end(), 0.0);
  output.avg = sum / static_cast<double>(values.size());
  auto pick = [&](double p) {
    const auto index = static_cast<std::size_t>(
        std::min<double>(values.size() - 1,
                         std::ceil(p * static_cast<double>(values.size())) - 1));
    return values[index];
  };
  output.p50 = pick(0.50);
  output.p95 = pick(0.95);
  output.p99 = pick(0.99);
  output.p999 = pick(0.999);
  return output;
}

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double elapsed_seconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double>(end - start).count();
}

std::size_t count_sstables(const std::filesystem::path& dynamic_dir) {
  agent_aware::dynamic::ManifestData manifest_data;
  agent_aware::dynamic::Manifest manifest(dynamic_dir / "manifest.json");
  if (manifest.load(manifest_data)) {
    return manifest_data.sstables.size();
  }
  return 0;
}

std::uintmax_t directory_bytes(const std::filesystem::path& dir) {
  if (!std::filesystem::exists(dir)) {
    return 0;
  }
  std::uintmax_t bytes = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      bytes += entry.file_size();
    }
  }
  return bytes;
}

double estimate_memory_mb(
    const std::vector<agent_aware::DynamicRecord>& records) {
  std::uint64_t bytes = 0;
  for (const auto& record : records) {
    bytes += sizeof(record);
    bytes += record.vector.size() * sizeof(float);
    bytes += record.neighbors.size() * sizeof(agent_aware::NodeId);
  }
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double recall_at_k(const std::vector<agent_aware::SearchResult>& result,
                   const std::vector<agent_aware::SearchResult>& truth,
                   std::size_t k) {
  if (k == 0 || truth.empty()) {
    return 1.0;
  }
  std::unordered_set<std::uint32_t> truth_ids;
  const std::size_t truth_k = std::min(k, truth.size());
  for (std::size_t i = 0; i < truth_k; ++i) {
    truth_ids.insert(truth[i].id);
  }
  std::size_t hits = 0;
  const std::size_t result_k = std::min(k, result.size());
  for (std::size_t i = 0; i < result_k; ++i) {
    if (truth_ids.find(result[i].id) != truth_ids.end()) {
      ++hits;
    }
  }
  return static_cast<double>(hits) / static_cast<double>(truth_k);
}

bool search_result_less(const agent_aware::SearchResult& lhs,
                        const agent_aware::SearchResult& rhs) {
  if (lhs.distance == rhs.distance) {
    return lhs.id < rhs.id;
  }
  return lhs.distance < rhs.distance;
}

std::vector<agent_aware::SearchResult> exact_dynamic_topk(
    const agent_aware::VectorSet& base, const float* query, std::size_t topk,
    const agent_aware::dynamic::DynamicSnapshot& snapshot) {
  std::unordered_map<agent_aware::NodeId, agent_aware::DynamicRecord> latest;
  latest.reserve(snapshot.records.size());
  for (const auto& record : snapshot.records) {
    latest[record.node_id] = record;
  }

  std::vector<agent_aware::SearchResult> results;
  results.reserve(base.size() + latest.size());
  for (std::size_t id = 0; id < base.size(); ++id) {
    const auto found = latest.find(static_cast<agent_aware::NodeId>(id));
    if (found != latest.end()) {
      const auto& record = found->second;
      if (record.deleted) {
        continue;
      }
      if (record.dim == base.dim && record.vector.size() == base.dim) {
        results.push_back(agent_aware::SearchResult{
            static_cast<std::uint32_t>(id),
            agent_aware::squared_l2(query, record.vector.data(), base.dim)});
        continue;
      }
    }
    results.push_back(agent_aware::SearchResult{
        static_cast<std::uint32_t>(id),
        agent_aware::squared_l2(query, base.row(id), base.dim)});
  }

  for (const auto& item : latest) {
    const auto& record = item.second;
    if (record.node_id < base.size() || record.deleted ||
        record.dim != base.dim || record.vector.size() != base.dim) {
      continue;
    }
    results.push_back(agent_aware::SearchResult{
        record.node_id,
        agent_aware::squared_l2(query, record.vector.data(), base.dim)});
  }

  std::sort(results.begin(), results.end(), search_result_less);
  if (results.size() > topk) {
    results.resize(topk);
  }
  return results;
}

std::vector<agent_aware::SearchResult> search_memory_with_delta(
    const agent_aware::VectorSet& base,
    const std::shared_ptr<agent_aware::dynamic::DynamicWriteManager>& manager,
    const float* query, std::size_t topk, std::uint64_t read_sequence) {
  auto base_results = agent_aware::search_memory_fast(base, query, topk);
  std::vector<agent_aware::NodeId> base_ids;
  base_ids.reserve(base_results.size());
  for (const auto& result : base_results) {
    base_ids.push_back(result.id);
  }
  auto latest_base_records =
      manager->latest_records_for(base_ids, read_sequence, true);
  auto delta_records = manager->search_delta_l2_at(
      query, static_cast<std::uint32_t>(base.dim), topk, read_sequence);
  delta_records.reserve(delta_records.size() + latest_base_records.size());
  for (auto& item : latest_base_records) {
    delta_records.push_back(std::move(item.second));
  }
  return agent_aware::dynamic::merge_base_and_delta_l2(
      base_results, delta_records, query, static_cast<std::uint32_t>(base.dim),
      topk);
}

agent_aware::PackedGraphEngineConfig engine_config_for(const Args& args,
    const std::shared_ptr<agent_aware::dynamic::DynamicWriteManager>& manager) {
  agent_aware::PackedGraphEngineConfig engine_config;
  engine_config.index_path = args.index_path;
  engine_config.dynamic_manager = manager;
  engine_config.cache_policy = args.cache_policy;
  engine_config.cache_pages = args.cache_pages;
  engine_config.io_mode = args.io_mode;
  engine_config.io_batch_size = args.io_batch;
  engine_config.io_depth = args.io_depth;
  engine_config.search.search_width = args.search_width;
  engine_config.search.entry_count = args.entry_count;
  engine_config.search.beam_width = args.beam_width;
  return engine_config;
}

std::shared_ptr<agent_aware::dynamic::DynamicWriteManager> open_manager(
    const Args& args, const std::filesystem::path& scenario_dir) {
  agent_aware::dynamic::DynamicWriteOptions options;
  options.dynamic_dir = scenario_dir;
  options.memtable_flush_bytes = args.memtable_flush_bytes;
  options.dynamic_graph_degree = args.dynamic_graph_degree;
  options.enable_wal = true;
  options.enable_auto_flush = args.enable_flush;

  auto manager =
      std::make_shared<agent_aware::dynamic::DynamicWriteManager>(options);
  if (!manager->open()) {
    throw std::runtime_error("Failed to open DynamicWriteManager");
  }
  return manager;
}

void record_recall_sample(
    ThreadMetrics& metrics, const agent_aware::VectorSet& base,
    const std::vector<agent_aware::SearchResult>& approx, const float* query,
    std::size_t topk,
    const std::shared_ptr<agent_aware::dynamic::DynamicWriteManager>& manager,
    std::uint64_t read_sequence) {
  const auto snapshot = manager->snapshot(read_sequence);
  const auto exact = exact_dynamic_topk(base, query, topk, snapshot);
  metrics.recall_sum +=
      recall_at_k(approx, exact, std::min<std::size_t>(10, topk));
  ++metrics.recall_samples;
  metrics.recall_sequence_min =
      std::min(metrics.recall_sequence_min, read_sequence);
  metrics.recall_sequence_max =
      std::max(metrics.recall_sequence_max, read_sequence);
  metrics.exact_delta_visible_sum +=
      snapshot.records.size() - snapshot.deleted_count;
  metrics.exact_deleted_sum += snapshot.deleted_count;
}

void merge_thread_metrics(ThreadMetrics& into, ThreadMetrics&& from) {
  into.op_latencies_ms.insert(into.op_latencies_ms.end(),
                              from.op_latencies_ms.begin(),
                              from.op_latencies_ms.end());
  into.read_latencies_ms.insert(into.read_latencies_ms.end(),
                                from.read_latencies_ms.begin(),
                                from.read_latencies_ms.end());
  into.write_latencies_ms.insert(into.write_latencies_ms.end(),
                                 from.write_latencies_ms.begin(),
                                 from.write_latencies_ms.end());
  into.read_ops += from.read_ops;
  into.write_records += from.write_records;
  into.recall_sum += from.recall_sum;
  into.recall_samples += from.recall_samples;
  into.recall_sequence_min =
      std::min(into.recall_sequence_min, from.recall_sequence_min);
  into.recall_sequence_max =
      std::max(into.recall_sequence_max, from.recall_sequence_max);
  into.exact_delta_visible_sum += from.exact_delta_visible_sum;
  into.exact_deleted_sum += from.exact_deleted_sum;
  into.graph_stats.node_reads += from.graph_stats.node_reads;
  into.graph_stats.page_requests += from.graph_stats.page_requests;
  into.graph_stats.page_cache_hits += from.graph_stats.page_cache_hits;
}

void maybe_run_compaction(
    const std::shared_ptr<agent_aware::dynamic::DynamicWriteManager>& manager,
    CompactionAggregate& aggregate, std::mutex& mutex) {
  agent_aware::dynamic::DynamicCompactionStats stats;
  const auto start = Clock::now();
  const bool success = manager->compact_once(&stats);
  const double duration = elapsed_ms(start, Clock::now());
  if (!stats.attempted) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex);
  ++aggregate.attempted;
  if (success) {
    ++aggregate.success;
    aggregate.duration_ms += duration;
    aggregate.input_record_count += stats.input_record_count;
    aggregate.output_record_count += stats.output_record_count;
  }
}

ScenarioResult finalize_result(
    const Args& args, const Scenario& scenario,
    const std::filesystem::path& scenario_dir,
    const std::vector<agent_aware::DynamicRecord>& delta_records,
    ThreadMetrics metrics, CompactionAggregate compaction,
    Clock::time_point start, Clock::time_point end, double flush_ms,
    double recovery_ms) {
  ScenarioResult result;
  result.scenario = scenario.name;
  result.read_ratio = scenario.read_ratio;
  result.write_ratio = scenario.write_ratio;
  result.read_threads = args.read_threads;
  result.write_threads = args.write_threads;
  result.duration_sec = elapsed_seconds(start, end);
  result.read_ops = metrics.read_ops;
  result.write_ops = metrics.write_records;
  const double seconds = std::max(1e-9, result.duration_sec);
  result.read_qps = static_cast<double>(metrics.read_ops) / seconds;
  result.write_qps = static_cast<double>(metrics.write_records) / seconds;
  result.total_qps =
      static_cast<double>(metrics.read_ops + metrics.write_records) / seconds;
  result.insert_throughput = result.write_qps;
  result.latency = summarize(std::move(metrics.op_latencies_ms));
  result.read_latency = summarize(std::move(metrics.read_latencies_ms));
  result.write_latency = summarize(std::move(metrics.write_latencies_ms));
  result.recall_samples = metrics.recall_samples;
  result.recall_sample_rate = args.recall_sample_rate;
  result.recall_at_10 =
      metrics.recall_samples == 0
          ? 1.0
          : metrics.recall_sum / static_cast<double>(metrics.recall_samples);
  if (metrics.recall_samples > 0) {
    result.recall_read_sequence_min = metrics.recall_sequence_min;
    result.recall_read_sequence_max = metrics.recall_sequence_max;
    result.exact_delta_visible_count_avg =
        static_cast<double>(metrics.exact_delta_visible_sum) /
        static_cast<double>(metrics.recall_samples);
    result.exact_deleted_count_avg =
        static_cast<double>(metrics.exact_deleted_sum) /
        static_cast<double>(metrics.recall_samples);
  }
  result.memtable_insert_avg_us = result.write_latency.avg * 1000.0;
  result.flush_duration_ms = flush_ms;
  result.sstable_count = count_sstables(scenario_dir);
  result.delta_record_count = delta_records.size();
  result.memory_usage_mb = estimate_memory_mb(delta_records);
  result.disk_usage_mb =
      static_cast<double>(directory_bytes(scenario_dir)) / (1024.0 * 1024.0);
  result.recovery_time_ms = recovery_ms;
  result.search_mode = args.index_path.empty() ? "memory_exact" : "ssd_graph";
  result.graph_reads_per_read =
      metrics.read_ops == 0 ? 0.0
                            : static_cast<double>(metrics.graph_stats.node_reads) /
                                  static_cast<double>(metrics.read_ops);
  result.cache_hit_rate =
      metrics.graph_stats.page_requests == 0
          ? 0.0
          : static_cast<double>(metrics.graph_stats.page_cache_hits) /
                static_cast<double>(metrics.graph_stats.page_requests);
  result.compaction = compaction;
  return result;
}

ScenarioResult run_sequential_scenario(
    const Args& args, const Scenario& scenario,
    const agent_aware::VectorSet& base,
    const std::vector<std::vector<float>>& queries) {
  const auto scenario_dir = args.dynamic_dir / scenario.name;
  std::filesystem::remove_all(scenario_dir);
  std::filesystem::create_directories(scenario_dir);
  auto manager = open_manager(args, scenario_dir);

  std::unique_ptr<agent_aware::PackedGraphEngine> engine;
  if (!args.index_path.empty()) {
    engine = std::make_unique<agent_aware::PackedGraphEngine>(
        engine_config_for(args, manager));
  }

  std::mt19937 rng(args.seed ^ static_cast<std::uint32_t>(scenario.name.size()));
  std::uniform_real_distribution<double> choice(0.0, 1.0);
  std::uniform_real_distribution<double> sample_pick(0.0, 1.0);
  ThreadMetrics metrics;
  metrics.op_latencies_ms.reserve(args.num_operations);
  std::size_t next_node_id = base.size();
  std::size_t next_query = 0;

  const auto start = Clock::now();
  for (std::size_t op = 0; op < args.num_operations; ++op) {
    const bool do_read = choice(rng) < scenario.read_ratio;
    const auto op_start = Clock::now();
    if (do_read) {
      const auto& query = queries[next_query++ % queries.size()];
      std::vector<agent_aware::SearchResult> merged;
      std::uint64_t read_sequence = manager->current_sequence();
      if (engine) {
        const auto engine_result = engine->search_one(query.data(), args.topk);
        merged = engine_result.topk;
        read_sequence = engine_result.stats.dynamic_read_sequence;
        metrics.graph_stats.node_reads += engine_result.stats.graph.node_reads;
        metrics.graph_stats.page_requests +=
            engine_result.stats.graph.page_requests;
        metrics.graph_stats.page_cache_hits +=
            engine_result.stats.graph.page_cache_hits;
      } else {
        merged = search_memory_with_delta(base, manager, query.data(),
                                          args.topk, read_sequence);
      }
      if (sample_pick(rng) <= args.recall_sample_rate) {
        record_recall_sample(metrics, base, merged, query.data(), args.topk,
                             manager, read_sequence);
      }
      ++metrics.read_ops;
      const double latency = elapsed_ms(op_start, Clock::now());
      metrics.read_latencies_ms.push_back(latency);
      metrics.op_latencies_ms.push_back(latency);
    } else {
      for (std::size_t i = 0; i < args.insert_batch_size; ++i) {
        auto vector = make_insert_vector(metrics.write_records, base.dim, rng);
        const auto node_id = static_cast<agent_aware::NodeId>(next_node_id++);
        if (engine) {
          engine->insert(node_id, vector.data());
        } else if (!manager->insert(node_id, vector.data(),
                                    static_cast<std::uint32_t>(base.dim))) {
          throw std::runtime_error("Dynamic insert failed");
        }
        ++metrics.write_records;
      }
      const double latency = elapsed_ms(op_start, Clock::now());
      metrics.write_latencies_ms.push_back(latency);
      metrics.op_latencies_ms.push_back(latency);
    }
  }
  const auto end = Clock::now();

  double flush_ms = 0.0;
  if (args.enable_flush) {
    const auto flush_start = Clock::now();
    if (!manager->flush()) {
      throw std::runtime_error("Dynamic flush failed");
    }
    flush_ms = elapsed_ms(flush_start, Clock::now());
  }

  CompactionAggregate compaction;
  std::mutex compaction_mutex;
  if (args.enable_compaction) {
    maybe_run_compaction(manager, compaction, compaction_mutex);
  }

  agent_aware::dynamic::DynamicWriteOptions options;
  options.dynamic_dir = scenario_dir;
  options.memtable_flush_bytes = args.memtable_flush_bytes;
  options.dynamic_graph_degree = args.dynamic_graph_degree;
  options.enable_wal = true;
  options.enable_auto_flush = args.enable_flush;

  const auto final_delta_records = manager->collect_all_delta_records();
  if (!manager->close()) {
    throw std::runtime_error("Dynamic close failed");
  }
  const auto recovery_start = Clock::now();
  agent_aware::dynamic::DynamicWriteManager recovered(options);
  if (!recovered.open()) {
    throw std::runtime_error("Dynamic recovery open failed");
  }
  const double recovery_ms = elapsed_ms(recovery_start, Clock::now());
  (void)recovered.close();

  return finalize_result(args, scenario, scenario_dir, final_delta_records,
                         std::move(metrics), compaction, start, end, flush_ms,
                         recovery_ms);
}

ScenarioResult run_concurrent_scenario(
    const Args& args, const Scenario& scenario,
    const agent_aware::VectorSet& base,
    const std::vector<std::vector<float>>& queries) {
  const auto scenario_dir = args.dynamic_dir / scenario.name;
  std::filesystem::remove_all(scenario_dir);
  std::filesystem::create_directories(scenario_dir);
  auto manager = open_manager(args, scenario_dir);

  const std::size_t read_threads =
      args.read_threads == 0 ? std::size_t{1} : args.read_threads;
  const std::size_t write_threads =
      args.write_threads == 0 && scenario.write_ratio > 0.0 ? std::size_t{1}
                                                            : args.write_threads;
  if (read_threads == 0 && write_threads == 0) {
    throw std::runtime_error("Concurrent mode needs at least one worker thread");
  }

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> claimed_ops{0};
  std::atomic<std::size_t> query_cursor{0};
  std::atomic<std::uint32_t> next_node_id{
      static_cast<std::uint32_t>(base.size())};
  std::atomic<std::size_t> write_index{0};

  auto claim_operation = [&]() {
    if (args.duration_sec > 0.0) {
      return !stop.load(std::memory_order_relaxed);
    }
    const auto id = claimed_ops.fetch_add(1, std::memory_order_relaxed);
    if (id >= args.num_operations) {
      stop.store(true, std::memory_order_relaxed);
      return false;
    }
    return true;
  };

  std::vector<ThreadMetrics> reader_metrics(read_threads);
  std::vector<ThreadMetrics> writer_metrics(write_threads);
  std::vector<std::thread> workers;
  std::vector<std::exception_ptr> errors(read_threads + write_threads);

  CompactionAggregate compaction;
  std::mutex compaction_mutex;
  std::thread compaction_thread;
  if (args.enable_compaction && args.compaction_background) {
    compaction_thread = std::thread([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(args.compaction_interval_ms));
        if (stop.load(std::memory_order_relaxed)) {
          break;
        }
        maybe_run_compaction(manager, compaction, compaction_mutex);
      }
    });
  }

  const auto start = Clock::now();
  for (std::size_t thread_id = 0; thread_id < read_threads; ++thread_id) {
    workers.emplace_back([&, thread_id]() {
      try {
        std::unique_ptr<agent_aware::PackedGraphEngine> engine;
        if (!args.index_path.empty()) {
          engine = std::make_unique<agent_aware::PackedGraphEngine>(
              engine_config_for(args, manager));
        }
        std::mt19937 rng(args.seed ^ static_cast<std::uint32_t>(thread_id + 17));
        std::uniform_real_distribution<double> sample_pick(0.0, 1.0);
        auto& metrics = reader_metrics[thread_id];
        while (claim_operation()) {
          const auto& query =
              queries[query_cursor.fetch_add(1, std::memory_order_relaxed) %
                      queries.size()];
          const auto op_start = Clock::now();
          std::vector<agent_aware::SearchResult> merged;
          std::uint64_t read_sequence = manager->current_sequence();
          if (engine) {
            const auto engine_result =
                engine->search_one(query.data(), args.topk);
            merged = engine_result.topk;
            read_sequence = engine_result.stats.dynamic_read_sequence;
            metrics.graph_stats.node_reads +=
                engine_result.stats.graph.node_reads;
            metrics.graph_stats.page_requests +=
                engine_result.stats.graph.page_requests;
            metrics.graph_stats.page_cache_hits +=
                engine_result.stats.graph.page_cache_hits;
          } else {
            merged = search_memory_with_delta(base, manager, query.data(),
                                              args.topk, read_sequence);
          }
          if (sample_pick(rng) <= args.recall_sample_rate) {
            record_recall_sample(metrics, base, merged, query.data(), args.topk,
                                 manager, read_sequence);
          }
          const double latency = elapsed_ms(op_start, Clock::now());
          metrics.read_latencies_ms.push_back(latency);
          metrics.op_latencies_ms.push_back(latency);
          ++metrics.read_ops;
        }
      } catch (...) {
        errors[thread_id] = std::current_exception();
        stop.store(true, std::memory_order_relaxed);
      }
    });
  }

  for (std::size_t thread_id = 0; thread_id < write_threads; ++thread_id) {
    workers.emplace_back([&, thread_id]() {
      try {
        std::mt19937 rng(args.seed ^ static_cast<std::uint32_t>(thread_id + 911));
        auto& metrics = writer_metrics[thread_id];
        while (claim_operation()) {
          const auto op_start = Clock::now();
          for (std::size_t i = 0; i < args.insert_batch_size; ++i) {
            const auto local_write =
                write_index.fetch_add(1, std::memory_order_relaxed);
            auto vector = make_insert_vector(local_write, base.dim, rng);
            const auto node_id = static_cast<agent_aware::NodeId>(
                next_node_id.fetch_add(1, std::memory_order_relaxed));
            if (!manager->insert(node_id, vector.data(),
                                 static_cast<std::uint32_t>(base.dim))) {
              throw std::runtime_error("Dynamic insert failed");
            }
            ++metrics.write_records;
          }
          const double latency = elapsed_ms(op_start, Clock::now());
          metrics.write_latencies_ms.push_back(latency);
          metrics.op_latencies_ms.push_back(latency);
        }
      } catch (...) {
        errors[read_threads + thread_id] = std::current_exception();
        stop.store(true, std::memory_order_relaxed);
      }
    });
  }

  if (args.duration_sec > 0.0) {
    std::this_thread::sleep_for(
        std::chrono::duration<double>(args.duration_sec));
    stop.store(true, std::memory_order_relaxed);
  }

  for (auto& worker : workers) {
    worker.join();
  }
  stop.store(true, std::memory_order_relaxed);
  if (compaction_thread.joinable()) {
    compaction_thread.join();
  }
  for (const auto& error : errors) {
    if (error != nullptr) {
      std::rethrow_exception(error);
    }
  }
  const auto end = Clock::now();

  ThreadMetrics metrics;
  for (auto& item : reader_metrics) {
    merge_thread_metrics(metrics, std::move(item));
  }
  for (auto& item : writer_metrics) {
    merge_thread_metrics(metrics, std::move(item));
  }

  double flush_ms = 0.0;
  if (args.enable_flush) {
    const auto flush_start = Clock::now();
    if (!manager->flush()) {
      throw std::runtime_error("Dynamic flush failed");
    }
    flush_ms = elapsed_ms(flush_start, Clock::now());
  }
  if (args.enable_compaction && !args.compaction_background) {
    maybe_run_compaction(manager, compaction, compaction_mutex);
  }

  agent_aware::dynamic::DynamicWriteOptions options;
  options.dynamic_dir = scenario_dir;
  options.memtable_flush_bytes = args.memtable_flush_bytes;
  options.dynamic_graph_degree = args.dynamic_graph_degree;
  options.enable_wal = true;
  options.enable_auto_flush = args.enable_flush;

  const auto final_delta_records = manager->collect_all_delta_records();
  if (!manager->close()) {
    throw std::runtime_error("Dynamic close failed");
  }
  const auto recovery_start = Clock::now();
  agent_aware::dynamic::DynamicWriteManager recovered(options);
  if (!recovered.open()) {
    throw std::runtime_error("Dynamic recovery open failed");
  }
  const double recovery_ms = elapsed_ms(recovery_start, Clock::now());
  (void)recovered.close();

  Args effective_args = args;
  effective_args.read_threads = read_threads;
  effective_args.write_threads = write_threads;
  return finalize_result(effective_args, scenario, scenario_dir,
                         final_delta_records, std::move(metrics), compaction,
                         start, end, flush_ms, recovery_ms);
}

std::vector<Scenario> scenarios_for(const Args& args) {
  if (args.read_ratio >= 0.0 || args.write_ratio >= 0.0) {
    double read_ratio =
        args.read_ratio >= 0.0 ? args.read_ratio : 1.0 - args.write_ratio;
    double write_ratio =
        args.write_ratio >= 0.0 ? args.write_ratio : 1.0 - read_ratio;
    const double total = read_ratio + write_ratio;
    if (total <= 0.0) {
      throw std::runtime_error("read_ratio + write_ratio must be positive");
    }
    read_ratio /= total;
    write_ratio /= total;
    return {Scenario{"custom", read_ratio, write_ratio}};
  }
  return {
      Scenario{"read-heavy", 0.95, 0.05},
      Scenario{"balanced", 0.70, 0.30},
      Scenario{"write-heavy", 0.50, 0.50},
  };
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

void write_csv(const std::filesystem::path& output_path,
               const std::vector<ScenarioResult>& results) {
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream output(output_path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create CSV output: " + output_path.string());
  }
  output << "scenario,read_ratio,write_ratio,read_threads,write_threads,"
            "duration_sec,read_ops,write_ops,read_qps,write_qps,total_qps,"
            "insert_throughput,avg_latency,p50_latency,p95_latency,p99_latency,"
            "p999_latency,read_p50,read_p95,read_p99,write_p50,write_p95,"
            "write_p99,recall_at_10,recall_samples,recall_sample_rate,"
            "recall_read_sequence_min,recall_read_sequence_max,"
            "exact_delta_visible_count_avg,exact_deleted_count_avg,"
            "memtable_insert_avg_us,flush_duration_ms,sstable_count,"
            "delta_record_count,memory_usage_mb,disk_usage_mb,recovery_time_ms,"
            "search_mode,graph_reads_per_read,cache_hit_rate,"
            "compaction_attempted,compaction_count,compaction_duration_ms,"
            "compaction_input_records,compaction_output_records\n";
  output << std::fixed << std::setprecision(6);
  for (const auto& result : results) {
    output << result.scenario << ','
           << result.read_ratio << ','
           << result.write_ratio << ','
           << result.read_threads << ','
           << result.write_threads << ','
           << result.duration_sec << ','
           << result.read_ops << ','
           << result.write_ops << ','
           << result.read_qps << ','
           << result.write_qps << ','
           << result.total_qps << ','
           << result.insert_throughput << ','
           << result.latency.avg << ','
           << result.latency.p50 << ','
           << result.latency.p95 << ','
           << result.latency.p99 << ','
           << result.latency.p999 << ','
           << result.read_latency.p50 << ','
           << result.read_latency.p95 << ','
           << result.read_latency.p99 << ','
           << result.write_latency.p50 << ','
           << result.write_latency.p95 << ','
           << result.write_latency.p99 << ','
           << result.recall_at_10 << ','
           << result.recall_samples << ','
           << result.recall_sample_rate << ','
           << result.recall_read_sequence_min << ','
           << result.recall_read_sequence_max << ','
           << result.exact_delta_visible_count_avg << ','
           << result.exact_deleted_count_avg << ','
           << result.memtable_insert_avg_us << ','
           << result.flush_duration_ms << ','
           << result.sstable_count << ','
           << result.delta_record_count << ','
           << result.memory_usage_mb << ','
           << result.disk_usage_mb << ','
           << result.recovery_time_ms << ','
           << result.search_mode << ','
           << result.graph_reads_per_read << ','
           << result.cache_hit_rate << ','
           << result.compaction.attempted << ','
           << result.compaction.success << ','
           << result.compaction.duration_ms << ','
           << result.compaction.input_record_count << ','
           << result.compaction.output_record_count << '\n';
  }
}

void write_json(const std::filesystem::path& output_path,
                const std::vector<ScenarioResult>& results) {
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream output(output_path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create JSON output: " + output_path.string());
  }
  output << std::fixed << std::setprecision(6);
  output << "{\n  \"status\": \"completed\",\n  \"scenarios\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    output << "    {\n"
           << "      \"scenario\": \"" << json_escape(result.scenario) << "\",\n"
           << "      \"read_ratio\": " << result.read_ratio << ",\n"
           << "      \"write_ratio\": " << result.write_ratio << ",\n"
           << "      \"read_threads\": " << result.read_threads << ",\n"
           << "      \"write_threads\": " << result.write_threads << ",\n"
           << "      \"duration_sec\": " << result.duration_sec << ",\n"
           << "      \"read_ops\": " << result.read_ops << ",\n"
           << "      \"write_ops\": " << result.write_ops << ",\n"
           << "      \"read_qps\": " << result.read_qps << ",\n"
           << "      \"write_qps\": " << result.write_qps << ",\n"
           << "      \"total_qps\": " << result.total_qps << ",\n"
           << "      \"read_p50_ms\": " << result.read_latency.p50 << ",\n"
           << "      \"read_p95_ms\": " << result.read_latency.p95 << ",\n"
           << "      \"read_p99_ms\": " << result.read_latency.p99 << ",\n"
           << "      \"read_p999_ms\": " << result.read_latency.p999 << ",\n"
           << "      \"write_p50_ms\": " << result.write_latency.p50 << ",\n"
           << "      \"write_p95_ms\": " << result.write_latency.p95 << ",\n"
           << "      \"write_p99_ms\": " << result.write_latency.p99 << ",\n"
           << "      \"recall_at_10\": " << result.recall_at_10 << ",\n"
           << "      \"recall_samples\": " << result.recall_samples << ",\n"
           << "      \"recall_sample_rate\": " << result.recall_sample_rate << ",\n"
           << "      \"recall_read_sequence_min\": "
           << result.recall_read_sequence_min << ",\n"
           << "      \"recall_read_sequence_max\": "
           << result.recall_read_sequence_max << ",\n"
           << "      \"exact_delta_visible_count_avg\": "
           << result.exact_delta_visible_count_avg << ",\n"
           << "      \"exact_deleted_count_avg\": "
           << result.exact_deleted_count_avg << ",\n"
           << "      \"flush_duration_ms\": " << result.flush_duration_ms << ",\n"
           << "      \"sstable_count\": " << result.sstable_count << ",\n"
           << "      \"delta_record_count\": " << result.delta_record_count << ",\n"
           << "      \"memory_usage_mb\": " << result.memory_usage_mb << ",\n"
           << "      \"disk_usage_mb\": " << result.disk_usage_mb << ",\n"
           << "      \"recovery_time_ms\": " << result.recovery_time_ms << ",\n"
           << "      \"search_mode\": \"" << json_escape(result.search_mode)
           << "\",\n"
           << "      \"graph_reads_per_read\": "
           << result.graph_reads_per_read << ",\n"
           << "      \"cache_hit_rate\": " << result.cache_hit_rate << ",\n"
           << "      \"compaction_attempted\": "
           << result.compaction.attempted << ",\n"
           << "      \"compaction_count\": " << result.compaction.success
           << ",\n"
           << "      \"compaction_duration_ms\": "
           << result.compaction.duration_ms << ",\n"
           << "      \"compaction_input_records\": "
           << result.compaction.input_record_count << ",\n"
           << "      \"compaction_output_records\": "
           << result.compaction.output_record_count << "\n"
           << "    }" << (i + 1 == results.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
}

void write_output(const std::filesystem::path& output_path,
                  const std::vector<ScenarioResult>& results) {
  if (output_path.extension() == ".json") {
    write_json(output_path, results);
  } else {
    write_csv(output_path, results);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    const auto base = make_dataset(args);
    if (base.empty()) {
      throw std::runtime_error("Base dataset is empty");
    }
    const auto queries =
        make_queries(base, std::max<std::size_t>(1, args.query_count), args.seed);

    std::vector<ScenarioResult> results;
    for (const auto& scenario : scenarios_for(args)) {
      if (concurrent_mode(args)) {
        results.push_back(run_concurrent_scenario(args, scenario, base, queries));
      } else {
        results.push_back(run_sequential_scenario(args, scenario, base, queries));
      }
    }
    write_output(args.output, results);

    std::cout << "p5_mixed_rw_output=" << args.output.string() << "\n";
    for (const auto& result : results) {
      std::cout << result.scenario << ": read_qps=" << result.read_qps
                << " write_qps=" << result.write_qps
                << " read_p95_ms=" << result.read_latency.p95
                << " read_p99_ms=" << result.read_latency.p99
                << " recall_at_10=" << result.recall_at_10
                << " recovery_ms=" << result.recovery_time_ms << "\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
