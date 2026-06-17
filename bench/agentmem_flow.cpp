#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "agentmem/data/dataset.h"
#include "agentmem/graph/disk_graph_index.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Args {
  bool help = false;
  bool synthetic = true;
  std::string sift_dir;
  std::string base_path;
  std::string query_path;
  std::string truth_path;
  std::size_t base_limit = 1000;
  std::size_t query_limit = 100;
  std::size_t synthetic_dim = 32;
  std::size_t synthetic_clusters = 16;
  std::uint32_t seed = 42;

  fs::path index_path = "build/agentmem_flow.idx";
  fs::path output_json;
  bool rebuild_index = true;
  std::size_t graph_degree = 32;
  std::size_t page_size = 4096;
  std::string build_policy = "lsh-rp";
  std::string packing_strategy = "bfs";

  std::size_t top_k = 10;
  std::size_t search_width = 64;
  std::size_t entry_count = 32;
  bool beam_width_requested = false;
  std::size_t requested_beam_width = 0;
  std::size_t effective_beam_width = 64;

  bool enable_prefetch = false;
  bool enable_prefetch_explicit = false;
  bool prefetch_depth_requested = false;
  std::size_t requested_prefetch_depth = 0;
  std::optional<std::size_t> effective_prefetch_depth;
  std::size_t prefetch_width = 0;
  std::string prefetch_policy = "frontier-next-hop";

  std::string io_mode = "pread";
  std::size_t io_batch = 1;
  std::size_t io_depth = 1;
  std::string cache_policy = "none";
  std::size_t cache_pages = 0;
  bool protect_hot_pages = false;
  std::size_t hot_degree_threshold = 0;
};

struct SearchAggregate {
  std::size_t queries = 0;
  double elapsed_ms = 0.0;
  double recall_sum = 0.0;
  std::size_t recall_queries = 0;
  agentmem::DiskGraphSearchStats stats;
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
      << "Usage: agentmem_flow [options]\n"
      << "Dataset:\n"
      << "  --synthetic 0|1              Use synthetic data, default 1\n"
      << "  --sift-dir PATH              Use sift_base/query/groundtruth files\n"
      << "  --base PATH                  Base .fvecs path\n"
      << "  --query PATH                 Query .fvecs path\n"
      << "  --truth PATH                 Ground-truth .ivecs path\n"
      << "  --base-limit N               Loaded base vectors, default 1000\n"
      << "  --query-limit N              Loaded query vectors, default 100\n"
      << "Search:\n"
      << "  --top-k N, --k N             Result count, default 10\n"
      << "  --search-width N             Total expansion budget, default 64\n"
      << "  --entry-count N              Entry points, default 32\n"
      << "  --beam-width N               Per-round candidate expansions, >= 1\n"
      << "  --beam_width N               Compatibility alias for --beam-width\n"
      << "Prefetch/cache/I/O:\n"
      << "  --enable-prefetch 0|1        Disable/enable async prefetch\n"
      << "  --prefetch-depth N           Supported value: 1 only\n"
      << "  --prefetch-width N           Frontier/next-hop prefetch width\n"
      << "  --prefetch-policy NAME       none/frontier/next-hop/frontier-next-hop\n"
      << "  --cache-policy NAME          none/lru/2q/graph-aware-2q/agent\n"
      << "  --cache-pages N              Page cache capacity\n"
      << "  --io-mode NAME               pread, odirect, or io_uring\n"
      << "  --io-batch N                 I/O batch size\n"
      << "  --io-depth N                 I/O depth for io_uring\n"
      << "Output/build:\n"
      << "  --index-path PATH            Packed graph index path\n"
      << "  --rebuild-index 0|1          Rebuild index before search, default 1\n"
      << "  --output-json PATH           Write result JSON to path\n";
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
    } else if (name == "--top-k" || name == "--k") {
      args.top_k = parse_size_value(require_value(i, argc, argv, name),
                                    name, 1);
    } else if (name == "--search-width" || name == "--search_width") {
      args.search_width = parse_size_value(require_value(i, argc, argv, name),
                                           name, 1);
    } else if (name == "--entry-count" || name == "--entry_count") {
      args.entry_count = parse_size_value(require_value(i, argc, argv, name),
                                          name, 1);
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
    } else if (name == "--protect-hot-pages") {
      args.protect_hot_pages =
          parse_bool_value(require_value(i, argc, argv, name), name);
    } else if (name == "--hot-degree-threshold") {
      args.hot_degree_threshold =
          parse_size_value(require_value(i, argc, argv, name), name, 0);
    } else {
      throw std::runtime_error("Unknown option: " + option);
    }
  }
  return args;
}

void validate_and_finalize_args(Args& args) {
  args.effective_beam_width =
      args.beam_width_requested
          ? std::clamp(args.requested_beam_width, std::size_t{1},
                       args.search_width)
          : args.search_width;

  if (args.prefetch_depth_requested) {
    if (args.requested_prefetch_depth == 0) {
      throw std::runtime_error(
          "Invalid --prefetch-depth: use --enable-prefetch 0 to disable prefetch");
    }
    if (args.requested_prefetch_depth != 1) {
      throw std::runtime_error(
          "Unsupported --prefetch-depth: current agentmem_flow supports only prefetch_depth=1");
    }
    if (args.enable_prefetch_explicit && !args.enable_prefetch) {
      throw std::runtime_error(
          "Conflicting prefetch options: --enable-prefetch 0 with --prefetch-depth");
    }
    args.enable_prefetch = true;
    args.enable_prefetch_explicit = true;
    args.effective_prefetch_depth = 1;
  } else if (args.enable_prefetch_explicit && args.enable_prefetch) {
    args.requested_prefetch_depth = 1;
    args.prefetch_depth_requested = true;
    args.effective_prefetch_depth = 1;
  } else if (args.enable_prefetch_explicit && !args.enable_prefetch) {
    args.requested_prefetch_depth = 0;
    args.prefetch_depth_requested = true;
    args.effective_prefetch_depth.reset();
  }

  if (args.prefetch_policy != "none" && args.prefetch_policy != "frontier" &&
      args.prefetch_policy != "next-hop" &&
      args.prefetch_policy != "frontier-next-hop") {
    throw std::runtime_error(
        "Invalid --prefetch-policy: expected none, frontier, next-hop, or frontier-next-hop");
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

agentmem::LoadedDataset load_input_dataset(const Args& args) {
  agentmem::DatasetLoadConfig config;
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
  return agentmem::load_dataset(config);
}

double recall_at_k(const std::vector<agentmem::SearchResult>& result,
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

void add_stats(SearchAggregate& aggregate,
               const agentmem::DiskGraphSearchStats& stats) {
  aggregate.stats.node_reads += stats.node_reads;
  aggregate.stats.expanded += stats.expanded;
  aggregate.stats.visited += stats.visited;
  aggregate.stats.page_requests += stats.page_requests;
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
  aggregate.stats.rerank_reads += stats.rerank_reads;
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

void build_index_if_needed(const Args& args, const agentmem::VectorSet& base,
                           bool& rebuilt_index) {
  rebuilt_index = false;
  if (!args.rebuild_index && fs::exists(args.index_path)) {
    return;
  }
  if (!args.index_path.parent_path().empty()) {
    fs::create_directories(args.index_path.parent_path());
  }
  agentmem::DiskGraphBuildConfig config;
  config.degree = args.graph_degree;
  config.page_size = args.page_size;
  config.build_policy = args.build_policy;
  config.packing_strategy = args.packing_strategy;
  config.random_seed = args.seed;
  agentmem::PackedDiskGraphBuilder::build(base, args.index_path.string(),
                                          config);
  rebuilt_index = true;
}

SearchAggregate run_searches(const Args& args,
                             const agentmem::LoadedDataset& dataset,
                             agentmem::PackedDiskGraphIndex& index) {
  agentmem::DiskGraphSearchConfig search;
  search.top_k = args.top_k;
  search.search_width = args.search_width;
  search.beam_width = args.effective_beam_width;
  search.entry_count = args.entry_count;
  search.prefetch_policy = args.enable_prefetch ? args.prefetch_policy : "none";
  search.prefetch_depth = args.effective_prefetch_depth.value_or(1);
  search.prefetch_width = args.prefetch_width;

  SearchAggregate aggregate;
  aggregate.queries = dataset.queries.size();
  const auto start = Clock::now();
  for (std::size_t i = 0; i < dataset.queries.size(); ++i) {
    const auto result = index.search_one(dataset.queries.row(i), search);
    add_stats(aggregate, result.stats);
    if (dataset.truth_from_file && i < dataset.truth.size()) {
      aggregate.recall_sum += recall_at_k(result.topk, dataset.truth[i],
                                          args.top_k);
      ++aggregate.recall_queries;
    }
  }
  const auto end = Clock::now();
  aggregate.elapsed_ms =
      std::chrono::duration<double, std::milli>(end - start).count();
  return aggregate;
}

std::string json_for_run(const Args& args,
                         const agentmem::LoadedDataset& dataset,
                         const agentmem::DiskGraphIoStatus& io_status,
                         const SearchAggregate& aggregate,
                         const fs::path& binary_path, bool rebuilt_index,
                         bool runtime_prefetch_enabled,
                         const std::vector<std::string>& warnings) {
  const double elapsed_s = std::max(1e-9, aggregate.elapsed_ms / 1000.0);
  const double qps = static_cast<double>(aggregate.queries) / elapsed_s;
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
  const double cache_hit_rate =
      aggregate.stats.page_requests == 0
          ? 0.0
          : static_cast<double>(aggregate.stats.page_cache_hits) /
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
  json << "  \"top_k\": " << args.top_k << ",\n";
  json << "  \"entry_count\": " << args.entry_count << ",\n";
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
  json << "  \"recall_at_k\": " << recall << ",\n";
  json << "  \"recall_queries\": " << aggregate.recall_queries << ",\n";
  json << "  \"stats\": {\n";
  json << "    \"expanded\": " << aggregate.stats.expanded << ",\n";
  json << "    \"visited\": " << aggregate.stats.visited << ",\n";
  json << "    \"page_reads\": " << aggregate.stats.node_reads << ",\n";
  json << "    \"node_reads\": " << aggregate.stats.node_reads << ",\n";
  json << "    \"page_requests\": " << aggregate.stats.page_requests << ",\n";
  json << "    \"page_requests_before_dedup\": "
       << aggregate.stats.page_requests_before_dedup << ",\n";
  json << "    \"page_requests_after_dedup\": "
       << aggregate.stats.page_requests_after_dedup << ",\n";
  json << "    \"cache_hits\": " << aggregate.stats.page_cache_hits << ",\n";
  json << "    \"cache_hit_rate\": " << cache_hit_rate << ",\n";
  json << "    \"page_cache_hits\": " << aggregate.stats.page_cache_hits
       << ",\n";
  json << "    \"page_cache_misses\": " << aggregate.stats.page_cache_misses
       << ",\n";
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
       << aggregate.stats.prefetch_wasted_pages << "\n";
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

    const fs::path binary_path = fs::absolute(argv[0]);
    const auto dataset = load_input_dataset(args);
    bool rebuilt_index = false;
    build_index_if_needed(args, dataset.base, rebuilt_index);

    agentmem::PackedDiskGraphIndex index(args.index_path.string());
    index.configure_cache(args.cache_policy, args.cache_pages,
                          args.protect_hot_pages,
                          args.hot_degree_threshold);
    index.configure_io(args.io_mode, args.io_batch, args.io_depth);
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

    const auto aggregate = run_searches(args, dataset, index);
    const std::string json =
        json_for_run(args, dataset, io_status, aggregate, binary_path,
                     rebuilt_index, runtime_prefetch_enabled, warnings);
    write_json_output(args.output_json, json);
    std::cout << json;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
