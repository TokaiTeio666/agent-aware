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
#include <optional>
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

#include "agent_aware/data/dataset.h"
#include "agent_aware/graph/disk_graph_index.h"
#include "agent_aware/graph/entry_selector.h"

namespace {

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
  std::size_t query_limit = 100;
  std::size_t synthetic_dim = 32;
  std::size_t synthetic_clusters = 16;
  std::uint32_t seed = 42;

  fs::path index_path = "indexes/sift.idx";
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
  std::size_t rerank_topk = 100;

  std::size_t top_k = 10;
  std::size_t search_width = 350;
  std::size_t entry_count = 64;
  std::string entry_strategy = "single-medoid";
  std::size_t entry_hub_count = 15;
  std::size_t entry_cluster_count = 0;
  std::size_t entry_cluster_train_limit = 100000;
  std::size_t entry_cluster_iterations = 4;
  bool beam_width_requested = false;
  std::size_t requested_beam_width = 0;
  std::size_t effective_beam_width = 16;

  bool enable_prefetch = true;
  bool enable_prefetch_explicit = false;
  bool prefetch_depth_requested = false;
  std::size_t requested_prefetch_depth = 0;
  std::optional<std::size_t> effective_prefetch_depth = 1;
  std::size_t prefetch_width = 1;
  std::size_t prefetch_fallback_width = 0;
  std::size_t prefetch_min_candidates_per_page = 2;
  bool adaptive_prefetch = true;
  std::size_t adaptive_prefetch_window = 32;
  bool enable_progressive_frontier_prefetch = false;
  std::string prefetch_policy = "next-hop";
  bool page_dedup = true;
  bool page_coalesce = true;

  std::string io_mode = "io_uring";
  std::size_t io_batch = 16;
  std::size_t io_depth = 32;
  std::string cache_policy = "graph-aware-2q";
  std::size_t cache_pages = 0;
  bool cache_pages_requested = false;
  double memory_budget_ratio = 0.20;
  bool protect_hot_pages = true;
  std::size_t hot_degree_threshold = 0;
  std::size_t num_search_threads = 1;
};

struct MemoryBudgetReport {
  std::uint64_t dataset_bytes = 0;
  std::uint64_t budget_bytes = 0;
  std::uint64_t fixed_resident_bytes = 0;
  std::uint64_t cache_bytes = 0;
  std::uint64_t estimated_resident_bytes = 0;
  double estimated_resident_ratio = 0.0;
  bool cache_pages_auto = false;
};

struct SearchAggregate {
  std::size_t queries = 0;
  std::size_t worker_count = 1;
  double elapsed_ms = 0.0;
  std::vector<double> query_latency_ms;
  double recall_sum = 0.0;
  std::size_t recall_queries = 0;
  agent_aware::DiskGraphSearchStats stats;
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
      << "Usage: agent_aware_flow [options]\n"
      << "Dataset:\n"
      << "  --synthetic 0|1              Use synthetic data, default 0\n"
      << "  --sift-dir PATH              Use sift_base/query/groundtruth files, default data/sift\n"
      << "  --base PATH                  Base .fvecs path\n"
      << "  --query PATH                 Query .fvecs path\n"
      << "  --truth PATH                 Ground-truth .ivecs path\n"
      << "  --base-limit N               Loaded base vectors, default 1000000\n"
      << "  --query-limit N              Loaded query vectors, default 100\n"
      << "Search:\n"
      << "  --top-k N, --k N             Result count, default 10\n"
      << "  --search-width N             Total expansion budget, default 350\n"
      << "  --entry-count N              Entry points, default 64\n"
      << "  --entry-strategy NAME        single-medoid/evenly-spaced/hybrid, default single-medoid\n"
      << "  --entry-hub-count N          Hybrid hub entries, default 15\n"
      << "  --entry-cluster-count N      Hybrid cluster medoids, default fill remaining\n"
      << "  --entry-cluster-train-limit N Hybrid cluster training sample, default 100000\n"
      << "  --entry-cluster-iterations N Hybrid cluster k-means iterations, default 4\n"
      << "  --beam-width N               Per-round candidate expansions, default 16\n"
      << "  --beam_width N               Compatibility alias for --beam-width\n"
      << "  --num-search-threads N       Query workers, default 1\n"
      << "PQ/ADC:\n"
      << "  --enable-pq 0|1              Use PQ ADC candidate scoring, default 1\n"
      << "  --pq-subspaces N             PQ subspaces, default 32\n"
      << "  --pq-centroids N             PQ centroids, 2-256, default 256\n"
      << "  --pq-train-limit N           PQ training vectors, default 100000\n"
      << "  --pq-iterations N            PQ k-means iterations, default 8\n"
      << "  --rerank-topk N              Exact rerank pool after PQ, default 100\n"
      << "Prefetch/cache/I/O:\n"
      << "  --enable-prefetch 0|1        Disable/enable async prefetch\n"
      << "  --prefetch-depth N           Next-hop prefetch depth; 0 disables prefetch, default 1\n"
      << "  --prefetch-width N           Frontier/next-hop prefetch width, default 1\n"
      << "  --prefetch-fallback-width N  Frontier pages to use when next-hop has no pages, default 0\n"
      << "  --prefetch-min-candidates-per-page N Candidate page reuse threshold, default 2\n"
      << "  --page-dedup 0|1             Deduplicate prefetch pages across a query, default 1\n"
      << "  --page-coalesce 0|1          Rank candidate pages by page reuse, default 1\n"
      << "  --adaptive-prefetch 0|1      Adapt width by useful prefetch hit ratio, default 1\n"
      << "  --adaptive-prefetch-window N Query window for adaptive gate, default 32\n"
      << "  --enable-progressive-frontier-prefetch 0|1 Experimental intra-beam frontier prefetch, default 0\n"
      << "  --prefetch-policy NAME       none/frontier/next-hop/frontier-next-hop, default next-hop\n"
      << "  --cache-policy NAME          none/lru/2q/graph-aware-2q/agent, default graph-aware-2q\n"
      << "  --cache-pages N              Page cache capacity, default auto within memory budget\n"
      << "  --memory-budget-ratio R      Resident memory budget ratio, default 0.20\n"
      << "  --io-mode NAME               pread, odirect, or io_uring, default io_uring\n"
      << "  --io-batch N                 I/O batch size, default 16\n"
      << "  --io-depth N                 I/O depth for io_uring, default 32\n"
      << "Output/build:\n"
      << "  --graph-degree N             Vamana graph degree, default 32\n"
      << "  --page-size N                Packed graph page size, default 4096\n"
      << "  --build-policy NAME          Graph build policy, default vamana\n"
      << "  --packing-strategy NAME      Graph packing strategy, default bfs\n"
      << "  --index-path PATH            Packed graph index path, default indexes/sift1m_vamana_pq100_p4096_sm.idx\n"
      << "  --rebuild-index 0|1          Rebuild index before search, default 0\n"
      << "  --output-json PATH           Write result JSON to path, default logs/sift_bench/result.json\n";
}

Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    const std::string name = option.substr(0, option.find('='));
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
    } else if (name == "--query-limit") {
      args.query_limit = parse_size_value(require_value(i, argc, argv, name),
                                          name, 1);
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
    } else if (name == "--pq-centroids" || name == "--pq_centroids") {
      args.pq_centroids = parse_size_value(require_value(i, argc, argv, name),
                                           name, 2);
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
    } else if (name == "--entry-count" || name == "--entry_count") {
      args.entry_count = parse_size_value(require_value(i, argc, argv, name),
                                          name, 1);
    } else if (name == "--entry-strategy" || name == "--entry_strategy") {
      args.entry_strategy = require_value(i, argc, argv, name);
    } else if (name == "--entry-hub-count" ||
               name == "--entry_hub_count") {
      args.entry_hub_count =
          parse_size_value(require_value(i, argc, argv, name), name, 0);
    } else if (name == "--entry-cluster-count" ||
               name == "--entry_cluster_count") {
      args.entry_cluster_count =
          parse_size_value(require_value(i, argc, argv, name), name, 0);
    } else if (name == "--entry-cluster-train-limit" ||
               name == "--entry_cluster_train_limit") {
      args.entry_cluster_train_limit =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else if (name == "--entry-cluster-iterations" ||
               name == "--entry_cluster_iterations") {
      args.entry_cluster_iterations =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else if (name == "--beam-width" || name == "--beam_width") {
      args.requested_beam_width =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
      args.beam_width_requested = true;
    } else if (name == "--enable-prefetch") {
      args.enable_prefetch =
          parse_bool_value(require_value(i, argc, argv, name), name);
      args.enable_prefetch_explicit = true;
    } else if (name == "--prefetch-depth" || name == "--prefetch_depth") {
      args.requested_prefetch_depth =
          parse_size_value(require_value(i, argc, argv, name), name, 0);
      args.prefetch_depth_requested = true;
    } else if (name == "--prefetch-width" || name == "--prefetch_width") {
      args.prefetch_width = parse_size_value(require_value(i, argc, argv, name),
                                             name, 1);
    } else if (name == "--prefetch-fallback-width" ||
               name == "--prefetch_fallback_width" ||
               name == "--fallback-width" || name == "--fallback_width") {
      args.prefetch_fallback_width =
          parse_size_value(require_value(i, argc, argv, name), name, 0);
    } else if (name == "--prefetch-min-candidates-per-page" ||
               name == "--prefetch_min_candidates_per_page") {
      args.prefetch_min_candidates_per_page =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else if (name == "--page-dedup" || name == "--page_dedup") {
      args.page_dedup =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--page-coalesce" || name == "--page_coalesce") {
      args.page_coalesce =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--adaptive-prefetch" ||
               name == "--adaptive_prefetch") {
      args.adaptive_prefetch =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--adaptive-prefetch-window" ||
               name == "--adaptive_prefetch_window") {
      args.adaptive_prefetch_window =
          parse_size_value(require_value(i, argc, argv, name), name, 1);
    } else if (name == "--enable-progressive-frontier-prefetch" ||
               name == "--enable_progressive_frontier_prefetch" ||
               name == "--progressive-frontier-prefetch" ||
               name == "--progressive_frontier_prefetch") {
      args.enable_progressive_frontier_prefetch =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--prefetch-policy" || name == "--prefetch_policy") {
      args.prefetch_policy = require_value(i, argc, argv, name);
    } else if (name == "--io-mode" || name == "--io_mode") {
      args.io_mode = require_value(i, argc, argv, name);
    } else if (name == "--io-batch" || name == "--io_batch") {
      args.io_batch = parse_size_value(require_value(i, argc, argv, name),
                                       name, 1);
    } else if (name == "--io-depth" || name == "--io_depth") {
      args.io_depth = parse_size_value(require_value(i, argc, argv, name),
                                       name, 1);
    } else if (name == "--cache-policy" || name == "--cache_policy") {
      args.cache_policy = require_value(i, argc, argv, name);
    } else if (name == "--cache-pages" || name == "--cache_pages") {
      args.cache_pages = parse_size_value(require_value(i, argc, argv, name),
                                          name, 0);
      args.cache_pages_requested = true;
    } else if (name == "--memory-budget-ratio" ||
               name == "--memory_budget_ratio") {
      args.memory_budget_ratio = std::stod(require_value(i, argc, argv, name));
    } else if (name == "--protect-hot-pages") {
      args.protect_hot_pages =
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
  (void)agent_aware::parse_entry_strategy(args.entry_strategy);

  args.effective_beam_width = std::clamp(
      args.beam_width_requested ? args.requested_beam_width
                                : args.effective_beam_width,
      std::size_t{1}, args.search_width);

  if (args.prefetch_depth_requested) {
    if (args.requested_prefetch_depth == 0) {
      args.enable_prefetch = false;
      args.enable_prefetch_explicit = true;
      args.effective_prefetch_depth.reset();
    } else {
      args.enable_prefetch = true;
      args.enable_prefetch_explicit = true;
      args.effective_prefetch_depth = args.requested_prefetch_depth;
    }
  } else if (args.enable_prefetch) {
    args.effective_prefetch_depth = 1;
  } else {
    args.effective_prefetch_depth.reset();
  }

  if (args.prefetch_policy != "none" && args.prefetch_policy != "frontier" &&
      args.prefetch_policy != "next-hop" &&
      args.prefetch_policy != "frontier-next-hop") {
    throw std::runtime_error(
        "Invalid --prefetch-policy: expected none, frontier, next-hop, or frontier-next-hop");
  }

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
#ifndef _WIN32
  FILE* pipe = popen(command.c_str(), "r");
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
  (void)pclose(pipe);
#else
  (void)command;
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

agent_aware::LoadedDataset load_input_dataset(const Args& args) {
  agent_aware::DatasetLoadConfig config;
  config.synthetic = args.synthetic;
  config.base_limit = args.base_limit;
  config.query_limit = args.query_limit;
  config.paths.sift_dir = args.sift_dir;
  config.paths.base = args.base_path;
  config.paths.query = args.query_path;
  config.paths.truth = args.truth_path;
  config.synthetic_config.base_count = args.base_limit;
  config.synthetic_config.query_count = args.query_limit;
  config.synthetic_config.dim = args.synthetic_dim;
  config.synthetic_config.clusters =
      std::min(args.synthetic_clusters, args.base_limit);
  config.synthetic_config.workload = "agent";
  config.synthetic_config.seed = args.seed;
  return agent_aware::load_dataset(config);
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
  aggregate.stats.pending_hits += stats.pending_hits;
  aggregate.stats.demand_reads += stats.demand_reads;
  aggregate.stats.prefetch_reads += stats.prefetch_reads;
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
  aggregate.stats.io_prefetches += stats.io_prefetches;
  aggregate.stats.io_prefetch_hits += stats.io_prefetch_hits;
  aggregate.stats.io_prefetch_waits += stats.io_prefetch_waits;
  aggregate.stats.io_pending_pages_peak =
      std::max(aggregate.stats.io_pending_pages_peak,
               stats.io_pending_pages_peak);
  aggregate.stats.prefetch_submitted_pages += stats.prefetch_submitted_pages;
  aggregate.stats.prefetch_useful_pages += stats.prefetch_useful_pages;
  aggregate.stats.prefetch_wasted_pages += stats.prefetch_wasted_pages;
  aggregate.stats.prefetch_submitted += stats.prefetch_submitted;
  aggregate.stats.prefetch_completed += stats.prefetch_completed;
  aggregate.stats.prefetch_ready_hit += stats.prefetch_ready_hit;
  aggregate.stats.prefetch_pending_hit += stats.prefetch_pending_hit;
  aggregate.stats.prefetch_late_hit += stats.prefetch_late_hit;
  aggregate.stats.prefetch_dropped += stats.prefetch_dropped;
  aggregate.stats.prefetch_promoted += stats.prefetch_promoted;
  aggregate.stats.prefetch_cache_pollution_avoided +=
      stats.prefetch_cache_pollution_avoided;
  aggregate.stats.demand_priority_blocks_prefetch +=
      stats.demand_priority_blocks_prefetch;
  aggregate.stats.prefetch_skip_seen_before +=
      stats.prefetch_skip_seen_before;
  aggregate.stats.prefetch_skip_cached += stats.prefetch_skip_cached;
  aggregate.stats.prefetch_skip_pending += stats.prefetch_skip_pending;
  aggregate.stats.prefetch_skip_materialized +=
      stats.prefetch_skip_materialized;
  aggregate.stats.prefetch_skip_budget_full +=
      stats.prefetch_skip_budget_full;
  aggregate.stats.prefetch_skip_low_page_reuse +=
      stats.prefetch_skip_low_page_reuse;
  aggregate.stats.page_dedup_requests += stats.page_dedup_requests;
  aggregate.stats.page_dedup_hits += stats.page_dedup_hits;
  aggregate.stats.duplicate_pages_eliminated +=
      stats.duplicate_pages_eliminated;
  aggregate.stats.batch_count += stats.batch_count;
  aggregate.stats.batch_expanded += stats.batch_expanded;
  aggregate.stats.max_batch_size =
      std::max(aggregate.stats.max_batch_size, stats.max_batch_size);
  aggregate.stats.same_page_node_reuse += stats.same_page_node_reuse;
  aggregate.stats.distance_direct_calls += stats.distance_direct_calls;
  aggregate.stats.adc_table_build_us += stats.adc_table_build_us;
  aggregate.stats.rerank_reads += stats.rerank_reads;
  aggregate.stats.pq_filter_reject_count += stats.pq_filter_reject_count;
  aggregate.stats.pq_filter_accept_count += stats.pq_filter_accept_count;
  aggregate.stats.search_mutex_wait_us += stats.search_mutex_wait_us;
  aggregate.stats.p4_io.ssd_reads += stats.p4_io.ssd_reads;
  aggregate.stats.p4_io.async_submits += stats.p4_io.async_submits;
  aggregate.stats.p4_io.async_completions += stats.p4_io.async_completions;
  aggregate.stats.p4_io.prefetch_issued += stats.p4_io.prefetch_issued;
  aggregate.stats.p4_io.prefetch_used += stats.p4_io.prefetch_used;
  aggregate.stats.p4_io.prefetch_dropped += stats.p4_io.prefetch_dropped;
  aggregate.stats.p4_io.fallback_reads += stats.p4_io.fallback_reads;
  aggregate.stats.p4_io.sync_reads += stats.p4_io.sync_reads;
  aggregate.stats.p4_io.dedup_hits += stats.p4_io.dedup_hits;
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
    const Args& args, const agent_aware::LoadedDataset& dataset,
    agent_aware::PackedDiskGraphIndex& index) {
  agent_aware::EntrySelectionConfig config;
  config.strategy = agent_aware::parse_entry_strategy(args.entry_strategy);
  config.entry_count = args.entry_count;
  config.hub_count = args.entry_hub_count;
  config.cluster_count = args.entry_cluster_count;
  config.cluster_train_limit = args.entry_cluster_train_limit;
  config.cluster_iterations = args.entry_cluster_iterations;
  config.seed = args.seed;

  const std::vector<std::uint32_t>* node_in_degrees = nullptr;
  if (config.strategy == agent_aware::EntryStrategy::HybridMedoidHubsClusters) {
    node_in_degrees = &index.node_in_degrees();
  }
  return agent_aware::select_entry_points(dataset.base, config, node_in_degrees);
}

std::optional<agent_aware::PQTrainingStats> train_pq_if_enabled(
    const Args& args, const agent_aware::VectorSet& base,
    agent_aware::PqAdcModel& pq_model) {
  if (!args.enable_pq) {
    return std::nullopt;
  }
  agent_aware::PQTrainingConfig config;
  config.subspaces = args.pq_subspaces;
  config.centroids = args.pq_centroids;
  config.train_limit = args.pq_train_limit;
  config.iterations = args.pq_iterations;
  config.seed = args.seed;
  return pq_model.train(base, config);
}

MemoryBudgetReport apply_memory_budget_defaults(
    Args& args, const agent_aware::LoadedDataset& dataset,
    const std::optional<agent_aware::PQTrainingStats>& pq_stats,
    std::vector<std::string>& warnings) {
  MemoryBudgetReport report;
  report.dataset_bytes =
      static_cast<std::uint64_t>(dataset.base.size()) * dataset.base.dim *
      sizeof(float);
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
  const std::size_t directory_copies = workers == 1 ? 1 : workers + 1;
  const std::uint64_t node_directory_bytes =
      static_cast<std::uint64_t>(dataset.base.size()) *
      sizeof(std::uint32_t) * directory_copies;
  report.fixed_resident_bytes = pq_bytes + node_directory_bytes;

  if (!args.cache_pages_requested && args.cache_policy != "none") {
    report.cache_pages_auto = true;
    const std::uint64_t available =
        report.budget_bytes > report.fixed_resident_bytes
            ? report.budget_bytes - report.fixed_resident_bytes
            : 0;
    args.cache_pages =
        static_cast<std::size_t>(available / workers / args.page_size);
    if (args.cache_pages == 0 && available > 0) {
      warnings.push_back(
          "memory budget leaves less than one cache page per worker");
    }
  }

  report.cache_bytes =
      static_cast<std::uint64_t>(args.cache_pages) * args.page_size * workers;
  report.estimated_resident_bytes =
      report.fixed_resident_bytes + report.cache_bytes;
  report.estimated_resident_ratio =
      report.dataset_bytes == 0
          ? 0.0
          : static_cast<double>(report.estimated_resident_bytes) /
                static_cast<double>(report.dataset_bytes);
  if (report.estimated_resident_bytes > report.budget_bytes) {
    warnings.push_back(
        "estimated resident memory exceeds configured dataset budget");
  }
  return report;
}

void build_index_if_needed(const Args& args, const agent_aware::VectorSet& base,
                           const agent_aware::PqAdcModel* pq_model,
                           bool& rebuilt_index) {
  rebuilt_index = false;
  if (!args.rebuild_index && fs::exists(args.index_path)) {
    return;
  }
  if (!args.index_path.parent_path().empty()) {
    fs::create_directories(args.index_path.parent_path());
  }
  agent_aware::DiskGraphBuildConfig config;
  config.degree = args.graph_degree;
  config.page_size = args.page_size;
  config.build_policy = args.build_policy;
  config.packing_strategy = args.packing_strategy;
  config.random_seed = args.seed;
  config.pq_model = pq_model;
  agent_aware::PackedDiskGraphBuilder::build(base, args.index_path.string(),
                                          config);
  rebuilt_index = true;
}

agent_aware::DiskGraphSearchConfig make_search_config(
    const Args& args, const agent_aware::PqAdcModel* pq_model,
    const std::vector<std::uint32_t>& entry_seed_ids,
    std::size_t prefetch_width_override) {
  agent_aware::DiskGraphSearchConfig search;
  search.top_k = args.top_k;
  search.search_width = args.search_width;
  search.beam_width = args.effective_beam_width;
  search.entry_count = args.entry_count;
  search.seed_ids = entry_seed_ids;
  search.adc_enable = args.enable_pq && pq_model != nullptr &&
                      pq_model->enabled();
  search.pq_model = search.adc_enable ? pq_model : nullptr;
  search.rerank_topk = search.adc_enable ? args.rerank_topk : 0;
  search.prefetch_policy = args.enable_prefetch ? args.prefetch_policy : "none";
  search.prefetch_depth = args.effective_prefetch_depth.value_or(1);
  search.prefetch_width = prefetch_width_override;
  search.prefetch_fallback_width = args.prefetch_fallback_width;
  search.prefetch_min_candidates_per_page =
      args.prefetch_min_candidates_per_page;
  search.adaptive_prefetch = args.adaptive_prefetch;
  search.enable_progressive_frontier_prefetch =
      args.enable_progressive_frontier_prefetch;
  search.page_dedup = args.page_dedup;
  search.page_coalesce = args.page_coalesce;
  return search;
}

SearchAggregate run_search_worker(const Args& args,
                                  const agent_aware::LoadedDataset& dataset,
                                  agent_aware::PackedDiskGraphIndex& index,
                                  const agent_aware::PqAdcModel* pq_model,
                                  const std::vector<std::uint32_t>&
                                      entry_seed_ids,
                                  std::size_t first_query,
                                  std::size_t query_stride) {
  SearchAggregate aggregate;
  std::size_t adaptive_prefetch_width = args.prefetch_width;
  agent_aware::DiskGraphSearchStats window_stats;
  for (std::size_t i = first_query; i < dataset.queries.size();
       i += query_stride) {
    const auto search =
        make_search_config(args, pq_model, entry_seed_ids,
                           args.adaptive_prefetch
                               ? adaptive_prefetch_width
                               : args.prefetch_width);
    const auto query_start = Clock::now();
    const auto result = index.search_one(dataset.queries.row(i), search);
    const auto query_end = Clock::now();
    aggregate.query_latency_ms.push_back(
        std::chrono::duration<double, std::milli>(query_end - query_start)
            .count());
    ++aggregate.queries;
    add_stats(aggregate, result.stats);
    window_stats.prefetch_submitted += result.stats.prefetch_submitted;
    window_stats.prefetch_ready_hit += result.stats.prefetch_ready_hit;
    window_stats.prefetch_pending_hit += result.stats.prefetch_pending_hit;
    window_stats.prefetch_useful_pages += result.stats.prefetch_useful_pages;
    if (args.adaptive_prefetch &&
        aggregate.queries % args.adaptive_prefetch_window == 0) {
      if (window_stats.prefetch_submitted == 0) {
        adaptive_prefetch_width = args.prefetch_width;
        window_stats = agent_aware::DiskGraphSearchStats{};
        continue;
      }
      const double useful_hit_ratio =
          static_cast<double>(window_stats.prefetch_useful_pages) /
          static_cast<double>(window_stats.prefetch_submitted);
      if (useful_hit_ratio < 0.30) {
        adaptive_prefetch_width = 0;
      } else {
        adaptive_prefetch_width = args.prefetch_width;
      }
      window_stats = agent_aware::DiskGraphSearchStats{};
    }
    if (dataset.truth_from_file && i < dataset.truth.size()) {
      aggregate.recall_sum += recall_at_k(result.topk, dataset.truth[i],
                                          args.top_k);
      ++aggregate.recall_queries;
    }
  }
  return aggregate;
}

SearchAggregate run_searches(const Args& args,
                             const agent_aware::LoadedDataset& dataset,
                             agent_aware::PackedDiskGraphIndex& index,
                             const agent_aware::PqAdcModel* pq_model,
                             const std::vector<std::uint32_t>&
                                 entry_seed_ids) {
  const std::size_t query_count = dataset.queries.size();
  const std::size_t worker_count =
      std::min(args.num_search_threads, std::max<std::size_t>(1, query_count));

  SearchAggregate aggregate;
  aggregate.worker_count = worker_count;
  const auto start = Clock::now();

  if (worker_count == 1) {
    merge_aggregate(
        aggregate,
        run_search_worker(args, dataset, index, pq_model, entry_seed_ids, 0,
                          worker_count));
  } else {
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
              worker_count);
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
  return aggregate;
}

std::string json_for_run(const Args& args,
                         const agent_aware::LoadedDataset& dataset,
                         const agent_aware::DiskGraphIoStatus& io_status,
                         const SearchAggregate& aggregate,
                         const fs::path& binary_path, bool rebuilt_index,
                         bool runtime_prefetch_enabled,
                         const std::optional<agent_aware::PQTrainingStats>&
                             pq_stats,
                         const MemoryBudgetReport& memory_budget,
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
  const double prefetch_reads_per_query =
      static_cast<double>(aggregate.stats.prefetch_reads) /
      query_count_for_rates;
  const double ready_hit_ratio =
      static_cast<double>(aggregate.stats.prefetch_ready_hit) /
      static_cast<double>(
          std::max<std::size_t>(1, aggregate.stats.prefetch_submitted));
  const double duplicate_skipped_per_query =
      static_cast<double>(aggregate.stats.duplicate_skipped) /
      query_count_for_rates;
  const double cache_hit_rate =
      aggregate.stats.page_requests == 0
          ? 0.0
          : static_cast<double>(aggregate.stats.cache_hits) /
                static_cast<double>(aggregate.stats.page_requests);
  const std::string io_mode_summary =
      !io_status.fallback_reason.empty()
          ? "fallback"
          : (io_status.io_uring_enabled ? "io_uring" : "sync");
  const auto dirty_files =
      command_lines("git status --porcelain --untracked-files=all 2>/dev/null");
  const std::string git_commit =
      first_command_line("git rev-parse HEAD 2>/dev/null");

  std::ostringstream json;
  json << std::boolalpha;
  json << "{\n";
  json << "  \"status\": \"completed\",\n";
  json << "  \"dataset_mode\": \"" << json_escape(dataset.mode) << "\",\n";
  json << "  \"base_count\": " << dataset.base.size() << ",\n";
  json << "  \"query_count\": " << dataset.queries.size() << ",\n";
  json << "  \"dim\": " << dataset.base.dim << ",\n";
  json << "  \"base_limit\": " << args.base_limit << ",\n";
  json << "  \"query_limit\": " << args.query_limit << ",\n";
  json << "  \"graph_degree\": " << args.graph_degree << ",\n";
  json << "  \"page_size\": " << args.page_size << ",\n";
  json << "  \"build_policy\": \"" << json_escape(args.build_policy)
       << "\",\n";
  json << "  \"packing_strategy\": \""
       << json_escape(args.packing_strategy) << "\",\n";
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
  json << "  \"requested_prefetch_depth\": ";
  if (args.prefetch_depth_requested) {
    json << args.requested_prefetch_depth;
  } else {
    json << "null";
  }
  json << ",\n";
  json << "  \"effective_prefetch_depth\": ";
  if (runtime_prefetch_enabled && args.effective_prefetch_depth.has_value()) {
    json << args.effective_prefetch_depth.value();
  } else {
    json << "null";
  }
  json << ",\n";
  json << "  \"prefetch_enabled\": " << runtime_prefetch_enabled << ",\n";
  json << "  \"prefetch_policy\": \"" << json_escape(args.enable_prefetch
                                                        ? args.prefetch_policy
                                                        : "none")
       << "\",\n";
  json << "  \"prefetch_width\": " << args.prefetch_width << ",\n";
  json << "  \"prefetch_fallback_width\": "
       << args.prefetch_fallback_width << ",\n";
  json << "  \"prefetch_min_candidates_per_page\": "
       << args.prefetch_min_candidates_per_page << ",\n";
  json << "  \"page_dedup\": " << args.page_dedup << ",\n";
  json << "  \"page_coalesce\": " << args.page_coalesce << ",\n";
  json << "  \"adaptive_prefetch\": " << args.adaptive_prefetch << ",\n";
  json << "  \"adaptive_prefetch_window\": "
       << args.adaptive_prefetch_window << ",\n";
  json << "  \"enable_progressive_frontier_prefetch\": "
       << args.enable_progressive_frontier_prefetch << ",\n";
  json << "  \"io_mode\": \"" << io_mode_summary << "\",\n";
  json << "  \"io_requested_mode\": \"" << json_escape(io_status.requested_mode)
       << "\",\n";
  json << "  \"io_effective_mode\": \"" << json_escape(io_status.effective_mode)
       << "\",\n";
  json << "  \"io_uring_enabled\": " << io_status.io_uring_enabled << ",\n";
  json << "  \"odirect_enabled\": " << io_status.direct_enabled << ",\n";
  json << "  \"io_fallback_reason\": \""
       << json_escape(io_status.fallback_reason) << "\",\n";
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
  json << "  \"cache_bytes\": " << memory_budget.cache_bytes << ",\n";
  json << "  \"estimated_resident_bytes\": "
       << memory_budget.estimated_resident_bytes << ",\n";
  json << "  \"estimated_resident_ratio\": "
       << memory_budget.estimated_resident_ratio << ",\n";
  json << "  \"top_k\": " << args.top_k << ",\n";
  json << "  \"entry_count\": " << args.entry_count << ",\n";
  json << "  \"entry_strategy\": \"" << json_escape(args.entry_strategy)
       << "\",\n";
  json << "  \"entry_seed_count\": " << entry_seed_ids.size() << ",\n";
  json << "  \"entry_hub_count\": " << args.entry_hub_count << ",\n";
  json << "  \"entry_cluster_count\": " << args.entry_cluster_count
       << ",\n";
  json << "  \"entry_cluster_train_limit\": "
       << args.entry_cluster_train_limit << ",\n";
  json << "  \"entry_cluster_iterations\": "
       << args.entry_cluster_iterations << ",\n";
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
  json << "  \"pq_subspaces\": " << args.pq_subspaces << ",\n";
  json << "  \"effective_pq_subspaces\": ";
  if (pq_stats.has_value()) {
    json << pq_stats->subspaces;
  } else {
    json << "null";
  }
  json << ",\n";
  json << "  \"pq_centroids\": " << args.pq_centroids << ",\n";
  json << "  \"pq_train_limit\": " << args.pq_train_limit << ",\n";
  json << "  \"pq_iterations\": " << args.pq_iterations << ",\n";
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
  json << "  \"rebuilt_index\": " << rebuilt_index << ",\n";
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
  json << "  \"reads_per_query\": " << reads_per_query << ",\n";
  json << "  \"demand_reads_per_query\": " << demand_reads_per_query << ",\n";
  json << "  \"prefetch_reads_per_query\": " << prefetch_reads_per_query
       << ",\n";
  json << "  \"ready_hit_ratio\": " << ready_hit_ratio << ",\n";
  json << "  \"duplicate_skipped_per_query\": "
       << duplicate_skipped_per_query << ",\n";
  json << "  \"cache_hit_rate\": " << cache_hit_rate << ",\n";
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
  json << "  \"stats\": {\n";
  json << "    \"expanded\": " << aggregate.stats.expanded << ",\n";
  json << "    \"visited\": " << aggregate.stats.visited << ",\n";
  json << "    \"page_reads\": " << aggregate.stats.submitted_reads << ",\n";
  json << "    \"node_reads\": " << aggregate.stats.node_reads << ",\n";
  json << "    \"page_requests\": " << aggregate.stats.page_requests << ",\n";
  json << "    \"cache_hits\": " << aggregate.stats.cache_hits << ",\n";
  json << "    \"pinned_hits\": " << aggregate.stats.pinned_hits << ",\n";
  json << "    \"pending_hits\": " << aggregate.stats.pending_hits << ",\n";
  json << "    \"demand_reads\": " << aggregate.stats.demand_reads << ",\n";
  json << "    \"prefetch_reads\": " << aggregate.stats.prefetch_reads
       << ",\n";
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
  json << "    \"io_submits\": " << aggregate.stats.io_submits << ",\n";
  json << "    \"io_completions\": " << aggregate.stats.io_completions
       << ",\n";
  json << "    \"io_submit_syscalls\": "
       << aggregate.stats.io_submit_syscalls << ",\n";
  json << "    \"uring_submit_count\": "
       << aggregate.stats.uring_submit_count << ",\n";
  json << "    \"uring_cqe_count\": " << aggregate.stats.uring_cqe_count
       << ",\n";
  json << "    \"io_prefetches\": " << aggregate.stats.io_prefetches
       << ",\n";
  json << "    \"io_prefetch_hits\": " << aggregate.stats.io_prefetch_hits
       << ",\n";
  json << "    \"io_prefetch_waits\": " << aggregate.stats.io_prefetch_waits
       << ",\n";
  json << "    \"io_pending_pages_peak\": "
       << aggregate.stats.io_pending_pages_peak << ",\n";
  json << "    \"batch_count\": " << aggregate.stats.batch_count << ",\n";
  json << "    \"avg_batch_size\": " << avg_batch_size << ",\n";
  json << "    \"max_batch_size\": " << aggregate.stats.max_batch_size
       << ",\n";
  json << "    \"page_dedup_requests\": "
       << aggregate.stats.page_dedup_requests << ",\n";
  json << "    \"page_dedup_hits\": " << aggregate.stats.page_dedup_hits
       << ",\n";
  json << "    \"duplicate_pages_eliminated\": "
       << aggregate.stats.duplicate_pages_eliminated << ",\n";
  json << "    \"prefetch_submitted_pages\": "
       << aggregate.stats.prefetch_submitted_pages << ",\n";
  json << "    \"prefetch_useful_pages\": "
       << aggregate.stats.prefetch_useful_pages << ",\n";
  json << "    \"prefetch_wasted_pages\": "
       << aggregate.stats.prefetch_wasted_pages << ",\n";
  json << "    \"prefetch_submitted\": "
       << aggregate.stats.prefetch_submitted << ",\n";
  json << "    \"prefetch_completed\": "
       << aggregate.stats.prefetch_completed << ",\n";
  json << "    \"prefetch_ready_hit\": "
       << aggregate.stats.prefetch_ready_hit << ",\n";
  json << "    \"prefetch_pending_hit\": "
       << aggregate.stats.prefetch_pending_hit << ",\n";
  json << "    \"prefetch_late_hit\": "
       << aggregate.stats.prefetch_late_hit << ",\n";
  json << "    \"prefetch_dropped\": "
       << aggregate.stats.prefetch_dropped << ",\n";
  json << "    \"prefetch_promoted\": "
       << aggregate.stats.prefetch_promoted << ",\n";
  json << "    \"prefetch_cache_pollution_avoided\": "
       << aggregate.stats.prefetch_cache_pollution_avoided << ",\n";
  json << "    \"demand_priority_blocks_prefetch\": "
       << aggregate.stats.demand_priority_blocks_prefetch << ",\n";
  json << "    \"ready_hit_ratio\": " << ready_hit_ratio << ",\n";
  json << "    \"prefetch_skip_seen_before\": "
       << aggregate.stats.prefetch_skip_seen_before << ",\n";
  json << "    \"prefetch_skip_cached\": "
       << aggregate.stats.prefetch_skip_cached << ",\n";
  json << "    \"prefetch_skip_pending\": "
       << aggregate.stats.prefetch_skip_pending << ",\n";
  json << "    \"prefetch_skip_materialized\": "
       << aggregate.stats.prefetch_skip_materialized << ",\n";
  json << "    \"prefetch_skip_budget_full\": "
       << aggregate.stats.prefetch_skip_budget_full << ",\n";
  json << "    \"prefetch_skip_low_page_reuse\": "
       << aggregate.stats.prefetch_skip_low_page_reuse << ",\n";
  json << "    \"adc_table_build_us\": "
       << aggregate.stats.adc_table_build_us << ",\n";
  json << "    \"rerank_reads\": " << aggregate.stats.rerank_reads << ",\n";
  json << "    \"pq_filter_accept_count\": "
       << aggregate.stats.pq_filter_accept_count << ",\n";
  json << "    \"pq_filter_reject_count\": "
       << aggregate.stats.pq_filter_reject_count << ",\n";
  json << "    \"distance_direct_calls\": "
       << aggregate.stats.distance_direct_calls << "\n";
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
    if (!args.enable_pq && args.rerank_topk > 0) {
      warnings.push_back("rerank_topk was requested but PQ is disabled");
    }
    if (args.enable_pq && args.rerank_topk == 0) {
      warnings.push_back(
          "rerank_topk=0 with PQ enabled; exact rerank is disabled");
    }
    const fs::path binary_path = fs::absolute(argv[0]);
    const auto dataset = load_input_dataset(args);
    if (args.enable_pq && args.pq_subspaces > dataset.base.dim) {
      warnings.push_back(
          "effective_pq_subspaces=clamp(requested_pq_subspaces, 1, dim)");
    }

    agent_aware::PqAdcModel pq_model;
    const auto pq_stats =
        train_pq_if_enabled(args, dataset.base, pq_model);
    const agent_aware::PqAdcModel* pq_model_ptr =
        pq_stats.has_value() ? &pq_model : nullptr;
    const auto memory_budget =
        apply_memory_budget_defaults(args, dataset, pq_stats, warnings);
    if (args.num_search_threads > 1 && args.cache_pages > 0) {
      warnings.push_back(
          "num_search_threads uses one page cache per worker");
    }

    bool rebuilt_index = false;
    build_index_if_needed(args, dataset.base, pq_model_ptr, rebuilt_index);

    agent_aware::PackedDiskGraphIndex index(args.index_path.string());
    configure_index_from_args(args, index);
    const auto io_status = index.io_status();
    const bool runtime_prefetch_enabled =
        args.enable_prefetch && io_status.io_uring_enabled &&
        args.prefetch_policy != "none";
    if (args.enable_prefetch && !runtime_prefetch_enabled) {
      warnings.push_back(
          "prefetch was requested but not enabled by the effective I/O mode");
    }
    if (!io_status.fallback_reason.empty()) {
      warnings.push_back("I/O mode fell back: " + io_status.fallback_reason);
    }

    const auto entry_seed_ids = select_search_entries(args, dataset, index);
    const auto aggregate =
        run_searches(args, dataset, index, pq_model_ptr, entry_seed_ids);
    const std::string json =
        json_for_run(args, dataset, io_status, aggregate, binary_path,
                     rebuilt_index, runtime_prefetch_enabled, pq_stats,
                     memory_budget, entry_seed_ids, warnings);
    write_json_output(args.output_json, json);
    std::cout << json;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
