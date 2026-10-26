#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "core/platform_io.h"
#include "data/dataset.h"
#include "graph/disk_graph_index.h"

namespace {

// SIFT benchmark 负责构建或打开 packed graph 索引、执行 ANN 搜索，并输出 recall、latency、I/O 和 cache JSON。
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Args {
  bool help = false;
  bool synthetic = false;
  std::string sift_dir = "data/sift";
  std::string base_path;
  std::string query_path;
  std::string truth_path;
  std::size_t base_limit = 1000000;
  std::size_t query_offset = 0;
  std::size_t query_limit = 1000;
  std::size_t synthetic_dim = 32;
  std::size_t synthetic_clusters = 16;
  std::uint32_t seed = 42;
  std::string run_id = "manual";
  std::string policy_version = "unspecified";
  std::string cache_condition = "unspecified";
  std::size_t warmup_query_limit = 0;

  fs::path index_path = "indexes/sift.idx";
  fs::path manifest_path;
  fs::path output_json = "logs/sift_bench/result.json";
  bool rebuild_index = false;
  std::size_t graph_degree = 32;
  std::size_t page_size = 4096;
  std::string build_policy = "vamana";
  std::string packing_strategy = "bfs";

  bool enable_pq = true;
  std::size_t pq_subspaces = 32;
  std::size_t pq_centroids = 256;
  std::size_t pq_train_limit = 100000;
  std::size_t pq_iterations = 8;
  bool pq_options_explicit = false;
  bool pq_centroids_explicit = false;
  std::size_t rerank_topk = 100;

  std::size_t top_k = 10;
  std::size_t search_width = 156;
  bool beam_width_requested = false;
  std::size_t requested_beam_width = 0;
  std::size_t effective_beam_width = 16;

  std::string io_mode = "io_uring";
  std::size_t io_batch = 16;
  std::size_t io_depth = 32;
  // 默认不开页 cache：1M 实测（io_uring 冷读，2000 条查询）无 cache 21 ms / 0.87 MB
  // 且 recall 0.986；开 lru 12000 页是 101 ms / 0.83 MB，读量只少 5% 而延迟涨 5 倍，
  // 还占掉 13% 预算。cache 省下的那点物理读，用撒点数换回来更划算。
  std::string cache_policy = "none";
  std::size_t cache_pages = 0;
  bool cache_pages_requested = false;
  double memory_budget_ratio = 0.20;
  bool protect_hot_pages = true;
  std::size_t hot_degree_threshold = 0;
  std::size_t num_search_threads = 1;
  bool memory_budget_enforce = true;
  // 冷读实验的硬闸门：索引在计时区间内全程驻留页缓存时，延迟里没有磁盘
  // 成分，任何 I/O 结论都不成立。默认关闭（热缓存实验是合法的），但冷读
  // 对照必须打开它，否则"实验无效"只会变成 JSON 里的一个 true。
  bool require_cold_cache = false;
  bool enable_prefetch = false;
  bool enable_prefetch_explicit = false;
  std::string prefetch_policy = "none";
  bool prefetch_policy_explicit = false;
  bool prefetch_adaptive = true;
  std::size_t prefetch_top_k = 2;
  std::size_t prefetch_max_inflight = 2;
  std::size_t prefetch_max_pages_per_query = 0;
  std::string prefetch_trace_path;
};

struct MemoryBudgetReport {
  std::uint64_t dataset_bytes = 0;
  std::uint64_t budget_bytes = 0;
  // 模型常驻部分：每 worker 的 PQ codes/码本与 node→page 目录。
  std::uint64_t model_bytes = 0;
  std::uint64_t directory_bytes = 0;
  std::uint64_t fixed_resident_bytes = 0;
  std::uint64_t in_degree_bytes = 0;
  std::uint64_t io_buffer_bytes = 0;
  std::uint64_t cache_bytes = 0;
  std::uint64_t estimated_resident_bytes = 0;
  double estimated_resident_ratio = 0.0;
  // 每页解码后的邻接 vector 容量之和（已经计入 cache_bytes）。
  std::uint64_t cache_neighbor_overhead_bytes = 0;
  bool cache_pages_auto = false;
  bool cache_truncated = false;
  bool budget_pass = true;
  std::size_t requested_cache_pages = 0;
};

struct SearchAggregate {
  std::size_t queries = 0;
  std::size_t worker_count = 1;
  double elapsed_ms = 0.0;
  std::vector<double> query_latency_ms;
  double recall_sum = 0.0;
  std::size_t recall_queries = 0;
  agent_aware::DiskGraphSearchStats stats;
  // 计时区间内块设备真实的读写量。与 stats.submitted_reads（pread/io_uring
  // 调用次数）并列报告：两者差距越大，说明越多读被系统页缓存吸收。
  agent_aware::PhysicalIoSnapshot physical_io;
  // 计时开始前、结束后索引文件在页缓存中的驻留比例（-1 表示不可测）。
  double cache_residency_before = -1.0;
  double cache_residency_after = -1.0;
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

std::size_t parse_size_value(const std::string& value,
                             const std::string& option,
                             std::size_t minimum) {
  auto invalid = [&]() {
    std::ostringstream error;
    error << "Invalid " << option << ": must be >= " << minimum;
    return std::runtime_error(error.str());
  };
  if (value.empty() || value.front() == '-') {
    throw invalid();
  }
  std::size_t consumed = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(value, &consumed);
  } catch (const std::exception&) {
    throw invalid();
  }
  if (consumed != value.size() || parsed < minimum) {
    throw invalid();
  }
  return static_cast<std::size_t>(parsed);
}

bool parse_bool_value(const std::string& value, const std::string& option) {
  if (value == "1" || value == "true" || value == "on" ||
      value == "yes") {
    return true;
  }
  if (value == "0" || value == "false" || value == "off" ||
      value == "no") {
    return false;
  }
  throw std::runtime_error("Invalid " + option + ": expected 0 or 1");
}

void print_help() {
  std::cout
      << "Usage: agent-aware-search [options]\n"
      << "  --sift-dir PATH | --query PATH [--truth PATH]\n"
      << "  --index-path PATH --query-offset N --query-limit N\n"
      << "  --manifest PATH (default <index-path>.manifest.json)\n"
      << "  --top-k N --search-width N --beam-width N\n"
      << "  --io-mode NAME --cache-policy NAME --cache-pages N\n"
      << "  --output-json PATH\n";
  return;
  std::cout
      << "Usage: agent-aware [options]\n"
      << "Dataset:\n"
      << "  --synthetic 0|1              Use synthetic data, default 0\n"
      << "  --sift-dir PATH              Use sift_base/query/groundtruth files, default data/sift\n"
      << "  --base PATH                  Base .fvecs path\n"
      << "  --query PATH                 Query .fvecs path\n"
      << "  --truth PATH                 Ground-truth .ivecs path\n"
      << "  --base-limit N               Loaded base vectors, default 1000000\n"
      << "  --query-offset N             First query id to load, default 0\n"
      << "  --query-limit N              Loaded query vectors, default 1000\n"
      << "  --warmup-query-limit N       Untimed same-process warmup queries, default 0\n"
      << "  --run-id ID                  Experiment run identifier, default manual\n"
      << "  --policy-version VERSION     Policy/model version, default unspecified\n"
      << "  --cache-condition NAME       Experiment cache condition metadata\n"
      << "Search:\n"
      << "  --top-k N, --k N             Result count, default 10\n"
      << "  --search-width N             Total expansion budget, default 156\n"
      << "  (Entry point is fixed to the persisted global medoid)\n"
      << "  --beam-width N               Per-round candidate expansions, default 16\n"
      << "  --beam_width N               Compatibility alias for --beam-width\n"
      << "  --num-search-threads N       Query workers, default 1\n"
      << "PQ/ADC:\n"
      << "  --enable-pq 1                Required; non-PQ candidate generation was removed\n"
      << "  --pq-subspaces N             PQ subspaces, default 32\n"
      << "  --pq-centroids N             PQ centroids, 2-256, default 256\n"
      << "  --pq-train-limit N           PQ training vectors, default 100000\n"
      << "  --pq-iterations N            PQ k-means iterations, default 8\n"
      << "  --rerank-topk N              Exact rerank pool after PQ, default 100\n"
      << "Cache/I/O:\n"
      << "  --cache-policy NAME          none/lru/2q/graph-aware-2q/agent, default none\n"
      << "  --cache-pages N              Page cache capacity, default auto within memory budget\n"
      << "  --memory-budget-ratio R      Resident memory budget ratio, default 0.20\n"
      << "  --memory-budget-enforce 0|1  Truncate --cache-pages to the budget and\n"
      << "                               fail on a measured peak-RSS overrun, default 1\n"
      << "  --require-cold-cache 0|1     Fail (exit 4) when the index stays resident\n"
      << "                               in the page cache during the timed region;\n"
      << "                               set 1 for every cold-read experiment, default 0\n"
      << "  --protect-hot-pages 0|1      Compute node in-degrees at configure time,\n"
      << "                               which reads the whole index into the page\n"
      << "                               cache; set 0 for cold experiments, default 1\n"
      << "  --io-mode NAME               pread, odirect, or io_uring, default io_uring\n"
      << "  --io-batch N                 I/O batch size, default 16\n"
      << "  --io-depth N                 I/O depth for io_uring, default 32\n"
      << "  --enable-prefetch 0|1        Enable topology prefetch, default 0\n"
      << "  --prefetch-policy NAME       none/topology, default none\n"
      << "  --prefetch-adaptive 0|1      Adaptive top-K/inflight control, default 1\n"
      << "  --prefetch-top-k N           Maximum pages submitted per window, default 2\n"
      << "  --prefetch-max-inflight N    Maximum pending prefetch pages, default 2\n"
      << "  --prefetch-max-pages-per-query N  Query budget, 0 means unlimited\n"
      << "  --prefetch-trace PATH        Append post-expand page decisions as CSV\n"
      << "Output/build:\n"
      << "  --graph-degree N             Vamana graph degree, default 32\n"
      << "  --page-size N                Packed graph page size, default 4096\n"
      << "  --build-policy NAME          Graph build policy, default vamana\n"
      << "  --packing-strategy NAME      Graph packing strategy, default bfs\n"
      << "  --index-path PATH            Packed graph index path, default indexes/sift.idx\n"
      << "  --rebuild-index 0|1          Rebuild index before search, default 0\n"
      << "  --output-json PATH           Write result JSON to path, default logs/sift_bench/result.json\n";
}

Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    const std::string name = option.substr(0, option.find('='));
    if (name == "--synthetic" || name == "--base" ||
        name == "--base-limit" || name == "--rebuild-index" ||
        name == "--graph-degree" || name == "--page-size" ||
        name == "--build-policy" || name == "--packing-strategy" ||
        name == "--pq-subspaces" || name == "--pq_subspaces" ||
        name == "--pq-centroids" || name == "--pq_centroids" ||
        name == "--pq-train-limit" || name == "--pq_train_limit" ||
        name == "--pq-iterations" || name == "--pq_iterations" ||
        name == "--entry-count" || name == "--entry_count" ||
        name == "--entry-strategy" || name == "--entry_strategy" ||
        name == "--entry-hub-count" || name == "--entry_hub_count" ||
        name == "--entry-cluster-count" || name == "--entry_cluster_count" ||
        name == "--entry-cluster-train-limit" ||
        name == "--entry_cluster_train_limit" ||
        name == "--entry-cluster-iterations" ||
        name == "--entry_cluster_iterations") {
      throw std::runtime_error(
          "Build option is not accepted by agent-aware-search; run agent-aware-build first: " +
          name);
    }
    if (name == "--help" || name == "-h") {
      args.help = true;
    } else if (name == "--synthetic") {
      args.synthetic = parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--sift-dir") {
      args.sift_dir = require_value(i, argc, argv, name);
      args.synthetic = false;
    } else if (name == "--base") {
      args.base_path = require_value(i, argc, argv, name);
      args.synthetic = false;
    } else if (name == "--query") {
      args.query_path = require_value(i, argc, argv, name);
      args.synthetic = false;
    } else if (name == "--truth") {
      args.truth_path = require_value(i, argc, argv, name);
    } else if (name == "--base-limit") {
      args.base_limit = parse_size_value(require_value(i, argc, argv, name),
                                         name, 1);
    } else if (name == "--query-offset" || name == "--query_offset") {
      args.query_offset = parse_size_value(require_value(i, argc, argv, name),
                                           name, 0);
    } else if (name == "--query-limit") {
      args.query_limit = parse_size_value(require_value(i, argc, argv, name),
                                          name, 1);
    } else if (name == "--warmup-query-limit") {
      args.warmup_query_limit =
          parse_size_value(require_value(i, argc, argv, name), name, 0);
    } else if (name == "--run-id") {
      args.run_id = require_value(i, argc, argv, name);
    } else if (name == "--policy-version") {
      args.policy_version = require_value(i, argc, argv, name);
    } else if (name == "--cache-condition") {
      args.cache_condition = require_value(i, argc, argv, name);
    } else if (name == "--synthetic-dim") {
      args.synthetic_dim = parse_size_value(require_value(i, argc, argv, name),
                                            name, 1);
    } else if (name == "--synthetic-clusters") {
      args.synthetic_clusters =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else if (name == "--seed") {
      args.seed = static_cast<std::uint32_t>(
          parse_size_value(require_value(i, argc, argv, name), name, 0));
    } else if (name == "--index-path") {
      args.index_path = require_value(i, argc, argv, name);
    } else if (name == "--manifest") {
      args.manifest_path = require_value(i, argc, argv, name);
    } else if (name == "--output-json") {
      args.output_json = require_value(i, argc, argv, name);
    } else if (name == "--rebuild-index") {
      args.rebuild_index =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--graph-degree") {
      args.graph_degree = parse_size_value(require_value(i, argc, argv, name),
                                           name, 1);
    } else if (name == "--page-size") {
      args.page_size = parse_size_value(require_value(i, argc, argv, name),
                                        name, 4096);
    } else if (name == "--build-policy") {
      args.build_policy = require_value(i, argc, argv, name);
    } else if (name == "--packing-strategy") {
      args.packing_strategy = require_value(i, argc, argv, name);
    } else if (name == "--enable-pq" || name == "--pq-enable" ||
               name == "--adc-enable") {
      args.enable_pq =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--pq-subspaces" || name == "--pq_subspaces") {
      args.pq_subspaces = parse_size_value(require_value(i, argc, argv, name),
                                           name, 1);
      args.pq_options_explicit = true;
    } else if (name == "--pq-centroids" || name == "--pq_centroids") {
      args.pq_centroids = parse_size_value(require_value(i, argc, argv, name),
                                           name, 2);
      args.pq_centroids_explicit = true;
    } else if (name == "--pq-train-limit" || name == "--pq_train_limit") {
      args.pq_train_limit =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else if (name == "--pq-iterations" || name == "--pq_iterations") {
      args.pq_iterations = parse_size_value(require_value(i, argc, argv, name),
                                            name, 1);
    } else if (name == "--rerank-topk" || name == "--rerank_topk") {
      args.rerank_topk = parse_size_value(require_value(i, argc, argv, name),
                                          name, 0);
    } else if (name == "--top-k" || name == "--k") {
      args.top_k = parse_size_value(require_value(i, argc, argv, name),
                                    name, 1);
    } else if (name == "--search-width" || name == "--search_width") {
      args.search_width = parse_size_value(require_value(i, argc, argv, name),
                                           name, 1);
    } else if (name == "--beam-width" || name == "--beam_width") {
      args.requested_beam_width =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
      args.beam_width_requested = true;
    } else if (name == "--io-mode" || name == "--io_mode") {
      args.io_mode = require_value(i, argc, argv, name);
    } else if (name == "--io-batch" || name == "--io_batch") {
      args.io_batch = parse_size_value(require_value(i, argc, argv, name),
                                       name, 1);
    } else if (name == "--io-depth" || name == "--io_depth") {
      args.io_depth = parse_size_value(require_value(i, argc, argv, name),
                                       name, 1);
    } else if (name == "--enable-prefetch") {
      args.enable_prefetch =
          parse_bool_value(require_value(i, argc, argv, name), name);
      args.enable_prefetch_explicit = true;
    } else if (name == "--prefetch-policy") {
      args.prefetch_policy = require_value(i, argc, argv, name);
      args.prefetch_policy_explicit = true;
    } else if (name == "--prefetch-adaptive") {
      args.prefetch_adaptive =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--prefetch-top-k") {
      args.prefetch_top_k =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else if (name == "--prefetch-max-inflight") {
      args.prefetch_max_inflight =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else if (name == "--prefetch-max-pages-per-query") {
      args.prefetch_max_pages_per_query =
          parse_size_value(require_value(i, argc, argv, name), name, 0);
    } else if (name == "--prefetch-trace") {
      args.prefetch_trace_path = require_value(i, argc, argv, name);
    } else if (name == "--cache-policy" || name == "--cache_policy") {
      args.cache_policy = require_value(i, argc, argv, name);
    } else if (name == "--cache-pages" || name == "--cache_pages") {
      args.cache_pages = parse_size_value(require_value(i, argc, argv, name),
                                          name, 0);
      args.cache_pages_requested = true;
    } else if (name == "--memory-budget-ratio" ||
               name == "--memory_budget_ratio") {
      args.memory_budget_ratio = std::stod(require_value(i, argc, argv, name));
    } else if (name == "--memory-budget-enforce" ||
               name == "--memory_budget_enforce") {
      args.memory_budget_enforce =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--protect-hot-pages") {
      args.protect_hot_pages =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--require-cold-cache" ||
               name == "--require_cold_cache") {
      args.require_cold_cache =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--hot-degree-threshold") {
      args.hot_degree_threshold =
          parse_size_value(require_value(i, argc, argv, name), name, 0);
    } else if (name == "--num-search-threads" ||
               name == "--num_search_threads" ||
               name == "--search-threads") {
      args.num_search_threads =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else {
      throw std::runtime_error("Unknown option: " + option);
    }
  }
  return args;
}

void validate_and_finalize_args(Args& args) {
  if (args.build_policy != "vamana") {
    throw std::runtime_error("Unsupported --build-policy: only vamana is supported");
  }
  if (args.packing_strategy != "bfs") {
    throw std::runtime_error("Unsupported --packing-strategy: only bfs is supported");
  }

  args.effective_beam_width = std::clamp(
      args.beam_width_requested ? args.requested_beam_width
                                : args.effective_beam_width,
      std::size_t{1}, args.search_width);

  if (!args.enable_pq) {
    throw std::runtime_error(
        "--enable-pq=0 is no longer supported; search requires PQ+ADC candidate generation");
  }
  if (args.prefetch_policy != "none" &&
      args.prefetch_policy != "topology") {
    throw std::runtime_error(
        "Invalid --prefetch-policy: expected none or topology");
  }
  if (args.prefetch_policy_explicit && !args.enable_prefetch_explicit) {
    args.enable_prefetch = args.prefetch_policy == "topology";
  }
  if (args.enable_prefetch && !args.prefetch_policy_explicit) {
    args.prefetch_policy = "topology";
  }
  if (!args.enable_prefetch) args.prefetch_policy = "none";

  if (args.enable_pq) {
    if (args.pq_subspaces < 1) {
      throw std::runtime_error("Invalid --pq-subspaces: must be >= 1");
    }
    if (args.pq_centroids < 2) {
      throw std::runtime_error("Invalid --pq-centroids: must be >= 2");
    }
    if (args.pq_centroids > 256) {
      throw std::runtime_error("Invalid --pq-centroids: must be <= 256");
    }
    if (args.pq_train_limit < args.pq_centroids) {
      throw std::runtime_error(
          "Invalid --pq-train-limit: must be >= --pq-centroids");
    }
    if (args.pq_iterations < 1) {
      throw std::runtime_error("Invalid --pq-iterations: must be >= 1");
    }
    if (args.rerank_topk != 0 && args.rerank_topk < args.top_k) {
      throw std::runtime_error(
          "Invalid --rerank-topk: must be 0 or >= --top-k");
    }
  }
  if (args.memory_budget_ratio <= 0.0 || args.memory_budget_ratio > 1.0) {
    throw std::runtime_error(
        "Invalid --memory-budget-ratio: must be in (0, 1]");
  }
  if (args.run_id.empty()) {
    throw std::runtime_error("Invalid --run-id: must not be empty");
  }
  if (args.policy_version.empty()) {
    throw std::runtime_error("Invalid --policy-version: must not be empty");
  }
  if (args.cache_condition.empty()) {
    throw std::runtime_error("Invalid --cache-condition: must not be empty");
  }
  if (args.warmup_query_limit > 0 && args.num_search_threads != 1) {
    throw std::runtime_error(
        "--warmup-query-limit currently requires --num-search-threads 1");
  }
}

std::string json_escape(const std::string& value) {
  std::ostringstream escaped;
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped << "\\\\";
        break;
      case '"':
        escaped << "\\\"";
        break;
      case '\n':
        escaped << "\\n";
        break;
      case '\r':
        escaped << "\\r";
        break;
      case '\t':
        escaped << "\\t";
        break;
      default:
        escaped << ch;
        break;
    }
  }
  return escaped.str();
}

std::vector<std::string> command_lines(const std::string& command) {
  std::vector<std::string> lines;
#if defined(_WIN32)
  FILE* pipe = _popen(command.c_str(), "r");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (pipe == nullptr) {
    return lines;
  }
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    std::string line(buffer);
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
#if defined(_WIN32)
  (void)_pclose(pipe);
#else
  (void)pclose(pipe);
#endif
  return lines;
}

std::string first_command_line(const std::string& command) {
  const auto lines = command_lines(command);
  return lines.empty() ? std::string("unknown") : lines.front();
}

long long binary_mtime_seconds(const fs::path& path) {
#ifndef _WIN32
  struct stat st;
  const std::string path_string = path.string();
  if (stat(path_string.c_str(), &st) == 0) {
    return static_cast<long long>(st.st_mtime);
  }
#else
  (void)path;
#endif
  return -1;
}

struct BuildManifest {
  std::uint64_t format_version = 0;
  std::uint64_t vector_count = 0;
  std::uint64_t dim = 0;
  std::uint64_t index_bytes = 0;
  std::string entry_strategy;
  std::uint64_t entry_id = 0;
};

std::string read_text_file(const fs::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error(
        "Build manifest does not exist; run agent-aware-build first: " +
        path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::uint64_t manifest_integer(const std::string& text,
                               const std::string& field) {
  const std::regex key_pattern("\\\"" + field + "\\\"\\s*:");
  if (std::distance(std::sregex_iterator(text.begin(), text.end(), key_pattern),
                    std::sregex_iterator()) != 1) {
    throw std::runtime_error(
        "Build manifest must contain exactly one field: " + field);
  }
  const std::regex pattern("\\\"" + field +
                           "\\\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  if (!std::regex_search(text, match, pattern)) {
    throw std::runtime_error("Build manifest is missing integer field: " + field);
  }
  std::size_t consumed = 0;
  const auto value = std::stoull(match[1].str(), &consumed);
  if (consumed != match[1].str().size()) {
    throw std::runtime_error("Build manifest has invalid integer field: " + field);
  }
  return value;
}

std::string manifest_string(const std::string& text,
                            const std::string& field) {
  const std::regex key_pattern("\\\"" + field + "\\\"\\s*:");
  if (std::distance(std::sregex_iterator(text.begin(), text.end(), key_pattern),
                    std::sregex_iterator()) != 1) {
    throw std::runtime_error(
        "Build manifest must contain exactly one field: " + field);
  }
  const std::regex pattern("\\\"" + field +
                           "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
  std::smatch match;
  if (!std::regex_search(text, match, pattern)) {
    throw std::runtime_error("Build manifest is missing string field: " + field);
  }
  return match[1].str();
}

BuildManifest load_build_manifest(
    const fs::path& path, const agent_aware::DiskGraphMetadata& metadata) {
  const std::string text = read_text_file(path);
  const std::string pair =
      "\\\"[^\\\"]+\\\"\\s*:\\s*(?:[0-9]+|\\\"[^\\\"]*\\\")";
  if (!std::regex_match(
          text, std::regex("\\s*\\{\\s*(?:" + pair +
                           "(?:\\s*,\\s*" + pair + ")*)?\\s*\\}\\s*"))) {
    throw std::runtime_error("Build manifest is not a JSON object: " + path.string());
  }
  BuildManifest manifest;
  manifest.format_version = manifest_integer(text, "format_version");
  manifest.vector_count = manifest_integer(text, "vector_count");
  manifest.dim = manifest_integer(text, "dim");
  manifest.index_bytes = manifest_integer(text, "index_bytes");
  manifest.entry_strategy = manifest_string(text, "entry_strategy");
  manifest.entry_id = manifest_integer(text, "entry_id");
  if (manifest.format_version != metadata.format_version ||
      manifest.vector_count != metadata.vector_count ||
      manifest.dim != metadata.dim || manifest.index_bytes != metadata.index_bytes) {
    throw std::runtime_error(
        "Build manifest does not match packed index metadata; rerun agent-aware-build");
  }
  if (manifest.entry_strategy != "single-medoid") {
    throw std::runtime_error(
        "Build manifest entry_strategy must be single-medoid");
  }
  if (manifest.entry_id >= metadata.vector_count) {
    throw std::runtime_error("Build manifest entry_id exceeds packed index vector count");
  }
  return manifest;
}

agent_aware::LoadedQueryDataset load_input_dataset(
    const Args& args, const agent_aware::DiskGraphMetadata& metadata) {
  agent_aware::QueryDatasetLoadConfig config;
  config.query_offset = args.query_offset;
  config.query_limit = args.query_limit;
  config.paths.sift_dir = args.sift_dir;
  config.paths.query = args.query_path;
  config.paths.truth = args.truth_path;
  config.expected_dim = metadata.dim;
  config.expected_base_count = static_cast<std::size_t>(metadata.vector_count);
  return agent_aware::load_query_dataset(config);
}

double recall_at_k(const std::vector<agent_aware::SearchResult>& result,
                   const std::vector<std::uint32_t>& truth,
                   std::size_t k) {
  if (k == 0 || truth.empty()) {
    return 1.0;
  }
  const std::size_t truth_k = std::min(k, truth.size());
  std::unordered_set<std::uint32_t> truth_ids;
  truth_ids.reserve(truth_k);
  for (std::size_t i = 0; i < truth_k; ++i) {
    truth_ids.insert(truth[i]);
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

double average_latency_ms(const std::vector<double>& latencies) {
  if (latencies.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for (const double latency : latencies) {
    sum += latency;
  }
  return sum / static_cast<double>(latencies.size());
}

double percentile_latency_ms(std::vector<double> latencies,
                             double percentile) {
  if (latencies.empty()) {
    return 0.0;
  }
  std::sort(latencies.begin(), latencies.end());
  std::size_t rank = static_cast<std::size_t>(
      std::ceil((percentile / 100.0) *
                static_cast<double>(latencies.size())));
  rank = std::clamp<std::size_t>(rank, 1, latencies.size());
  return latencies[rank - 1];
}

void add_stats(SearchAggregate& aggregate,
               const agent_aware::DiskGraphSearchStats& stats) {
  aggregate.stats.node_reads += stats.node_reads;
  aggregate.stats.expanded += stats.expanded;
  aggregate.stats.visited += stats.visited;
  aggregate.stats.page_requests += stats.page_requests;
  aggregate.stats.cache_hits += stats.cache_hits;
  aggregate.stats.pinned_hits += stats.pinned_hits;
  aggregate.stats.demand_reads += stats.demand_reads;
  aggregate.stats.unique_nodes_accessed += stats.unique_nodes_accessed;
  aggregate.stats.unique_pages_touched += stats.unique_pages_touched;
  aggregate.stats.unique_demand_pages += stats.unique_demand_pages;
  aggregate.stats.duplicate_skipped += stats.duplicate_skipped;
  aggregate.stats.submitted_reads += stats.submitted_reads;
  aggregate.stats.completed_reads += stats.completed_reads;
  aggregate.stats.page_requests_before_dedup +=
      stats.page_requests_before_dedup;
  aggregate.stats.page_requests_after_dedup += stats.page_requests_after_dedup;
  aggregate.stats.page_cache_hits += stats.page_cache_hits;
  aggregate.stats.page_cache_misses += stats.page_cache_misses;
  aggregate.stats.page_cache_evictions += stats.page_cache_evictions;
  aggregate.stats.page_cache_promotions += stats.page_cache_promotions;
  aggregate.stats.io_submits += stats.io_submits;
  aggregate.stats.io_completions += stats.io_completions;
  aggregate.stats.io_submit_syscalls += stats.io_submit_syscalls;
  aggregate.stats.uring_submit_count += stats.uring_submit_count;
  aggregate.stats.uring_cqe_count += stats.uring_cqe_count;
  aggregate.stats.demand_read_wait_us += stats.demand_read_wait_us;
  aggregate.stats.page_dedup_requests += stats.page_dedup_requests;
  aggregate.stats.page_dedup_hits += stats.page_dedup_hits;
  aggregate.stats.duplicate_pages_eliminated +=
      stats.duplicate_pages_eliminated;
  aggregate.stats.batch_count += stats.batch_count;
  aggregate.stats.batch_expanded += stats.batch_expanded;
  aggregate.stats.max_batch_size =
      std::max(aggregate.stats.max_batch_size, stats.max_batch_size);
  aggregate.stats.beam_unique_pages += stats.beam_unique_pages;
  aggregate.stats.beam_cached_pages += stats.beam_cached_pages;
  aggregate.stats.same_page_node_reuse += stats.same_page_node_reuse;
  aggregate.stats.distance_direct_calls += stats.distance_direct_calls;
  aggregate.stats.adc_table_build_us += stats.adc_table_build_us;
  aggregate.stats.rerank_reads += stats.rerank_reads;
  aggregate.stats.pq_filter_accept_count += stats.pq_filter_accept_count;
  aggregate.stats.search_mutex_wait_us += stats.search_mutex_wait_us;
  aggregate.stats.unique_prefetch_pages += stats.unique_prefetch_pages;
  aggregate.stats.prefetch_candidate_nodes += stats.prefetch_candidate_nodes;
  aggregate.stats.prefetch_candidate_pages += stats.prefetch_candidate_pages;
  aggregate.stats.prefetch_submitted += stats.prefetch_submitted;
  aggregate.stats.prefetch_submit_batches += stats.prefetch_submit_batches;
  aggregate.stats.prefetch_ready_hit += stats.prefetch_ready_hit;
  aggregate.stats.prefetch_pending_hit += stats.prefetch_pending_hit;
  aggregate.stats.prefetch_useful_pages += stats.prefetch_useful_pages;
  aggregate.stats.prefetch_wasted_pages += stats.prefetch_wasted_pages;
  aggregate.stats.prefetch_skip_cached += stats.prefetch_skip_cached;
  aggregate.stats.prefetch_skip_pending += stats.prefetch_skip_pending;
  aggregate.stats.prefetch_skip_seen += stats.prefetch_skip_seen;
  aggregate.stats.prefetch_skip_budget += stats.prefetch_skip_budget;
  aggregate.stats.demand_priority_blocks_prefetch +=
      stats.demand_priority_blocks_prefetch;
  aggregate.stats.prefetch_adaptive_pause_count +=
      stats.prefetch_adaptive_pause_count;
  aggregate.stats.prefetch_adaptive_final_top_k =
      stats.prefetch_adaptive_final_top_k;
  aggregate.stats.prefetch_adaptive_final_lead =
      stats.prefetch_adaptive_final_lead;
  aggregate.stats.prefetch_planner_cpu_us += stats.prefetch_planner_cpu_us;
}

void merge_aggregate(SearchAggregate& aggregate,
                     const SearchAggregate& partial) {
  aggregate.queries += partial.queries;
  aggregate.query_latency_ms.insert(aggregate.query_latency_ms.end(),
                                    partial.query_latency_ms.begin(),
                                    partial.query_latency_ms.end());
  aggregate.recall_sum += partial.recall_sum;
  aggregate.recall_queries += partial.recall_queries;
  add_stats(aggregate, partial.stats);
}

void configure_index_from_args(const Args& args,
                               agent_aware::PackedDiskGraphIndex& index) {
  index.configure_cache(args.cache_policy, args.cache_pages,
                        args.protect_hot_pages,
                        args.hot_degree_threshold);
  index.configure_io(args.io_mode, args.io_batch, args.io_depth);
}

std::vector<std::uint32_t> select_search_entries(
    const BuildManifest& manifest) {
  return {static_cast<std::uint32_t>(manifest.entry_id)};
}

agent_aware::PQTrainingStats pq_stats_from_model(
    const agent_aware::PqAdcModel& model, std::size_t training_vectors) {
  return agent_aware::PQTrainingStats{
      model.vector_count(), training_vectors, model.dim(), model.subspaces(),
      model.centroids(), model.code_bytes(), model.codebook_bytes()};
}

// 搜索阶段复用索引内的 PQ 模型，而不是重新训练：训练已在 agent-aware-build
// 时完成并写进 V3 索引，重训既会改变码本（索引里的 codes 对不上），也会让
// 每个 benchmark 进程额外付出训练时间。这里只记录模型规模。
std::optional<agent_aware::PQTrainingStats> load_index_pq_stats(
    const agent_aware::PqAdcModel& model) {
  if (!model.enabled()) {
    throw std::runtime_error("Packed graph index does not contain a PQ model");
  }
  return pq_stats_from_model(model, 0);
}

MemoryBudgetReport apply_memory_budget_defaults(
    Args& args, const agent_aware::LoadedQueryDataset& dataset,
    const agent_aware::DiskGraphMetadata& metadata,
    const std::optional<agent_aware::PQTrainingStats>& pq_stats,
    std::vector<std::string>& warnings) {
  MemoryBudgetReport report;
  report.dataset_bytes = metadata.vector_count * metadata.dim * sizeof(float);
  report.budget_bytes = static_cast<std::uint64_t>(
      std::floor(static_cast<double>(report.dataset_bytes) *
                 args.memory_budget_ratio));
  const std::uint64_t pq_bytes =
      pq_stats.has_value()
          ? static_cast<std::uint64_t>(pq_stats->code_bytes) +
                static_cast<std::uint64_t>(pq_stats->codebook_bytes)
          : 0;
  const std::size_t workers =
      std::min(args.num_search_threads,
               std::max<std::size_t>(1, dataset.queries.size()));
  // 多线程搜索每个 worker 都会打开一份 index：PQ 码、目录、页 cache、io_uring
  // 对齐缓冲都按 worker 数放大（主线程那份不计入搜索期常驻）。hub-aware 策略下
  // 每个 worker 还会额外持有一份 node 入度表。
  const std::size_t copies = workers;
  report.model_bytes = pq_bytes * copies;
  report.directory_bytes =
      static_cast<std::uint64_t>(metadata.vector_count) *
      sizeof(std::uint32_t) * copies;
  const bool needs_in_degrees =
      args.cache_policy == "graph-aware-2q" || args.cache_policy == "agent" ||
      args.protect_hot_pages;
  report.in_degree_bytes =
      needs_in_degrees
          ? static_cast<std::uint64_t>(metadata.vector_count) *
                sizeof(std::uint32_t) * copies
          : 0;
  // io_uring 模式下每个 index 持有 io_depth 个页大小对齐缓冲。
  const bool uring_possible = args.io_mode == "io_uring";
  report.io_buffer_bytes =
      uring_possible
          ? static_cast<std::uint64_t>(args.io_depth) * metadata.page_size *
                copies
          : 0;
  report.fixed_resident_bytes = report.model_bytes + report.directory_bytes +
                                report.in_degree_bytes +
                                report.io_buffer_bytes;

  // 解码页的真实常驻代价 = 页字节 + 每个节点一份邻接 vector，比 page_size 大得多；
  // 预算必须按它换算 cache 页数，否则"预算内"只是公式上的。
  const std::size_t resident_page_bytes =
      agent_aware::PackedDiskGraphIndex::resident_page_bytes(metadata);
  report.cache_neighbor_overhead_bytes =
      static_cast<std::uint64_t>(resident_page_bytes - metadata.page_size);

  const std::uint64_t available =
      report.budget_bytes > report.fixed_resident_bytes
          ? report.budget_bytes - report.fixed_resident_bytes
          : 0;
  const std::uint64_t max_total_pages =
      resident_page_bytes == 0 ? 0 : available / resident_page_bytes;
  const auto pages_per_worker = static_cast<std::size_t>(
      max_total_pages / std::max<std::size_t>(1, workers));

  if (!args.cache_pages_requested && args.cache_policy != "none") {
    report.cache_pages_auto = true;
    args.cache_pages = pages_per_worker;
    if (args.cache_pages == 0 && available > 0) {
      warnings.push_back(
          "memory budget leaves less than one decoded cache page per worker");
    }
  } else if (args.cache_policy != "none" &&
             args.cache_pages > pages_per_worker) {
    report.requested_cache_pages = args.cache_pages;
    // 预算从"提示"变成硬约束：超出时截断到预算允许的页数并如实记录，
    // --memory-budget-enforce 0 仅用于对照实验。
    if (args.memory_budget_enforce) {
      args.cache_pages = pages_per_worker;
      report.cache_truncated = true;
      warnings.push_back(
          "requested --cache-pages exceeds the memory budget; truncated to " +
          std::to_string(pages_per_worker) + " pages per worker");
    } else {
      report.budget_pass = false;
      warnings.push_back(
          "--memory-budget-enforce 0 and --cache-pages exceed the memory "
          "budget; budget_pass=false");
    }
  } else {
    report.requested_cache_pages = args.cache_pages;
  }
  report.cache_bytes = static_cast<std::uint64_t>(args.cache_pages) *
                       resident_page_bytes * workers;
  report.estimated_resident_bytes =
      report.fixed_resident_bytes + report.cache_bytes;
  report.estimated_resident_ratio =
      report.dataset_bytes == 0
          ? 0.0
          : static_cast<double>(report.estimated_resident_bytes) /
                static_cast<double>(report.dataset_bytes);
  // 估算值必须等于"实际生效"的 cache 页数：被截断或超过预算时仍按请求页数
  // 估算，会把 estimated_resident_ratio 报成一个并不会发生的配置。
  if (report.fixed_resident_bytes > report.budget_bytes) {
    report.budget_pass = false;
    warnings.push_back(
        "PQ codes + codebook + directory alone exceed the dataset memory "
        "budget; no cache configuration can bring the process inside it");
  }
  if (report.estimated_resident_bytes > report.budget_bytes &&
      !report.cache_truncated) {
    report.budget_pass = false;
    warnings.push_back(
        "estimated resident memory exceeds configured dataset budget");
  }
  return report;
}

agent_aware::DiskGraphSearchConfig make_search_config(
    const Args& args, const agent_aware::PqAdcModel* pq_model,
    const std::vector<std::uint32_t>& entry_seed_ids) {
  agent_aware::DiskGraphSearchConfig search;
  search.top_k = args.top_k;
  search.search_width = args.search_width;
  search.beam_width = args.effective_beam_width;
  search.entry_count = 1;
  search.seed_ids = entry_seed_ids;
  search.adc_enable = args.enable_pq && pq_model != nullptr &&
                      pq_model->enabled();
  search.pq_model = search.adc_enable ? pq_model : nullptr;
  search.rerank_topk = search.adc_enable ? args.rerank_topk : 0;
  search.prefetch_policy = args.prefetch_policy;
  search.prefetch_adaptive = args.prefetch_adaptive;
  search.prefetch_top_k = args.prefetch_top_k;
  search.prefetch_max_inflight = args.prefetch_max_inflight;
  search.prefetch_max_pages_per_query = args.prefetch_max_pages_per_query;
  search.prefetch_trace_path = args.prefetch_trace_path;
  search.run_id = args.run_id;
  return search;
}

SearchAggregate run_search_worker(const Args& args,
                                  const agent_aware::LoadedQueryDataset& dataset,
                                  agent_aware::PackedDiskGraphIndex& index,
                                  const agent_aware::PqAdcModel* pq_model,
                                  const std::vector<std::uint32_t>&
                                      entry_seed_ids,
                                  std::size_t first_query,
                                  std::size_t query_stride,
                                  std::size_t query_count,
                                  std::vector<std::vector<std::uint32_t>>*
                                      detail_ids = nullptr,
                                  std::vector<double>* detail_recall = nullptr,
                                  std::size_t detail_capacity = 0) {
  SearchAggregate aggregate;
  for (std::size_t i = first_query; i < query_count;
       i += query_stride) {
    // worker 以 stride 方式遍历 query，避免共享可变 query 状态，并保持每个 worker 的延迟样本独立。
    auto search = make_search_config(args, pq_model, entry_seed_ids);
    search.query_id = args.query_offset + i;
    const auto query_start = Clock::now();
    const auto result = index.search_one(dataset.queries.row(i), search);
    const auto query_end = Clock::now();
    if (std::getenv("AGENT_AWARE_TRACE_RSS") != nullptr && i % 10 == 0) {
      std::fprintf(stderr, "query %zu rss=%.1fMB cache_pages=%zu\n", i,
                   static_cast<double>(agent_aware::process_current_resident_bytes()) / 1e6,
                   index.metadata().page_count);
    }
    aggregate.query_latency_ms.push_back(
        std::chrono::duration<double, std::milli>(query_end - query_start)
            .count());
    ++aggregate.queries;
    add_stats(aggregate, result.stats);
    if (dataset.truth_from_file && i < dataset.truth.size()) {
      const double query_recall =
          recall_at_k(result.topk, dataset.truth[i], args.top_k);
      aggregate.recall_sum += query_recall;
      ++aggregate.recall_queries;
      // 单查询明细落盘，便于回答"哪类查询召不回"这类问题。多 worker 时各线程
      // 写不同条目，汇总在 run_searches 里完成。
      if (detail_ids != nullptr && detail_recall != nullptr &&
          i < detail_capacity) {
        (*detail_ids)[i].clear();
        for (const auto& r : result.topk) {
          (*detail_ids)[i].push_back(r.id);
        }
        (*detail_recall)[i] = query_recall;
      }
    }
  }
  return aggregate;
}

SearchAggregate run_searches(const Args& args,
                             const agent_aware::LoadedQueryDataset& dataset,
                             agent_aware::PackedDiskGraphIndex& index,
                             const agent_aware::PqAdcModel* pq_model,
                             const std::vector<std::uint32_t>&
                                 entry_seed_ids,
                             std::size_t query_limit = 0,
                             std::vector<std::vector<std::uint32_t>>*
                                 detail_ids = nullptr,
                             std::vector<double>* detail_recall = nullptr) {
  const std::size_t query_count =
      query_limit == 0
          ? dataset.queries.size()
          : std::min(query_limit, dataset.queries.size());
  const std::size_t worker_count =
      std::min(args.num_search_threads, std::max<std::size_t>(1, query_count));

  SearchAggregate aggregate;
  aggregate.worker_count = worker_count;
  // 页缓存在计时区间内必须保持稳定：如果中途被逐出或重新填充，读延迟的
  // 变化会被误读成算法收益。这里记录前后的驻留比例，供事后判断实验有效性。
  const std::string index_path_str = args.index_path.string();
  aggregate.cache_residency_before =
      agent_aware::page_cache_residency_ratio(index_path_str.c_str());
  const auto physical_before = agent_aware::physical_io_snapshot_for_path(
      index_path_str.c_str());
  // 单查询明细最多记录前 256 条：足以定位召回失败的模式，又不会让 JSON 膨胀。
  constexpr std::size_t kDetailCapacity = 256;
  std::vector<std::vector<std::uint32_t>> local_ids;
  std::vector<double> local_recall;
  if (detail_ids == nullptr) {
    detail_ids = &local_ids;
    detail_recall = &local_recall;
  }
  detail_ids->clear();
  detail_recall->clear();
  if (dataset.truth_from_file) {
    detail_ids->resize(std::min(query_count, kDetailCapacity));
    detail_recall->assign(detail_ids->size(), -1.0);
  }
  const std::size_t detail_capacity = detail_ids->size();
  const auto start = Clock::now();

  if (worker_count == 1) {
    merge_aggregate(
        aggregate,
        run_search_worker(args, dataset, index, pq_model, entry_seed_ids, 0,
                          worker_count, query_count, detail_ids, detail_recall,
                          detail_capacity));
  } else {
    // 每个 worker 打开自己的 PackedDiskGraphIndex，因为本 benchmark 中页 cache 和异步 I/O 状态按 index/thread 隔离。
    std::vector<SearchAggregate> partials(worker_count);
    std::vector<std::exception_ptr> errors(worker_count);
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      workers.emplace_back([&, worker]() {
        try {
          agent_aware::PackedDiskGraphIndex worker_index(args.index_path.string());
          configure_index_from_args(args, worker_index);
          partials[worker] = run_search_worker(
              args, dataset, worker_index, pq_model, entry_seed_ids, worker,
              worker_count, query_count, detail_ids, detail_recall,
              detail_capacity);
        } catch (...) {
          errors[worker] = std::current_exception();
        }
      });
    }

    for (auto& worker : workers) {
      worker.join();
    }
    for (const auto& error : errors) {
      if (error != nullptr) {
        std::rethrow_exception(error);
      }
    }
    for (const auto& partial : partials) {
      merge_aggregate(aggregate, partial);
    }
  }

  const auto end = Clock::now();
  aggregate.elapsed_ms =
      std::chrono::duration<double, std::milli>(end - start).count();
  const auto physical_after = agent_aware::physical_io_snapshot_for_path(
      index_path_str.c_str());
  aggregate.physical_io =
      agent_aware::physical_io_delta(physical_before, physical_after);
  aggregate.cache_residency_after =
      agent_aware::page_cache_residency_ratio(index_path_str.c_str());
  return aggregate;
}

std::string json_for_run(const Args& args,
                         const agent_aware::LoadedQueryDataset& dataset,
                         const agent_aware::DiskGraphMetadata& index_metadata,
                         const std::vector<std::vector<std::uint32_t>>&
                             detail_ids,
                         const std::vector<double>& detail_recall,
                         const agent_aware::DiskGraphIoStatus& io_status,
                         const SearchAggregate& aggregate,
                         const fs::path& binary_path,
                         const std::optional<agent_aware::PQTrainingStats>&
                             pq_stats,
                         const MemoryBudgetReport& memory_budget,
                         std::uint64_t baseline_rss,
                         double startup_cache_residency,
                         const std::vector<std::uint32_t>& entry_seed_ids,
                         const std::vector<std::string>& warnings) {
  const double elapsed_s = std::max(1e-9, aggregate.elapsed_ms / 1000.0);
  const double qps = static_cast<double>(aggregate.queries) / elapsed_s;
  const double avg_query_latency_ms =
      average_latency_ms(aggregate.query_latency_ms);
  const double latency_p95_ms =
      percentile_latency_ms(aggregate.query_latency_ms, 95.0);
  const double latency_p99_ms =
      percentile_latency_ms(aggregate.query_latency_ms, 99.0);
  const double latency_p999_ms =
      percentile_latency_ms(aggregate.query_latency_ms, 99.9);
  const double recall =
      aggregate.recall_queries == 0
          ? 0.0
          : aggregate.recall_sum /
                static_cast<double>(aggregate.recall_queries);
  const double avg_batch_size =
      aggregate.stats.batch_count == 0
          ? 0.0
          : static_cast<double>(aggregate.stats.batch_expanded) /
                static_cast<double>(aggregate.stats.batch_count);
  const double query_count_for_rates =
      static_cast<double>(std::max<std::size_t>(1, aggregate.queries));
  const double reads_per_query =
      static_cast<double>(aggregate.stats.submitted_reads) /
      query_count_for_rates;
  const double demand_reads_per_query =
      static_cast<double>(aggregate.stats.demand_reads) /
      query_count_for_rates;
  const double duplicate_skipped_per_query =
      static_cast<double>(aggregate.stats.duplicate_skipped) /
      query_count_for_rates;
  // 命中率的分子与分母必须是同一口径的"一次页需求"。
  // page_requests 含重复请求（同一页在一次批处理里被多次请求），而
  // cache_hits / pinned_hits 只在去重后的路径累加；用含重复的分母会得出
  // 偏低的命中率（实测 0.661，而真实"未走 I/O 即满足"的比例是 0.819）。
  // 这里统一用 page_requests 作分母，分子同时包含会话内已物化（pinned）
  // 与跨查询页缓存命中（cache）两种"没走 I/O"的情形。
  const double cache_hit_rate =
      aggregate.stats.page_requests == 0
          ? 0.0
          : static_cast<double>(aggregate.stats.cache_hits +
                                aggregate.stats.pinned_hits) /
                static_cast<double>(aggregate.stats.page_requests);
  // 单独保留"跨查询页缓存命中"的比例，避免把两种来源混成一个数。
  const double cross_query_cache_hit_rate =
      aggregate.stats.page_requests == 0
          ? 0.0
          : static_cast<double>(aggregate.stats.cache_hits) /
                static_cast<double>(aggregate.stats.page_requests);
  const double nodes_used_per_loaded_page =
      aggregate.stats.unique_pages_touched == 0
          ? 0.0
          : static_cast<double>(aggregate.stats.unique_nodes_accessed) /
                static_cast<double>(aggregate.stats.unique_pages_touched);
  const double page_fill_utilization =
      index_metadata.nodes_per_page == 0
          ? 0.0
          : nodes_used_per_loaded_page /
                static_cast<double>(index_metadata.nodes_per_page);
  const std::string io_mode_summary =
      !io_status.fallback_reason.empty()
          ? "fallback"
          : (io_status.io_uring_enabled ? "io_uring" : "sync");
  const auto dirty_files =
#if defined(_WIN32)
      command_lines("git status --porcelain --untracked-files=all 2>nul");
#else
      command_lines("git status --porcelain --untracked-files=all 2>/dev/null");
#endif
  const std::string git_commit =
#if defined(_WIN32)
      first_command_line("git rev-parse HEAD 2>nul");
#else
      first_command_line("git rev-parse HEAD 2>/dev/null");
#endif

  std::ostringstream json;
  json << std::boolalpha;
  json << "{\n";
  json << "  \"status\": \"completed\",\n";
  json << "  \"dataset_mode\": \"" << json_escape(dataset.mode) << "\",\n";
  json << "  \"base_count\": " << index_metadata.vector_count << ",\n";
  json << "  \"query_count\": " << dataset.queries.size() << ",\n";
  json << "  \"dim\": " << index_metadata.dim << ",\n";
  json << "  \"query_offset\": " << args.query_offset << ",\n";
  json << "  \"query_limit\": " << args.query_limit << ",\n";
  json << "  \"warmup_query_limit\": " << args.warmup_query_limit << ",\n";
  json << "  \"run_id\": \"" << json_escape(args.run_id) << "\",\n";
  json << "  \"policy_version\": \"" << json_escape(args.policy_version)
       << "\",\n";
  json << "  \"cache_condition\": \""
       << json_escape(args.cache_condition) << "\",\n";
  json << "  \"seed\": " << args.seed << ",\n";
  json << "  \"graph_degree\": " << index_metadata.degree << ",\n";
  json << "  \"page_size\": " << index_metadata.page_size << ",\n";
  json << "  \"index_format_version\": " << index_metadata.format_version
       << ",\n";
  json << "  \"index_bytes\": " << index_metadata.index_bytes << ",\n";
  json << "  \"pq_bytes\": " << index_metadata.pq_bytes << ",\n";
  json << "  \"nodes_per_page\": " << index_metadata.nodes_per_page
       << ",\n";
  json << "  \"page_count\": " << index_metadata.page_count << ",\n";
  const double dataset_vector_bytes =
      static_cast<double>(index_metadata.vector_count) * index_metadata.dim * sizeof(float);
  json << "  \"index_size_ratio_to_dataset\": "
       << (dataset_vector_bytes == 0.0
               ? 0.0
               : static_cast<double>(index_metadata.index_bytes) /
                     dataset_vector_bytes)
       << ",\n";
  json << "  \"truth_source\": \"" << json_escape(dataset.truth_source)
       << "\",\n";
  json << "  \"truth_from_file\": " << dataset.truth_from_file << ",\n";
  json << "  \"requested_search_width\": " << args.search_width << ",\n";
  json << "  \"effective_search_width\": " << args.search_width << ",\n";
  json << "  \"requested_beam_width\": ";
  if (args.beam_width_requested) {
    json << args.requested_beam_width;
  } else {
    json << "null";
  }
  json << ",\n";
  json << "  \"effective_beam_width\": " << args.effective_beam_width
       << ",\n";
  json << "  \"beam_width_supported\": true,\n";
  json << "  \"io_mode\": \"" << io_mode_summary << "\",\n";
  json << "  \"io_requested_mode\": \"" << json_escape(io_status.requested_mode)
       << "\",\n";
  json << "  \"io_effective_mode\": \"" << json_escape(io_status.effective_mode)
       << "\",\n";
  json << "  \"io_uring_enabled\": " << io_status.io_uring_enabled << ",\n";
  json << "  \"odirect_enabled\": " << io_status.direct_enabled << ",\n";
  json << "  \"io_fallback_reason\": \""
       << json_escape(io_status.fallback_reason) << "\",\n";
  json << "  \"enable_prefetch\": " << args.enable_prefetch << ",\n";
  json << "  \"prefetch_policy\": \""
       << json_escape(args.prefetch_policy) << "\",\n";
  json << "  \"prefetch_runtime_enabled\": "
       << (args.prefetch_policy == "topology" && io_status.io_uring_enabled)
       << ",\n";
  json << "  \"prefetch_trigger\": \"post_expand\",\n";
  json << "  \"prefetch_candidate_multiplier\": 2,\n";
  json << "  \"prefetch_adaptive\": " << args.prefetch_adaptive << ",\n";
  json << "  \"prefetch_top_k\": " << args.prefetch_top_k << ",\n";
  json << "  \"prefetch_max_inflight\": " << args.prefetch_max_inflight
       << ",\n";
  json << "  \"prefetch_max_pages_per_query\": "
       << args.prefetch_max_pages_per_query << ",\n";
  json << "  \"cache_policy\": \"" << json_escape(args.cache_policy)
       << "\",\n";
  json << "  \"cache_pages\": " << args.cache_pages << ",\n";
  json << "  \"cache_pages_auto\": " << memory_budget.cache_pages_auto << ",\n";
  json << "  \"memory_budget_ratio\": " << args.memory_budget_ratio << ",\n";
  json << "  \"dataset_bytes\": " << memory_budget.dataset_bytes << ",\n";
  json << "  \"memory_budget_bytes\": " << memory_budget.budget_bytes
       << ",\n";
  json << "  \"fixed_resident_bytes\": "
       << memory_budget.fixed_resident_bytes << ",\n";
  json << "  \"model_bytes\": " << memory_budget.model_bytes << ",\n";
  json << "  \"directory_bytes\": " << memory_budget.directory_bytes
       << ",\n";
  json << "  \"in_degree_bytes\": " << memory_budget.in_degree_bytes << ",\n";
  json << "  \"io_buffer_bytes\": " << memory_budget.io_buffer_bytes << ",\n";
  json << "  \"resident_page_bytes\": "
       << agent_aware::PackedDiskGraphIndex::resident_page_bytes(
              index_metadata)
       << ",\n";
  json << "  \"cache_neighbor_overhead_bytes\": "
       << memory_budget.cache_neighbor_overhead_bytes << ",\n";
  json << "  \"cache_bytes\": " << memory_budget.cache_bytes << ",\n";
  json << "  \"requested_cache_pages\": "
       << memory_budget.requested_cache_pages << ",\n";
  json << "  \"cache_pages_truncated\": " << memory_budget.cache_truncated
       << ",\n";
  json << "  \"memory_budget_enforce\": " << args.memory_budget_enforce
       << ",\n";
  json << "  \"memory_budget_pass\": " << memory_budget.budget_pass << ",\n";
  json << "  \"estimated_resident_bytes\": "
       << memory_budget.estimated_resident_bytes << ",\n";
  json << "  \"estimated_resident_ratio\": "
       << memory_budget.estimated_resident_ratio << ",\n";
  json << "  \"measured_baseline_rss_bytes\": " << baseline_rss << ",\n";
  // 真实内存：VmHWM/PeakWorkingSet 覆盖模型、目录、cache、会话临时对象和其它
  // 未建模的分配，是在"预算公式"之外唯一可被外部工具复核的数字。
  const std::uint64_t measured_peak = agent_aware::process_peak_resident_bytes();
  const std::uint64_t measured_current =
      agent_aware::process_current_resident_bytes();
  json << "  \"measured_peak_rss_bytes\": " << measured_peak << ",\n";
  json << "  \"measured_current_rss_bytes\": " << measured_current << ",\n";
  json << "  \"measured_peak_rss_ratio\": "
       << (memory_budget.dataset_bytes == 0
               ? 0.0
               : static_cast<double>(measured_peak) /
                     static_cast<double>(memory_budget.dataset_bytes))
       << ",\n";
  json << "  \"rss_within_budget\": "
       << (measured_peak != 0 && measured_peak <= memory_budget.budget_bytes)
       << ",\n";
  json << "  \"top_k\": " << args.top_k << ",\n";
  // 入口语义：manifest 里的 entry_id 是"图构建时选出的可达性最好节点"
  // （见 disk_graph_builder.cpp 的 select_entry_node），不是几何 medoid。
  // 搜索时还会在此基础上均匀撒 spread_start_count 个起点，撒点数量必须
  // 如实报告，否则读者会以为只有一个入口。
  json << "  \"entry_count\": 1,\n";
  json << "  \"entry_strategy\": \"reachability-maximizing\",\n";
  json << "  \"spread_start_count\": "
       << agent_aware::DiskGraphSearchConfig{}.spread_start_count << ",\n";
  json << "  \"entry_seed_count\": " << entry_seed_ids.size() << ",\n";
  json << "  \"entry_seed_ids\": [";
  for (std::size_t i = 0; i < entry_seed_ids.size(); ++i) {
    if (i != 0) {
      json << ", ";
    }
    json << entry_seed_ids[i];
  }
  json << "],\n";
  json << "  \"num_search_threads\": " << args.num_search_threads << ",\n";
  json << "  \"effective_num_search_threads\": " << aggregate.worker_count
       << ",\n";
  json << "  \"enable_pq\": " << args.enable_pq << ",\n";
  json << "  \"pq_enabled\": " << args.enable_pq << ",\n";
  json << "  \"pq_adc_enabled\": " << pq_stats.has_value() << ",\n";
  json << "  \"pq_subspaces\": " << index_metadata.pq_subspaces << ",\n";
  json << "  \"effective_pq_subspaces\": ";
  if (pq_stats.has_value()) {
    json << pq_stats->subspaces;
  } else {
    json << "null";
  }
  json << ",\n";
  json << "  \"pq_centroids\": " << index_metadata.pq_centroids << ",\n";
  json << "  \"pq_training_vectors\": ";
  if (pq_stats.has_value()) {
    json << pq_stats->training_vectors;
  } else {
    json << "null";
  }
  json << ",\n";
  json << "  \"pq_code_bytes\": ";
  if (pq_stats.has_value()) {
    json << pq_stats->code_bytes;
  } else {
    json << "null";
  }
  json << ",\n";
  json << "  \"pq_codebook_bytes\": ";
  if (pq_stats.has_value()) {
    json << pq_stats->codebook_bytes;
  } else {
    json << "null";
  }
  json << ",\n";
  json << "  \"rerank_topk\": " << args.rerank_topk << ",\n";
  json << "  \"effective_rerank_topk\": "
       << (pq_stats.has_value() ? args.rerank_topk : std::size_t{0})
       << ",\n";
  json << "  \"index_path\": \"" << json_escape(args.index_path.string())
       << "\",\n";
  json << "  \"build_manifest\": \""
       << json_escape(args.manifest_path.string())
       << "\",\n";
  json << "  \"binary_path\": \"" << json_escape(binary_path.string())
       << "\",\n";
  json << "  \"binary_mtime\": " << binary_mtime_seconds(binary_path)
       << ",\n";
  json << "  \"git_commit\": \"" << json_escape(git_commit) << "\",\n";
  json << "  \"dirty_untracked_files\": [";
  for (std::size_t i = 0; i < dirty_files.size(); ++i) {
    if (i != 0) {
      json << ", ";
    }
    json << "\"" << json_escape(dirty_files[i]) << "\"";
  }
  json << "],\n";
  json << "  \"elapsed_ms\": " << aggregate.elapsed_ms << ",\n";
  json << "  \"qps\": " << qps << ",\n";
  json << "  \"avg_query_latency_ms\": " << avg_query_latency_ms << ",\n";
  json << "  \"latency_p95_ms\": " << latency_p95_ms << ",\n";
  json << "  \"latency_p99_ms\": " << latency_p99_ms << ",\n";
  json << "  \"latency_p999_ms\": " << latency_p999_ms << ",\n";
  json << "  \"bytes_read_per_query\": "
       << static_cast<double>(aggregate.stats.completed_reads) *
              static_cast<double>(index_metadata.page_size) /
              query_count_for_rates
       << ",\n";
  json << "  \"reads_per_query\": " << reads_per_query << ",\n";
  // 物理 I/O：submitted_reads 只是调用次数；这一组才是块设备真实搬运的字节。
  // physical_io_available=false 表示平台不支持（如 Windows），此时任何
  // "I/O 优化" 结论都不成立，必须显式标注而不是留空。
  json << "  \"physical_io_available\": "
       << aggregate.physical_io.valid << ",\n";
  json << "  \"physical_io_device\": \""
       << json_escape(aggregate.physical_io.device) << "\",\n";
  json << "  \"physical_read_bytes\": "
       << aggregate.physical_io.read_sectors * 512ULL << ",\n";
  json << "  \"physical_write_bytes\": "
       << aggregate.physical_io.write_sectors * 512ULL << ",\n";
  json << "  \"physical_read_ops\": " << aggregate.physical_io.read_ops
       << ",\n";
  json << "  \"physical_write_ops\": " << aggregate.physical_io.write_ops
       << ",\n";
  json << "  \"physical_read_bytes_per_query\": "
       << static_cast<double>(aggregate.physical_io.read_sectors * 512ULL) /
              query_count_for_rates
       << ",\n";
  // 逻辑读字节 / 物理读字节。接近 1 表示每次页读都真的落到了设备；
  // 远小于 1 表示大部分读被系统页缓存吸收——此时的延迟不能当作磁盘延迟。
  json << "  \"logical_to_physical_read_ratio\": ";
  {
    const std::uint64_t physical_bytes =
        aggregate.physical_io.read_sectors * 512ULL;
    const double logical_bytes =
        static_cast<double>(aggregate.stats.completed_reads) *
        static_cast<double>(index_metadata.page_size);
    if (!aggregate.physical_io.valid || physical_bytes == 0) {
      json << "null";
    } else {
      json << logical_bytes / static_cast<double>(physical_bytes);
    }
  }
  json << ",\n";
  // 三个采样点共同界定一次实验的缓存状态：
  //   startup    索引对象构造之前（drop_caches 之后应立即接近 0）
  //   before/after  计时区间前后（应保持稳定，否则延迟变化来自缓存而非算法）
  json << "  \"index_cache_residency_startup\": " << startup_cache_residency
       << ",\n";
  json << "  \"index_cache_residency_before\": "
       << aggregate.cache_residency_before << ",\n";
  json << "  \"index_cache_residency_after\": "
       << aggregate.cache_residency_after << ",\n";
  // 计时区间内索引是否全程驻留页缓存。true 时延迟里没有磁盘成分，
  // 不能用于论证磁盘 I/O 优化。
  json << "  \"index_fully_cached_during_run\": "
       << (aggregate.cache_residency_before > 0.9 &&
           aggregate.cache_residency_after > 0.9)
       << ",\n";
  json << "  \"demand_reads_per_query\": " << demand_reads_per_query << ",\n";
  json << "  \"duplicate_skipped_per_query\": "
       << duplicate_skipped_per_query << ",\n";
  json << "  \"cache_hit_rate\": " << cache_hit_rate << ",\n";
  json << "  \"cross_query_cache_hit_rate\": " << cross_query_cache_hit_rate
       << ",\n";
  json << "  \"query_latency_ms\": [";
  for (std::size_t i = 0; i < aggregate.query_latency_ms.size(); ++i) {
    if (i != 0) {
      json << ", ";
    }
    json << aggregate.query_latency_ms[i];
  }
  json << "],\n";
  json << "  \"recall_at_k\": " << recall << ",\n";
  json << "  \"recall_queries\": " << aggregate.recall_queries << ",\n";
  json << "  \"query_details\": [";
  for (std::size_t i = 0; i < detail_ids.size(); ++i) {
    json << (i == 0 ? "\n" : ",\n");
    json << "    {\"query\": " << args.query_offset + i << ", \"recall\": "
         << detail_recall[i] << ", \"topk\": [";
    for (std::size_t j = 0; j < detail_ids[i].size(); ++j) {
      json << (j == 0 ? "" : ", ") << detail_ids[i][j];
    }
    json << "]}";
  }
  json << (detail_ids.empty() ? "],\n" : "\n  ],\n");
  json << "  \"stats\": {\n";
  json << "    \"expanded\": " << aggregate.stats.expanded << ",\n";
  json << "    \"visited\": " << aggregate.stats.visited << ",\n";
  json << "    \"page_reads\": " << aggregate.stats.submitted_reads << ",\n";
  json << "    \"node_reads\": " << aggregate.stats.node_reads << ",\n";
  json << "    \"page_requests\": " << aggregate.stats.page_requests << ",\n";
  json << "    \"cache_hits\": " << aggregate.stats.cache_hits << ",\n";
  json << "    \"pinned_hits\": " << aggregate.stats.pinned_hits << ",\n";
  json << "    \"demand_reads\": " << aggregate.stats.demand_reads << ",\n";
  json << "    \"unique_nodes_accessed\": "
       << aggregate.stats.unique_nodes_accessed << ",\n";
  json << "    \"unique_pages_touched\": "
       << aggregate.stats.unique_pages_touched << ",\n";
  json << "    \"nodes_used_per_loaded_page\": "
       << nodes_used_per_loaded_page << ",\n";
  json << "    \"page_fill_utilization\": " << page_fill_utilization
       << ",\n";
  json << "    \"unique_demand_pages\": "
       << aggregate.stats.unique_demand_pages << ",\n";
  json << "    \"duplicate_skipped\": "
       << aggregate.stats.duplicate_skipped << ",\n";
  json << "    \"submitted_reads\": " << aggregate.stats.submitted_reads
       << ",\n";
  json << "    \"completed_reads\": " << aggregate.stats.completed_reads
       << ",\n";
  json << "    \"page_requests_before_dedup\": "
       << aggregate.stats.page_requests_before_dedup << ",\n";
  json << "    \"page_requests_after_dedup\": "
       << aggregate.stats.page_requests_after_dedup << ",\n";
  json << "    \"cache_hit_rate\": " << cache_hit_rate << ",\n";
  json << "    \"page_cache_hits\": " << aggregate.stats.page_cache_hits
       << ",\n";
  json << "    \"page_cache_misses\": " << aggregate.stats.page_cache_misses
       << ",\n";
  json << "    \"search_mutex_wait_us\": "
       << aggregate.stats.search_mutex_wait_us << ",\n";
  json << "    \"demand_read_wait_us\": "
       << aggregate.stats.demand_read_wait_us << ",\n";
  json << "    \"io_submits\": " << aggregate.stats.io_submits << ",\n";
  json << "    \"io_completions\": " << aggregate.stats.io_completions
       << ",\n";
  json << "    \"io_submit_syscalls\": "
       << aggregate.stats.io_submit_syscalls << ",\n";
  json << "    \"uring_submit_count\": "
       << aggregate.stats.uring_submit_count << ",\n";
  json << "    \"uring_cqe_count\": " << aggregate.stats.uring_cqe_count
       << ",\n";
  json << "    \"batch_count\": " << aggregate.stats.batch_count << ",\n";
  json << "    \"avg_batch_size\": " << avg_batch_size << ",\n";
  json << "    \"max_batch_size\": " << aggregate.stats.max_batch_size
       << ",\n";
  json << "    \"beam_unique_pages\": "
       << aggregate.stats.beam_unique_pages << ",\n";
  json << "    \"beam_cached_pages\": "
       << aggregate.stats.beam_cached_pages << ",\n";
  json << "    \"page_dedup_requests\": "
       << aggregate.stats.page_dedup_requests << ",\n";
  json << "    \"page_dedup_hits\": " << aggregate.stats.page_dedup_hits
       << ",\n";
  json << "    \"duplicate_pages_eliminated\": "
       << aggregate.stats.duplicate_pages_eliminated << ",\n";
  json << "    \"same_page_node_reuse\": "
       << aggregate.stats.same_page_node_reuse << ",\n";
  json << "    \"adc_table_build_us\": "
       << aggregate.stats.adc_table_build_us << ",\n";
  json << "    \"rerank_reads\": " << aggregate.stats.rerank_reads << ",\n";
  json << "    \"pq_filter_accept_count\": "
       << aggregate.stats.pq_filter_accept_count << ",\n";
  json << "    \"distance_direct_calls\": "
       << aggregate.stats.distance_direct_calls << ",\n";
  json << "    \"unique_prefetch_pages\": "
       << aggregate.stats.unique_prefetch_pages << ",\n";
  json << "    \"prefetch_candidate_nodes\": "
       << aggregate.stats.prefetch_candidate_nodes << ",\n";
  json << "    \"prefetch_candidate_pages\": "
       << aggregate.stats.prefetch_candidate_pages << ",\n";
  json << "    \"prefetch_submitted\": "
       << aggregate.stats.prefetch_submitted << ",\n";
  json << "    \"prefetch_submit_batches\": "
       << aggregate.stats.prefetch_submit_batches << ",\n";
  json << "    \"prefetch_ready_hit\": "
       << aggregate.stats.prefetch_ready_hit << ",\n";
  json << "    \"prefetch_pending_hit\": "
       << aggregate.stats.prefetch_pending_hit << ",\n";
  json << "    \"prefetch_useful_pages\": "
       << aggregate.stats.prefetch_useful_pages << ",\n";
  json << "    \"prefetch_wasted_pages\": "
       << aggregate.stats.prefetch_wasted_pages << ",\n";
  json << "    \"prefetch_skip_cached\": "
       << aggregate.stats.prefetch_skip_cached << ",\n";
  json << "    \"prefetch_skip_pending\": "
       << aggregate.stats.prefetch_skip_pending << ",\n";
  json << "    \"prefetch_skip_seen\": "
       << aggregate.stats.prefetch_skip_seen << ",\n";
  json << "    \"prefetch_skip_budget\": "
       << aggregate.stats.prefetch_skip_budget << ",\n";
  json << "    \"demand_priority_blocks_prefetch\": "
       << aggregate.stats.demand_priority_blocks_prefetch << ",\n";
  json << "    \"prefetch_adaptive_pause_count\": "
       << aggregate.stats.prefetch_adaptive_pause_count << ",\n";
  json << "    \"prefetch_adaptive_final_top_k\": "
       << aggregate.stats.prefetch_adaptive_final_top_k << ",\n";
  json << "    \"prefetch_adaptive_final_lead\": "
       << aggregate.stats.prefetch_adaptive_final_lead << ",\n";
  json << "    \"prefetch_planner_cpu_us\": "
       << aggregate.stats.prefetch_planner_cpu_us << ",\n";
  json << "    \"prefetch_planner_cpu_ratio\": "
       << (aggregate.elapsed_ms == 0.0 ? 0.0
                                      : aggregate.stats.prefetch_planner_cpu_us /
                                            (aggregate.elapsed_ms * 1000.0))
       << "\n";
  json << "  },\n";
  json << "  \"warning\": ";
  if (warnings.empty()) {
    json << "null";
  } else {
    json << "\"" << json_escape(warnings.front()) << "\"";
  }
  json << ",\n";
  json << "  \"warnings\": [";
  for (std::size_t i = 0; i < warnings.size(); ++i) {
    if (i != 0) {
      json << ", ";
    }
    json << "\"" << json_escape(warnings[i]) << "\"";
  }
  json << "]\n";
  json << "}\n";
  return json.str();
}

void write_json_output(const fs::path& output_path, const std::string& json) {
  if (output_path.empty()) {
    return;
  }
  if (!output_path.parent_path().empty()) {
    fs::create_directories(output_path.parent_path());
  }
  std::ofstream output(output_path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create JSON output: " +
                             output_path.string());
  }
  output << json;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    if (args.help) {
      print_help();
      return 0;
    }
    validate_and_finalize_args(args);

    std::vector<std::string> warnings;
    if (args.beam_width_requested &&
        args.requested_beam_width > args.search_width) {
      warnings.push_back(
          "effective_beam_width=clamp(requested_beam_width, 1, search_width)");
    }
    if (args.enable_pq && args.rerank_topk == 0) {
      warnings.push_back(
          "rerank_topk=0 with PQ enabled; exact rerank is disabled");
    }
    const fs::path binary_path = fs::absolute(argv[0]);
    if (!fs::exists(args.index_path)) {
      throw std::runtime_error("Packed graph index does not exist; run agent-aware-build first: " +
                               args.index_path.string());
    }
    // 索引对象构造会把 metadata、PQ 码本与目录读进来。必须在构造之前采样，
    // 否则测到的是"加载完成后"的 100%，看不到 drop_caches 的实际效果。
    const double startup_cache_residency =
        agent_aware::page_cache_residency_ratio(args.index_path.string().c_str());
    agent_aware::PackedDiskGraphIndex index(args.index_path.string());
    if (args.manifest_path.empty()) {
      args.manifest_path = fs::path(args.index_path.string() + ".manifest.json");
    }
    const auto build_manifest =
        load_build_manifest(args.manifest_path, index.metadata());
    const auto dataset = load_input_dataset(args, index.metadata());
    if (args.num_search_threads > 1 && args.cache_pages > 0) {
      warnings.push_back(
          "num_search_threads uses one page cache per worker");
    }

    const agent_aware::PqAdcModel* pq_model_ptr = &index.pq_model();
    const auto pq_stats = load_index_pq_stats(index.pq_model());
    // --pq-subspaces/--pq-centroids 只在显式传入且与索引不一致时提示：默认值
    // 并不代表用户意图，不能对每个未传该参数的运行都报警。
    if ((args.pq_options_explicit &&
         index.metadata().pq_subspaces != args.pq_subspaces) ||
        (args.pq_centroids_explicit &&
         index.metadata().pq_centroids != args.pq_centroids)) {
      warnings.push_back(
          "--pq-subspaces/--pq-centroids only apply to agent-aware-build; "
          "search uses the PQ model stored in the index");
    }
    const auto memory_budget =
        apply_memory_budget_defaults(args, dataset, index.metadata(), pq_stats,
                                     warnings);
    // 预算里的 estimated_resident_bytes 只覆盖本进程的静态结构；而预算本身是
    // 按"数据集 × 比例"定义的进程总上限，操作系统、主索引、查询集等常驻部分
    // 必须先扣掉。预热前取一次 RSS 作为基线（此时只加载了索引和查询集）。
    const std::uint64_t baseline_rss =
        agent_aware::process_current_resident_bytes();
    if (memory_budget.budget_bytes > 0 &&
        baseline_rss + memory_budget.estimated_resident_bytes >
            memory_budget.budget_bytes) {
      warnings.push_back(
          "measured baseline RSS (" + std::to_string(baseline_rss) +
          " B) plus modelled search structures exceeds the dataset memory "
          "budget; reduce --memory_budget_ratio inputs or data scale");
    }
    configure_index_from_args(args, index);
    const auto io_status = index.io_status();
    if (!io_status.fallback_reason.empty()) {
      warnings.push_back("I/O mode fell back: " + io_status.fallback_reason);
    }

    const auto entry_seed_ids = select_search_entries(build_manifest);
    if (args.warmup_query_limit > 0) {
      (void)run_searches(args, dataset, index, pq_model_ptr,
                         entry_seed_ids, args.warmup_query_limit);
    }
    std::vector<std::vector<std::uint32_t>> detail_ids;
    std::vector<double> detail_recall;
    const auto aggregate = run_searches(args, dataset, index, pq_model_ptr,
                                        entry_seed_ids, 0, &detail_ids,
                                        &detail_recall);
    const std::string json =
        json_for_run(args, dataset, index.metadata(), detail_ids, detail_recall,
                     io_status, aggregate, binary_path,
                     pq_stats,
                     memory_budget, baseline_rss, startup_cache_residency,
                     entry_seed_ids, warnings);
    write_json_output(args.output_json, json);
    std::cout << json;

    // 内存约束是赛题硬指标。估算值只能说明"配置上没超"，真实进程峰值 RSS
    // 才算数；一旦超出就必须以非零码退出，让脚本和 CI 直接失败，而不是
    // 留一行警告让结论继续流通。
    if (args.memory_budget_enforce) {
      const std::uint64_t peak = agent_aware::process_peak_resident_bytes();
      // 进程尚未分配任何索引相关工作集之前的 RSS，是"任何配置都省不掉"的
      // 固定开销。预算低于它时，再省也过不了 —— 这属于预算设定错误
      // （数据集太小 / 比例太小），不是被实现的某一个选择超出来的。
      const std::uint64_t floor_rss =
          std::max(baseline_rss, agent_aware::process_current_resident_bytes());
      if (peak != 0 && memory_budget.budget_bytes != 0 &&
          memory_budget.budget_bytes < floor_rss) {
        std::cerr << "error: dataset memory budget "
                  << memory_budget.budget_bytes << " B (ratio "
                  << args.memory_budget_ratio
                  << ") is below the process's own fixed footprint " << floor_rss
                  << " B; no cache configuration can satisfy it — use a larger "
                     "dataset or a higher --memory-budget-ratio\n";
        return 3;
      }
      if (peak != 0 && memory_budget.budget_bytes != 0 &&
          peak > memory_budget.budget_bytes) {
        std::cerr << "error: measured peak RSS " << peak
                  << " B exceeds the dataset memory budget "
                  << memory_budget.budget_bytes << " B (ratio "
                  << args.memory_budget_ratio << "); pass "
                     "--memory-budget-enforce 0 to record a deliberately "
                     "over-budget run\n";
        return 2;
      }
    }
    // 冷读实验的硬闸门。索引在计时区间内全程驻留页缓存时，延迟里没有磁盘
    // 成分，任何 I/O 结论都不成立。这个判据以前只写进 JSON 的
    // index_fully_cached_during_run，没有任何调用方据此停下——结果就是
    // "实验无效"变成了报告里一个没人看的 true。现在让它直接失败。
    if (args.require_cold_cache) {
      constexpr double kResidentThreshold = 0.9;
      const double before = aggregate.cache_residency_before;
      const double after = aggregate.cache_residency_after;
      if (before > kResidentThreshold || after > kResidentThreshold) {
        std::cerr
            << "error: --require-cold-cache 1 but the index was resident in "
               "the page cache during the timed region (before="
            << before << ", after=" << after
            << ", threshold=" << kResidentThreshold
            << "); the latency contains no disk component and cannot support "
               "an I/O conclusion — pass --protect-hot-pages 0 (the in-degree "
               "pass reads the whole index at configure time) and evict the "
               "page cache before the run\n";
        return 4;
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
