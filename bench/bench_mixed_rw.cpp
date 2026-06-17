#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agentmem/core/brute_force.h"
#include "agentmem/data/dataset.h"
#include "agentmem/dynamic/compaction.h"
#include "agentmem/dynamic/dynamic_write_manager.h"
#include "agentmem/dynamic/manifest.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Args {
  std::string data_path;
  std::string index_path;
  std::filesystem::path dynamic_dir = "build/p5_mixed_rw_dynamic";
  std::filesystem::path output = "build/p5_mixed_rw.csv";
  std::size_t num_operations = 1000;
  double read_ratio = -1.0;
  double write_ratio = -1.0;
  std::size_t topk = 10;
  std::size_t insert_batch_size = 1;
  bool enable_flush = true;
  bool enable_compaction = false;
  std::size_t base_count = 1000;
  std::size_t query_count = 256;
  std::size_t dim = 32;
  std::size_t memtable_flush_bytes = 256 * 1024;
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
};

struct ScenarioResult {
  std::string scenario;
  double read_ratio = 0.0;
  double write_ratio = 0.0;
  double read_qps = 0.0;
  double write_qps = 0.0;
  double insert_throughput = 0.0;
  Percentiles latency;
  double recall_at_10 = 0.0;
  double wal_append_avg_us = 0.0;
  double memtable_insert_avg_us = 0.0;
  double flush_duration_ms = 0.0;
  std::size_t sstable_count = 0;
  std::size_t delta_record_count = 0;
  double memory_usage_mb = 0.0;
  double disk_usage_mb = 0.0;
  double recovery_time_ms = 0.0;
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
      args.num_operations =
          static_cast<std::size_t>(std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--read_ratio") {
      args.read_ratio = std::stod(require_value(i, argc, argv, name));
    } else if (name == "--write_ratio") {
      args.write_ratio = std::stod(require_value(i, argc, argv, name));
    } else if (name == "--topk") {
      args.topk =
          static_cast<std::size_t>(std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--insert_batch_size") {
      args.insert_batch_size =
          static_cast<std::size_t>(std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--enable_flush") {
      args.enable_flush = parse_bool(require_value(i, argc, argv, name));
    } else if (name == "--enable_compaction") {
      args.enable_compaction = parse_bool(require_value(i, argc, argv, name));
    } else if (name == "--base_count") {
      args.base_count =
          static_cast<std::size_t>(std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--query_count") {
      args.query_count =
          static_cast<std::size_t>(std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--dim") {
      args.dim =
          static_cast<std::size_t>(std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--memtable_flush_bytes") {
      args.memtable_flush_bytes =
          static_cast<std::size_t>(std::stoull(require_value(i, argc, argv, name)));
    } else if (name == "--seed") {
      args.seed = static_cast<std::uint32_t>(
          std::stoul(require_value(i, argc, argv, name)));
    } else if (name == "--help" || name == "-h") {
      std::cout
          << "Usage: bench_mixed_rw [options]\n"
          << "  --data_path PATH             Optional fvecs base vectors\n"
          << "  --index_path PATH            Accepted for compatibility; unused\n"
          << "  --dynamic_dir PATH           Dynamic WAL/SSTable directory\n"
          << "  --num_operations N           Operation events per scenario\n"
          << "  --read_ratio R               Run one custom scenario\n"
          << "  --write_ratio R              Custom scenario write ratio\n"
          << "  --topk N                     Search top-k, default 10\n"
          << "  --insert_batch_size N        Inserts per write event\n"
          << "  --enable_flush 0|1           Flush at end and allow auto flush\n"
          << "  --enable_compaction 0|1      Run a measured manual compaction\n"
          << "  --output PATH                CSV output path\n";
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown option: " + option);
    }
  }
  if (args.num_operations == 0 || args.topk == 0 ||
      args.insert_batch_size == 0) {
    throw std::runtime_error("num_operations, topk and insert_batch_size must be positive");
  }
  return args;
}

agentmem::VectorSet make_dataset(const Args& args) {
  if (!args.data_path.empty()) {
    return agentmem::load_fvecs(args.data_path, args.base_count);
  }

  agentmem::SyntheticConfig config;
  config.base_count = args.base_count;
  config.query_count = args.query_count;
  config.dim = args.dim;
  config.clusters = std::max<std::size_t>(1, std::min<std::size_t>(64, args.base_count));
  config.seed = args.seed;
  return agentmem::generate_synthetic(config).base;
}

std::vector<std::vector<float>> make_queries(const agentmem::VectorSet& base,
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
  return output;
}

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double elapsed_seconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double>(end - start).count();
}

std::size_t count_sstables(const std::filesystem::path& dynamic_dir) {
  const auto dir = dynamic_dir / "sstable";
  if (!std::filesystem::exists(dir)) {
    return 0;
  }
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".meta") {
      ++count;
    }
  }
  return count;
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

double estimate_memory_mb(const std::vector<agentmem::DynamicRecord>& records) {
  std::uint64_t bytes = 0;
  for (const auto& record : records) {
    bytes += sizeof(record);
    bytes += record.vector.size() * sizeof(float);
    bytes += record.neighbors.size() * sizeof(agentmem::NodeId);
  }
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double recall_at_k(const std::vector<agentmem::SearchResult>& result,
                   const std::vector<agentmem::SearchResult>& truth,
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

double run_optional_compaction(const std::filesystem::path& dynamic_dir) {
  agentmem::dynamic::ManifestData manifest_data;
  agentmem::dynamic::Manifest manifest(dynamic_dir / "manifest.json");
  if (!manifest.load(manifest_data) || manifest_data.sstables.size() < 2) {
    return 0.0;
  }

  agentmem::dynamic::CompactionInput input;
  input.output_dir = dynamic_dir / "sstable";
  input.output_sstable_id = manifest_data.next_sstable_id + 100000;
  input.output_level = 1;
  for (const auto& entry : manifest_data.sstables) {
    const auto base_path =
        entry.base_path.is_absolute() ? entry.base_path
                                      : dynamic_dir / entry.base_path;
    auto reader =
        std::make_shared<agentmem::dynamic::SSTableReader>(base_path);
    if (reader->open()) {
      input.input_tables.push_back(std::move(reader));
    }
  }
  if (input.input_tables.size() < 2) {
    return 0.0;
  }

  const auto start = Clock::now();
  const auto result = agentmem::dynamic::CompactionJob(std::move(input)).run();
  const auto end = Clock::now();
  return result.success ? elapsed_ms(start, end) : 0.0;
}

ScenarioResult run_scenario(const Args& args, const Scenario& scenario,
                            const agentmem::VectorSet& base,
                            const std::vector<std::vector<float>>& queries) {
  const auto scenario_dir = args.dynamic_dir / scenario.name;
  std::filesystem::remove_all(scenario_dir);
  std::filesystem::create_directories(scenario_dir);

  agentmem::dynamic::DynamicWriteOptions options;
  options.dynamic_dir = scenario_dir;
  options.memtable_flush_bytes = args.memtable_flush_bytes;
  options.enable_wal = true;
  options.enable_auto_flush = args.enable_flush;

  agentmem::dynamic::DynamicWriteManager manager(options);
  if (!manager.open()) {
    throw std::runtime_error("Failed to open DynamicWriteManager");
  }

  std::mt19937 rng(args.seed ^ static_cast<std::uint32_t>(scenario.name.size()));
  std::uniform_real_distribution<double> choice(0.0, 1.0);
  std::vector<double> latencies_ms;
  std::vector<double> write_event_latencies_ms;
  latencies_ms.reserve(args.num_operations);

  std::size_t read_ops = 0;
  std::size_t write_records = 0;
  double recall_sum = 0.0;
  std::size_t recall_samples = 0;
  std::size_t next_node_id = base.size();
  std::size_t next_query = 0;

  const auto start = Clock::now();
  for (std::size_t op = 0; op < args.num_operations; ++op) {
    const bool do_read = choice(rng) < scenario.read_ratio;
    const auto op_start = Clock::now();
    if (do_read) {
      const auto& query = queries[next_query++ % queries.size()];
      const auto base_results =
          agentmem::search_memory_fast(base, query.data(), args.topk);
      const auto delta_results =
          manager.search_delta_l2(query.data(), static_cast<std::uint32_t>(base.dim),
                                  args.topk);
      const auto merged = agentmem::dynamic::merge_base_and_delta_l2(
          base_results, delta_results, query.data(),
          static_cast<std::uint32_t>(base.dim), args.topk);

      const auto all_delta = manager.collect_all_delta_records();
      const auto exact = agentmem::dynamic::merge_base_and_delta_l2(
          agentmem::search_memory_fast(base, query.data(), args.topk), all_delta,
          query.data(), static_cast<std::uint32_t>(base.dim), args.topk);
      recall_sum += recall_at_k(merged, exact, std::min<std::size_t>(10, args.topk));
      ++recall_samples;
      ++read_ops;
    } else {
      for (std::size_t i = 0; i < args.insert_batch_size; ++i) {
        auto vector = make_insert_vector(write_records, base.dim, rng);
        if (!manager.insert(static_cast<agentmem::NodeId>(next_node_id++),
                            vector.data(), static_cast<std::uint32_t>(base.dim))) {
          throw std::runtime_error("Dynamic insert failed");
        }
        ++write_records;
      }
    }
    const auto op_end = Clock::now();
    const double latency = elapsed_ms(op_start, op_end);
    latencies_ms.push_back(latency);
    if (!do_read) {
      write_event_latencies_ms.push_back(latency);
    }
  }
  const auto end = Clock::now();

  double flush_ms = 0.0;
  if (args.enable_flush) {
    const auto flush_start = Clock::now();
    if (!manager.flush()) {
      throw std::runtime_error("Dynamic flush failed");
    }
    flush_ms = elapsed_ms(flush_start, Clock::now());
  }
  if (args.enable_compaction) {
    flush_ms += run_optional_compaction(scenario_dir);
  }

  const auto delta_records = manager.collect_all_delta_records();
  if (!manager.close()) {
    throw std::runtime_error("Dynamic close failed");
  }

  const auto recovery_start = Clock::now();
  agentmem::dynamic::DynamicWriteManager recovered(options);
  if (!recovered.open()) {
    throw std::runtime_error("Dynamic recovery open failed");
  }
  const double recovery_ms = elapsed_ms(recovery_start, Clock::now());
  (void)recovered.close();

  ScenarioResult result;
  result.scenario = scenario.name;
  result.read_ratio = scenario.read_ratio;
  result.write_ratio = scenario.write_ratio;
  const double seconds = std::max(1e-9, elapsed_seconds(start, end));
  result.read_qps = static_cast<double>(read_ops) / seconds;
  result.write_qps = static_cast<double>(write_records) / seconds;
  result.insert_throughput = result.write_qps;
  result.latency = summarize(std::move(latencies_ms));
  result.recall_at_10 =
      recall_samples == 0 ? 1.0 : recall_sum / static_cast<double>(recall_samples);
  const auto write_latency = summarize(write_event_latencies_ms);
  result.wal_append_avg_us = 0.0;
  result.memtable_insert_avg_us = write_latency.avg * 1000.0;
  result.flush_duration_ms = flush_ms;
  result.sstable_count = count_sstables(scenario_dir);
  result.delta_record_count = delta_records.size();
  result.memory_usage_mb = estimate_memory_mb(delta_records);
  result.disk_usage_mb =
      static_cast<double>(directory_bytes(scenario_dir)) / (1024.0 * 1024.0);
  result.recovery_time_ms = recovery_ms;
  return result;
}

std::vector<Scenario> scenarios_for(const Args& args) {
  if (args.read_ratio >= 0.0 || args.write_ratio >= 0.0) {
    double read_ratio = args.read_ratio >= 0.0 ? args.read_ratio : 1.0 - args.write_ratio;
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

void write_csv(const std::filesystem::path& output_path,
               const std::vector<ScenarioResult>& results) {
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream output(output_path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create CSV output: " + output_path.string());
  }
  output << "scenario,read_ratio,write_ratio,read_qps,write_qps,"
            "insert_throughput,avg_latency,p50_latency,p95_latency,p99_latency,"
            "recall_at_10,wal_append_avg_us,memtable_insert_avg_us,"
            "flush_duration_ms,sstable_count,delta_record_count,"
            "memory_usage_mb,disk_usage_mb,recovery_time_ms\n";
  output << std::fixed << std::setprecision(6);
  for (const auto& result : results) {
    output << result.scenario << ','
           << result.read_ratio << ','
           << result.write_ratio << ','
           << result.read_qps << ','
           << result.write_qps << ','
           << result.insert_throughput << ','
           << result.latency.avg << ','
           << result.latency.p50 << ','
           << result.latency.p95 << ','
           << result.latency.p99 << ','
           << result.recall_at_10 << ','
           << result.wal_append_avg_us << ','
           << result.memtable_insert_avg_us << ','
           << result.flush_duration_ms << ','
           << result.sstable_count << ','
           << result.delta_record_count << ','
           << result.memory_usage_mb << ','
           << result.disk_usage_mb << ','
           << result.recovery_time_ms << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (!args.index_path.empty()) {
      std::cerr << "warning: --index_path is accepted for compatibility; "
                   "this P5 benchmark uses exact in-memory base search.\n";
    }

    const auto base = make_dataset(args);
    if (base.empty()) {
      throw std::runtime_error("Base dataset is empty");
    }
    const auto queries =
        make_queries(base, std::max<std::size_t>(1, args.query_count), args.seed);

    std::vector<ScenarioResult> results;
    for (const auto& scenario : scenarios_for(args)) {
      results.push_back(run_scenario(args, scenario, base, queries));
    }
    write_csv(args.output, results);

    std::cout << "p5_mixed_rw_output=" << args.output.string() << "\n";
    for (const auto& result : results) {
      std::cout << result.scenario << ": read_qps=" << result.read_qps
                << " write_qps=" << result.write_qps
                << " p95_ms=" << result.latency.p95
                << " p99_ms=" << result.latency.p99
                << " recovery_ms=" << result.recovery_time_ms << "\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
