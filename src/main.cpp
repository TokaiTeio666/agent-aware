#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agentmem/brute_force.h"
#include "agentmem/dataset.h"
#include "agentmem/dynamic_index.h"
#include "agentmem/graph_index.h"
#include "agentmem/metrics.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

struct Args {
  std::string engine = "exact";
  std::string base_path;
  std::string query_path;
  std::string truth_path;
  std::string index_path = "build/v1_graph.idx";
  std::string layout = "one-node";
  std::string packing_strategy = "coaccess";
  std::string run_type = "smoke";
  std::string cache_policy = "none";
  std::string path_cache_policy = "none";
  std::string workload_mode = "read-only";
  std::string wal_path = "build/v5_delta.wal";
  std::string compaction_policy = "none";
  std::string compaction_io_mode = "time";
  std::string compaction_io_path = "build/v6_compaction_io.bin";
  std::size_t base_limit = 0;
  std::size_t query_limit = 0;
  std::size_t k = 10;
  std::size_t graph_degree = 16;
  std::size_t page_size = 4096;
  std::size_t search_width = 64;
  std::size_t entry_count = 32;
  std::size_t routing_sample_count = 256;
  std::size_t warmup_runs = 0;
  std::size_t cache_pages = 0;
  std::size_t path_cache_capacity = 0;
  std::size_t path_cache_hit_search_width = 32;
  std::size_t operation_count = 0;
  std::size_t write_ratio_percent = 0;
  std::size_t delta_compaction_threshold = 64;
  std::size_t compaction_batch_size = 16;
  std::size_t compaction_work_us = 0;
  std::size_t compaction_io_bytes_per_vector = 0;
  std::size_t coaccess_sessions = 64;
  std::size_t coaccess_trace_length = 32;
  double sla_p99_ms = 1.0;
  bool synthetic = true;
  bool build_index = false;
  agentmem::SyntheticConfig synthetic_config;
};

struct RunOutput {
  std::vector<std::vector<agentmem::SearchResult>> results;
  std::vector<double> latencies_ms;
  std::vector<double> ssd_reads;
  std::vector<double> cache_requests;
  std::vector<double> cache_hits;
  std::vector<double> cache_misses;
  std::vector<double> path_cache_requests;
  std::vector<double> path_cache_hits;
  std::vector<double> expanded;
  std::vector<double> visited;
  std::vector<double> insert_latencies_ms;
  std::vector<double> query_compaction_ms;
  std::vector<std::vector<std::uint32_t>> dynamic_truth;
  std::size_t operation_count = 0;
  std::size_t insert_count = 0;
  std::size_t compaction_ops = 0;
  std::size_t compaction_vectors = 0;
  std::size_t compaction_skipped_sla = 0;
  std::size_t queries_with_compaction = 0;
  std::size_t compaction_io_bytes = 0;
  std::size_t delta_active_size = 0;
  std::size_t delta_sealed_size = 0;
  std::size_t wal_records = 0;
  std::size_t wal_bytes = 0;
  double compaction_seconds = 0.0;
  double elapsed_seconds = 0.0;
};

void print_usage(const char* program) {
  std::cout
      << "AgentMem-Flow: vector retrieval baseline and SSD graph baseline\n\n"
      << "Usage:\n"
      << "  " << program << " --engine exact --synthetic [--base-count N]\n"
      << "              [--query-count N] [--dim D] [--clusters C] [--k K]\n"
      << "  " << program << " --engine graph --synthetic --build-index\n"
      << "              [--index path/v1_graph.idx] [--graph-degree R]\n"
      << "              [--layout one-node|packed]\n"
      << "              [--packing random|bfs|coaccess]\n"
      << "              [--search-width W] [--entry-count E]\n"
      << "              [--routing-sample-count S]\n"
      << "  " << program << " --engine exact|graph --base path/base.fvecs\n"
      << "              --query path/query.fvecs [--truth path/groundtruth.ivecs]\n\n"
      << "Options:\n"
      << "  --engine NAME        exact or graph, default exact\n"
      << "  --base PATH          SIFT fvecs base vectors\n"
      << "  --query PATH         SIFT fvecs query vectors\n"
      << "  --truth PATH         SIFT ivecs ground-truth ids\n"
      << "  --base-limit N       Load at most N base vectors\n"
      << "  --query-limit N      Load at most N query vectors\n"
      << "  --synthetic          Generate clustered synthetic data (default)\n"
      << "  --base-count N       Synthetic base vector count, default 10000\n"
      << "  --query-count N      Synthetic query count, default 1000\n"
      << "  --dim D              Synthetic dimension, default 128\n"
      << "  --clusters C         Synthetic cluster count, default 64\n"
      << "  --synthetic-workload NAME random or agent, default random\n"
      << "  --session-length N   Agent workload consecutive queries/session\n"
      << "  --seed N             Synthetic random seed, default 42\n"
      << "  --k K                Top-K, default 10\n"
      << "  --index PATH         Graph index path, default build/v1_graph.idx\n"
      << "  --build-index        Rebuild graph index before graph search\n"
      << "  --layout NAME        one-node or packed, default one-node\n"
      << "  --packing NAME       packed layout strategy: random, bfs, coaccess\n"
      << "  --run-type NAME      smoke, cold, or warm, default smoke\n"
      << "  --warmup-runs N      Unmeasured warmup passes before measurement\n"
      << "  --cache-policy NAME  none, lru, or agent, default none\n"
      << "  --cache-pages N      Global packed page cache capacity, default 0\n"
      << "  --path-cache-policy NAME  none or reuse, default none\n"
      << "  --path-cache-capacity N   Query path cache entries, default 0\n"
      << "  --path-cache-hit-search-width N  Search width on path hit\n"
      << "  --workload-mode NAME read-only or mixed, default read-only\n"
      << "  --operation-count N  Mixed workload operations, default query count\n"
      << "  --write-ratio N      Mixed workload insert percentage, default 0\n"
      << "  --wal PATH           Delta insert WAL path, default build/v5_delta.wal\n"
      << "  --compaction-policy NAME none, aggressive, or sla, default none\n"
      << "  --delta-compaction-threshold N  Active delta size to compact\n"
      << "  --compaction-batch-size N       Vectors moved per compaction tick\n"
      << "  --compaction-work-us N          Simulated compaction I/O time/tick\n"
      << "  --compaction-io-mode NAME       time or file, default time\n"
      << "  --compaction-io-path PATH       File path for V6 file I/O compaction\n"
      << "  --compaction-io-bytes-per-vector N  Bytes written per compacted vector\n"
      << "  --sla-p99-ms X       P99 budget for SLA compaction, default 1.0\n"
      << "  --graph-degree R     Exact kNN graph out-degree, default 16\n"
      << "  --page-size BYTES    Fixed node page size, default 4096\n"
      << "  --search-width W     Max graph node expansions/query, default 64\n"
      << "  --entry-count E      Evenly spaced graph entry points, default 32\n"
      << "  --routing-sample-count S  Resident sampled vectors for seed routing,\n"
      << "                            default 256, use 0 to disable\n"
      << "  --coaccess-sessions N      Agent-style trace sessions, default 64\n"
      << "  --coaccess-trace-length N  Agent-style trace length, default 32\n"
      << "  --help               Show this help\n";
}

std::size_t parse_size(const std::string& value, const std::string& name) {
  std::size_t parsed = 0;
  try {
    parsed = static_cast<std::size_t>(std::stoull(value));
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid numeric value for " + name + ": " + value);
  }
  return parsed;
}

double parse_double(const std::string& value, const std::string& name) {
  double parsed = 0.0;
  try {
    parsed = std::stod(value);
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid numeric value for " + name + ": " + value);
  }
  return parsed;
}

Args parse_args(int argc, char** argv) {
  Args args;

  for (int i = 1; i < argc; ++i) {
    const std::string opt = argv[i];
    auto require_value = [&](const std::string& name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error("Missing value for " + name);
      }
      return argv[++i];
    };

    if (opt == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (opt == "--engine") {
      args.engine = require_value(opt);
    } else if (opt == "--base") {
      args.base_path = require_value(opt);
      args.synthetic = false;
    } else if (opt == "--query") {
      args.query_path = require_value(opt);
      args.synthetic = false;
    } else if (opt == "--truth") {
      args.truth_path = require_value(opt);
    } else if (opt == "--base-limit") {
      args.base_limit = parse_size(require_value(opt), opt);
    } else if (opt == "--query-limit") {
      args.query_limit = parse_size(require_value(opt), opt);
    } else if (opt == "--synthetic") {
      args.synthetic = true;
    } else if (opt == "--base-count") {
      args.synthetic_config.base_count = parse_size(require_value(opt), opt);
    } else if (opt == "--query-count") {
      args.synthetic_config.query_count = parse_size(require_value(opt), opt);
    } else if (opt == "--dim") {
      args.synthetic_config.dim = parse_size(require_value(opt), opt);
    } else if (opt == "--clusters") {
      args.synthetic_config.clusters = parse_size(require_value(opt), opt);
    } else if (opt == "--synthetic-workload") {
      args.synthetic_config.workload = require_value(opt);
    } else if (opt == "--session-length") {
      args.synthetic_config.session_length = parse_size(require_value(opt), opt);
    } else if (opt == "--seed") {
      args.synthetic_config.seed =
          static_cast<std::uint32_t>(parse_size(require_value(opt), opt));
    } else if (opt == "--k") {
      args.k = parse_size(require_value(opt), opt);
    } else if (opt == "--index") {
      args.index_path = require_value(opt);
    } else if (opt == "--build-index") {
      args.build_index = true;
    } else if (opt == "--layout") {
      args.layout = require_value(opt);
    } else if (opt == "--packing") {
      args.packing_strategy = require_value(opt);
    } else if (opt == "--run-type") {
      args.run_type = require_value(opt);
    } else if (opt == "--warmup-runs") {
      args.warmup_runs = parse_size(require_value(opt), opt);
    } else if (opt == "--cache-policy") {
      args.cache_policy = require_value(opt);
    } else if (opt == "--cache-pages") {
      args.cache_pages = parse_size(require_value(opt), opt);
    } else if (opt == "--path-cache-policy") {
      args.path_cache_policy = require_value(opt);
    } else if (opt == "--path-cache-capacity") {
      args.path_cache_capacity = parse_size(require_value(opt), opt);
    } else if (opt == "--path-cache-hit-search-width") {
      args.path_cache_hit_search_width = parse_size(require_value(opt), opt);
    } else if (opt == "--workload-mode") {
      args.workload_mode = require_value(opt);
    } else if (opt == "--operation-count") {
      args.operation_count = parse_size(require_value(opt), opt);
    } else if (opt == "--write-ratio") {
      args.write_ratio_percent = parse_size(require_value(opt), opt);
    } else if (opt == "--wal") {
      args.wal_path = require_value(opt);
    } else if (opt == "--compaction-policy") {
      args.compaction_policy = require_value(opt);
    } else if (opt == "--delta-compaction-threshold") {
      args.delta_compaction_threshold = parse_size(require_value(opt), opt);
    } else if (opt == "--compaction-batch-size") {
      args.compaction_batch_size = parse_size(require_value(opt), opt);
    } else if (opt == "--compaction-work-us") {
      args.compaction_work_us = parse_size(require_value(opt), opt);
    } else if (opt == "--compaction-io-mode") {
      args.compaction_io_mode = require_value(opt);
    } else if (opt == "--compaction-io-path") {
      args.compaction_io_path = require_value(opt);
    } else if (opt == "--compaction-io-bytes-per-vector") {
      args.compaction_io_bytes_per_vector = parse_size(require_value(opt), opt);
    } else if (opt == "--sla-p99-ms") {
      args.sla_p99_ms = parse_double(require_value(opt), opt);
    } else if (opt == "--graph-degree") {
      args.graph_degree = parse_size(require_value(opt), opt);
    } else if (opt == "--page-size") {
      args.page_size = parse_size(require_value(opt), opt);
    } else if (opt == "--search-width") {
      args.search_width = parse_size(require_value(opt), opt);
    } else if (opt == "--entry-count") {
      args.entry_count = parse_size(require_value(opt), opt);
    } else if (opt == "--routing-sample-count") {
      args.routing_sample_count = parse_size(require_value(opt), opt);
    } else if (opt == "--coaccess-sessions") {
      args.coaccess_sessions = parse_size(require_value(opt), opt);
    } else if (opt == "--coaccess-trace-length") {
      args.coaccess_trace_length = parse_size(require_value(opt), opt);
    } else {
      throw std::runtime_error("Unknown option: " + opt);
    }
  }

  if (args.engine != "exact" && args.engine != "graph") {
    throw std::runtime_error("--engine must be exact or graph");
  }
  if (args.layout != "one-node" && args.layout != "packed") {
    throw std::runtime_error("--layout must be one-node or packed");
  }
  if (args.packing_strategy != "random" && args.packing_strategy != "bfs" &&
      args.packing_strategy != "coaccess") {
    throw std::runtime_error("--packing must be random, bfs, or coaccess");
  }
  if (args.run_type != "smoke" && args.run_type != "cold" &&
      args.run_type != "warm") {
    throw std::runtime_error("--run-type must be smoke, cold, or warm");
  }
  if (args.cache_policy != "none" && args.cache_policy != "lru" &&
      args.cache_policy != "agent") {
    throw std::runtime_error("--cache-policy must be none, lru, or agent");
  }
  if (args.path_cache_policy != "none" && args.path_cache_policy != "reuse") {
    throw std::runtime_error("--path-cache-policy must be none or reuse");
  }
  if (args.workload_mode != "read-only" && args.workload_mode != "mixed") {
    throw std::runtime_error("--workload-mode must be read-only or mixed");
  }
  if (args.workload_mode == "mixed" && args.engine != "graph") {
    throw std::runtime_error("--workload-mode mixed currently requires --engine graph");
  }
  if (args.write_ratio_percent > 100) {
    throw std::runtime_error("--write-ratio must be between 0 and 100");
  }
  if (args.compaction_policy != "none" &&
      args.compaction_policy != "aggressive" &&
      args.compaction_policy != "sla") {
    throw std::runtime_error(
        "--compaction-policy must be none, aggressive, or sla");
  }
  if (args.compaction_batch_size == 0) {
    throw std::runtime_error("--compaction-batch-size must be positive");
  }
  if (args.compaction_io_mode != "time" && args.compaction_io_mode != "file") {
    throw std::runtime_error("--compaction-io-mode must be time or file");
  }
  if (args.sla_p99_ms <= 0.0) {
    throw std::runtime_error("--sla-p99-ms must be positive");
  }
  if (!args.synthetic && (args.base_path.empty() || args.query_path.empty())) {
    throw std::runtime_error("--base and --query are required for fvecs mode");
  }
  if (args.k == 0) {
    throw std::runtime_error("--k must be positive");
  }

  return args;
}

std::vector<std::vector<std::uint32_t>> results_as_truth(
    const std::vector<std::vector<agentmem::SearchResult>>& results) {
  std::vector<std::vector<std::uint32_t>> truth;
  truth.reserve(results.size());
  for (const auto& row : results) {
    std::vector<std::uint32_t> ids;
    ids.reserve(row.size());
    for (const auto& item : row) {
      ids.push_back(item.id);
    }
    truth.push_back(std::move(ids));
  }
  return truth;
}

std::vector<std::vector<std::uint32_t>> exact_truth(
    const agentmem::VectorSet& base, const agentmem::VectorSet& queries,
    std::size_t k) {
  const agentmem::BruteForceIndex exact(base);
  return results_as_truth(exact.search_batch(queries, k));
}

RunOutput run_exact(const agentmem::VectorSet& base,
                    const agentmem::VectorSet& queries, std::size_t k) {
  const agentmem::BruteForceIndex index(base);
  RunOutput output;
  output.results.reserve(queries.size());
  output.latencies_ms.reserve(queries.size());

  const auto batch_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < queries.size(); ++i) {
    const auto query_start = std::chrono::steady_clock::now();
    output.results.push_back(index.search_one(queries.row(i), k));
    const auto query_end = std::chrono::steady_clock::now();
    output.latencies_ms.push_back(
        std::chrono::duration<double, std::milli>(query_end - query_start)
            .count());
  }
  const auto batch_end = std::chrono::steady_clock::now();
  output.elapsed_seconds =
      std::chrono::duration<double>(batch_end - batch_start).count();
  return output;
}

struct RoutedEntries {
  std::uint32_t signature = 0;
  std::vector<std::uint32_t> seeds;
};

std::vector<std::uint32_t> merge_ids(const std::vector<std::uint32_t>& lhs,
                                     const std::vector<std::uint32_t>& rhs,
                                     std::size_t limit) {
  std::vector<std::uint32_t> merged;
  std::unordered_set<std::uint32_t> seen;
  merged.reserve(std::min(limit, lhs.size() + rhs.size()));
  auto append = [&](const std::vector<std::uint32_t>& ids) {
    for (const auto id : ids) {
      if (merged.size() >= limit) {
        return;
      }
      if (seen.insert(id).second) {
        merged.push_back(id);
      }
    }
  };
  append(lhs);
  append(rhs);
  return merged;
}

std::vector<std::uint32_t> topk_ids(
    const std::vector<agentmem::SearchResult>& results) {
  std::vector<std::uint32_t> ids;
  ids.reserve(results.size());
  for (const auto& result : results) {
    ids.push_back(result.id);
  }
  return ids;
}

struct PathCacheEntry {
  std::uint32_t signature = 0;
  std::uint64_t last_access = 0;
  std::uint64_t frequency = 0;
  std::vector<std::uint32_t> seeds;
  std::vector<std::uint32_t> top_ids;
};

class QueryPathCache {
 public:
  QueryPathCache(std::string policy, std::size_t capacity)
      : policy_(std::move(policy)), capacity_(capacity) {}

  bool enabled() const {
    return policy_ == "reuse" && capacity_ > 0;
  }

  const PathCacheEntry* lookup(std::uint32_t signature) {
    if (!enabled()) {
      return nullptr;
    }
    auto found = entries_.find(signature);
    if (found == entries_.end()) {
      return nullptr;
    }
    found->second.last_access = ++clock_;
    ++found->second.frequency;
    return &found->second;
  }

  void update(std::uint32_t signature, const std::vector<std::uint32_t>& seeds,
              const std::vector<agentmem::SearchResult>& results) {
    if (!enabled()) {
      return;
    }
    auto found = entries_.find(signature);
    if (found == entries_.end()) {
      if (entries_.size() >= capacity_) {
        evict_one();
      }
      PathCacheEntry entry;
      entry.signature = signature;
      entry.last_access = ++clock_;
      entry.frequency = 1;
      entry.seeds = seeds;
      entry.top_ids = topk_ids(results);
      entries_.emplace(signature, std::move(entry));
      return;
    }

    found->second.last_access = ++clock_;
    ++found->second.frequency;
    found->second.seeds = seeds;
    found->second.top_ids = topk_ids(results);
  }

 private:
  void evict_one() {
    auto victim = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->second.frequency < victim->second.frequency ||
          (it->second.frequency == victim->second.frequency &&
           it->second.last_access < victim->second.last_access)) {
        victim = it;
      }
    }
    entries_.erase(victim);
  }

  std::string policy_;
  std::size_t capacity_ = 0;
  std::uint64_t clock_ = 0;
  std::unordered_map<std::uint32_t, PathCacheEntry> entries_;
};

RoutedEntries select_routed_entries_with_signature(
    const agentmem::VectorSet& base, const float* query, std::size_t entry_count,
    std::size_t routing_sample_count) {
  RoutedEntries routed;
  if (entry_count == 0 || routing_sample_count == 0 || base.empty()) {
    return routed;
  }

  const std::size_t sample_count = std::min(routing_sample_count, base.size());
  std::vector<agentmem::SearchResult> scored;
  scored.reserve(sample_count);

  for (std::size_t i = 0; i < sample_count; ++i) {
    std::size_t id = sample_count == 1 ? 0 : (i * (base.size() - 1)) / (sample_count - 1);
    if (!scored.empty() && scored.back().id == id) {
      continue;
    }
    scored.push_back(agentmem::SearchResult{
        static_cast<std::uint32_t>(id),
        agentmem::squared_l2(query, base.row(id), base.dim)});
  }

  std::sort(scored.begin(), scored.end(),
            [](const agentmem::SearchResult& lhs,
               const agentmem::SearchResult& rhs) {
              if (lhs.distance == rhs.distance) {
                return lhs.id < rhs.id;
              }
              return lhs.distance < rhs.distance;
            });

  if (!scored.empty()) {
    routed.signature = scored.front().id;
  }
  const std::size_t actual = std::min(entry_count, scored.size());
  routed.seeds.reserve(actual);
  for (std::size_t i = 0; i < actual; ++i) {
    routed.seeds.push_back(scored[i].id);
  }
  return routed;
}

template <typename Index>
agentmem::DiskGraphSearchResult search_graph_one_with_routing(
    Index& index, const Args& args, const agentmem::VectorSet& base,
    const float* query, QueryPathCache& path_cache, bool& path_hit) {
  agentmem::DiskGraphSearchConfig per_query_config;
  per_query_config.top_k = args.k;
  per_query_config.search_width = args.search_width;
  per_query_config.entry_count = args.entry_count;

  const RoutedEntries routed =
      select_routed_entries_with_signature(base, query, args.entry_count,
                                           args.routing_sample_count);
  per_query_config.seed_ids = routed.seeds;
  path_hit = false;
  const PathCacheEntry* cached = path_cache.lookup(routed.signature);
  if (cached != nullptr) {
    path_hit = true;
    per_query_config.seed_ids =
        merge_ids(cached->top_ids, cached->seeds, args.entry_count);
    per_query_config.seed_ids =
        merge_ids(per_query_config.seed_ids, routed.seeds, args.entry_count);
    per_query_config.search_width =
        std::min(args.search_width, args.path_cache_hit_search_width);
  }

  auto result = index.search_one(query, per_query_config);
  path_cache.update(routed.signature, routed.seeds, result.topk);
  return result;
}

template <typename Index>
RunOutput execute_graph_queries(Index& index, const Args& args,
                                const agentmem::VectorSet& base,
                                const agentmem::VectorSet& queries,
                                bool collect_output,
                                QueryPathCache& path_cache) {
  RunOutput output;
  if (collect_output) {
    output.results.reserve(queries.size());
    output.latencies_ms.reserve(queries.size());
    output.ssd_reads.reserve(queries.size());
    output.cache_requests.reserve(queries.size());
    output.cache_hits.reserve(queries.size());
    output.cache_misses.reserve(queries.size());
    output.path_cache_requests.reserve(queries.size());
    output.path_cache_hits.reserve(queries.size());
    output.expanded.reserve(queries.size());
    output.visited.reserve(queries.size());
  }

  const auto batch_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < queries.size(); ++i) {
    bool path_hit = false;
    const auto query_start = std::chrono::steady_clock::now();
    auto result = search_graph_one_with_routing(index, args, base,
                                                queries.row(i), path_cache,
                                                path_hit);
    const auto query_end = std::chrono::steady_clock::now();

    if (collect_output) {
      output.latencies_ms.push_back(
          std::chrono::duration<double, std::milli>(query_end - query_start)
              .count());
      output.ssd_reads.push_back(static_cast<double>(result.stats.node_reads));
      output.cache_requests.push_back(
          static_cast<double>(result.stats.page_requests));
      output.cache_hits.push_back(
          static_cast<double>(result.stats.page_cache_hits));
      output.cache_misses.push_back(
          static_cast<double>(result.stats.page_cache_misses));
      output.path_cache_requests.push_back(path_cache.enabled() ? 1.0 : 0.0);
      output.path_cache_hits.push_back(path_hit ? 1.0 : 0.0);
      output.expanded.push_back(static_cast<double>(result.stats.expanded));
      output.visited.push_back(static_cast<double>(result.stats.visited));
      output.results.push_back(std::move(result.topk));
    }
  }
  const auto batch_end = std::chrono::steady_clock::now();
  output.elapsed_seconds =
      std::chrono::duration<double>(batch_end - batch_start).count();
  return output;
}

std::vector<std::uint32_t> ids_from_results(
    const std::vector<agentmem::SearchResult>& results) {
  std::vector<std::uint32_t> ids;
  ids.reserve(results.size());
  for (const auto& result : results) {
    ids.push_back(result.id);
  }
  return ids;
}

std::vector<float> make_insert_vector(const agentmem::VectorSet& queries,
                                      std::size_t query_index,
                                      std::size_t insert_index,
                                      std::uint32_t seed) {
  std::vector<float> vector(queries.dim, 0.0f);
  const float* source = queries.row(query_index % queries.size());
  for (std::size_t d = 0; d < queries.dim; ++d) {
    const int bucket =
        static_cast<int>((insert_index * 31 + d * 17 + seed) % 23) - 11;
    vector[d] = source[d] + static_cast<float>(bucket) * 0.0025f;
  }
  return vector;
}

double recent_p99_ms(const std::vector<double>& latencies,
                     std::size_t window) {
  if (latencies.empty()) {
    return 0.0;
  }
  const std::size_t begin =
      latencies.size() > window ? latencies.size() - window : 0;
  std::vector<double> recent(latencies.begin() +
                                 static_cast<std::ptrdiff_t>(begin),
                             latencies.end());
  return agentmem::summarize_latency(recent).p99_ms;
}

struct CompactionTick {
  std::size_t moved = 0;
  std::size_t io_bytes = 0;
  double ms = 0.0;
};

void write_compaction_io_file(const std::string& path, std::size_t bytes) {
  if (bytes == 0) {
    return;
  }

  std::vector<char> block(std::min<std::size_t>(bytes, 1024 * 1024), 0);
  for (std::size_t i = 0; i < block.size(); ++i) {
    block[i] = static_cast<char>((i * 131) & 0xff);
  }

#ifdef _WIN32
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    throw std::runtime_error("Cannot open compaction I/O file: " + path);
  }
  std::size_t written = 0;
  while (written < bytes) {
    const std::size_t chunk = std::min(block.size(), bytes - written);
    output.write(block.data(), static_cast<std::streamsize>(chunk));
    if (!output) {
      throw std::runtime_error("Failed to write compaction I/O file: " + path);
    }
    written += chunk;
  }
  output.flush();
  if (!output) {
    throw std::runtime_error("Failed to flush compaction I/O file: " + path);
  }
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd < 0) {
    throw std::runtime_error("Cannot open compaction I/O file: " + path);
  }

  std::size_t written = 0;
  while (written < bytes) {
    const std::size_t chunk = std::min(block.size(), bytes - written);
    const ssize_t rc = ::write(fd, block.data(), chunk);
    if (rc < 0) {
      ::close(fd);
      throw std::runtime_error("Failed to write compaction I/O file: " + path);
    }
    written += static_cast<std::size_t>(rc);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("Failed to fsync compaction I/O file: " + path);
  }
  ::close(fd);
#endif
}

CompactionTick run_compaction_tick(agentmem::DeltaFlatIndex& delta,
                                   const Args& args) {
  CompactionTick tick;
  if (delta.active_size() < args.delta_compaction_threshold) {
    return tick;
  }

  const auto start = std::chrono::steady_clock::now();
  tick.moved = delta.compact_active_to_sealed(args.compaction_batch_size);
  tick.io_bytes = tick.moved * args.compaction_io_bytes_per_vector;
  if (tick.moved > 0 && args.compaction_io_mode == "file") {
    write_compaction_io_file(args.compaction_io_path, tick.io_bytes);
  } else if (tick.moved > 0 && args.compaction_work_us > 0) {
    const auto until =
        std::chrono::steady_clock::now() +
        std::chrono::microseconds(args.compaction_work_us);
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  const auto end = std::chrono::steady_clock::now();
  tick.ms = std::chrono::duration<double, std::milli>(end - start).count();
  return tick;
}

void record_compaction(RunOutput& output, const CompactionTick& tick,
                       bool on_query_path) {
  if (tick.moved == 0) {
    return;
  }
  ++output.compaction_ops;
  output.compaction_vectors += tick.moved;
  output.compaction_io_bytes += tick.io_bytes;
  output.compaction_seconds += tick.ms / 1000.0;
  if (on_query_path) {
    ++output.queries_with_compaction;
  }
}

std::vector<agentmem::SearchResult> exact_dynamic_topk(
    const agentmem::BruteForceIndex& exact_base,
    const agentmem::DeltaFlatIndex& delta, const float* query, std::size_t k) {
  const auto base_results = exact_base.search_one(query, k);
  const auto delta_results = delta.search_one(query, k);
  return agentmem::merge_topk(base_results, delta_results, k);
}

template <typename Index>
RunOutput run_mixed_graph_with_index(Index& index, const Args& args,
                                     const agentmem::VectorSet& base,
                                     const agentmem::VectorSet& queries,
                                     QueryPathCache& path_cache) {
  if (args.engine != "graph") {
    throw std::runtime_error("Mixed workload currently requires --engine graph");
  }
  if (queries.empty()) {
    throw std::runtime_error("Mixed workload requires non-empty queries");
  }

  agentmem::DeltaFlatIndex delta(base.dim);
  agentmem::WalWriter wal(args.wal_path, base.dim);
  const agentmem::BruteForceIndex exact_base(base);

  RunOutput output;
  output.operation_count =
      args.operation_count == 0 ? queries.size() : args.operation_count;
  output.results.reserve(output.operation_count);
  output.latencies_ms.reserve(output.operation_count);
  output.ssd_reads.reserve(output.operation_count);
  output.cache_requests.reserve(output.operation_count);
  output.cache_hits.reserve(output.operation_count);
  output.cache_misses.reserve(output.operation_count);
  output.path_cache_requests.reserve(output.operation_count);
  output.path_cache_hits.reserve(output.operation_count);
  output.expanded.reserve(output.operation_count);
  output.visited.reserve(output.operation_count);
  output.insert_latencies_ms.reserve(output.operation_count);
  output.query_compaction_ms.reserve(output.operation_count);
  output.dynamic_truth.reserve(output.operation_count);

  std::size_t query_index = 0;
  std::size_t insert_index = 0;
  std::size_t write_budget = 0;

  const auto batch_start = std::chrono::steady_clock::now();
  for (std::size_t op = 0; op < output.operation_count; ++op) {
    write_budget += args.write_ratio_percent;
    const bool is_insert = write_budget >= 100;
    if (is_insert) {
      write_budget -= 100;
      const auto insert_start = std::chrono::steady_clock::now();
      const auto vector = make_insert_vector(
          queries, query_index == 0 ? 0 : query_index - 1, insert_index,
          args.synthetic_config.seed);
      const auto id =
          static_cast<std::uint32_t>(base.size() + insert_index);
      wal.append_insert(id, vector.data());
      wal.flush();
      delta.insert(id, vector.data());
      ++insert_index;
      ++output.insert_count;

      if (args.compaction_policy == "sla") {
        const double p99 = recent_p99_ms(output.latencies_ms, 32);
        if (p99 == 0.0 || p99 < args.sla_p99_ms) {
          const auto tick = run_compaction_tick(delta, args);
          record_compaction(output, tick, false);
        } else if (delta.active_size() >= args.delta_compaction_threshold) {
          ++output.compaction_skipped_sla;
        }
      }

      const auto insert_end = std::chrono::steady_clock::now();
      output.insert_latencies_ms.push_back(
          std::chrono::duration<double, std::milli>(insert_end - insert_start)
              .count());
      continue;
    }

    const float* query = queries.row(query_index % queries.size());
    ++query_index;
    bool path_hit = false;
    double query_compaction_ms = 0.0;

    const auto query_start = std::chrono::steady_clock::now();
    if (args.compaction_policy == "aggressive") {
      const auto tick = run_compaction_tick(delta, args);
      query_compaction_ms = tick.ms;
      record_compaction(output, tick, true);
    } else if (args.compaction_policy == "sla" &&
               delta.active_size() >= args.delta_compaction_threshold) {
      ++output.compaction_skipped_sla;
    }

    auto main_result =
        search_graph_one_with_routing(index, args, base, query, path_cache,
                                      path_hit);
    const auto delta_results = delta.search_one(query, args.k);
    const auto merged =
        agentmem::merge_topk(main_result.topk, delta_results, args.k);
    const auto truth_results =
        exact_dynamic_topk(exact_base, delta, query, args.k);
    const auto query_end = std::chrono::steady_clock::now();

    output.latencies_ms.push_back(
        std::chrono::duration<double, std::milli>(query_end - query_start)
            .count());
    output.query_compaction_ms.push_back(query_compaction_ms);
    output.ssd_reads.push_back(static_cast<double>(main_result.stats.node_reads));
    output.cache_requests.push_back(
        static_cast<double>(main_result.stats.page_requests));
    output.cache_hits.push_back(
        static_cast<double>(main_result.stats.page_cache_hits));
    output.cache_misses.push_back(
        static_cast<double>(main_result.stats.page_cache_misses));
    output.path_cache_requests.push_back(path_cache.enabled() ? 1.0 : 0.0);
    output.path_cache_hits.push_back(path_hit ? 1.0 : 0.0);
    output.expanded.push_back(static_cast<double>(main_result.stats.expanded));
    output.visited.push_back(static_cast<double>(main_result.stats.visited));
    output.results.push_back(merged);
    output.dynamic_truth.push_back(ids_from_results(truth_results));
  }
  const auto batch_end = std::chrono::steady_clock::now();
  output.elapsed_seconds =
      std::chrono::duration<double>(batch_end - batch_start).count();
  output.delta_active_size = delta.active_size();
  output.delta_sealed_size = delta.sealed_size();
  output.wal_records = wal.stats().records;
  output.wal_bytes = wal.stats().bytes;
  return output;
}

template <typename Index>
RunOutput run_graph_with_index(Index& index, const Args& args,
                               const agentmem::VectorSet& base,
                               const agentmem::VectorSet& queries) {
  if (index.metadata().dim != base.dim) {
    throw std::runtime_error("Graph index dimension does not match loaded base");
  }
  if (index.metadata().vector_count != base.size()) {
    throw std::runtime_error("Graph index vector count does not match loaded base");
  }

  std::cout << "graph_index_path=" << args.index_path << "\n";
  std::cout << "run_type=" << args.run_type << "\n";
  std::cout << "warmup_runs=" << args.warmup_runs << "\n";
  std::cout << "layout=" << args.layout << "\n";
  std::cout << "cache_policy=" << args.cache_policy << "\n";
  std::cout << "cache_pages=" << args.cache_pages << "\n";
  std::cout << "path_cache_policy=" << args.path_cache_policy << "\n";
  std::cout << "path_cache_capacity=" << args.path_cache_capacity << "\n";
  std::cout << "path_cache_hit_search_width="
            << args.path_cache_hit_search_width << "\n";
  std::cout << "workload_mode=" << args.workload_mode << "\n";
  std::cout << "operation_count=" << args.operation_count << "\n";
  std::cout << "write_ratio_percent=" << args.write_ratio_percent << "\n";
  std::cout << "wal_path=" << args.wal_path << "\n";
  std::cout << "compaction_policy=" << args.compaction_policy << "\n";
  std::cout << "delta_compaction_threshold="
            << args.delta_compaction_threshold << "\n";
  std::cout << "compaction_batch_size=" << args.compaction_batch_size << "\n";
  std::cout << "compaction_work_us=" << args.compaction_work_us << "\n";
  std::cout << "compaction_io_mode=" << args.compaction_io_mode << "\n";
  std::cout << "compaction_io_path=" << args.compaction_io_path << "\n";
  std::cout << "compaction_io_bytes_per_vector="
            << args.compaction_io_bytes_per_vector << "\n";
  std::cout << "sla_p99_ms=" << args.sla_p99_ms << "\n";
  std::cout << "packing_strategy="
            << (args.layout == "packed" ? args.packing_strategy : "none")
            << "\n";
  std::cout << "graph_vector_count=" << index.metadata().vector_count << "\n";
  std::cout << "graph_degree=" << index.metadata().degree << "\n";
  std::cout << "graph_page_size=" << index.metadata().page_size << "\n";
  std::cout << "graph_page_count=" << index.metadata().page_count << "\n";
  std::cout << "graph_nodes_per_page=" << index.metadata().nodes_per_page
            << "\n";
  std::cout << "search_width=" << args.search_width << "\n";
  std::cout << "entry_count=" << args.entry_count << "\n";
  std::cout << "routing_sample_count=" << args.routing_sample_count << "\n";

  QueryPathCache path_cache(args.path_cache_policy, args.path_cache_capacity);
  for (std::size_t i = 0; i < args.warmup_runs; ++i) {
    (void)execute_graph_queries(index, args, base, queries, false, path_cache);
  }
  if (args.workload_mode == "mixed") {
    return run_mixed_graph_with_index(index, args, base, queries, path_cache);
  }
  return execute_graph_queries(index, args, base, queries, true, path_cache);
}

RunOutput run_graph(const Args& args, const agentmem::VectorSet& base,
                    const agentmem::VectorSet& queries) {
  if (args.build_index) {
    agentmem::DiskGraphBuildConfig build_config;
    build_config.degree = args.graph_degree;
    build_config.page_size = args.page_size;
    build_config.packing_strategy = args.packing_strategy;
    build_config.random_seed = args.synthetic_config.seed;
    build_config.coaccess_sessions = args.coaccess_sessions;
    build_config.coaccess_trace_length = args.coaccess_trace_length;
    const auto build_start = std::chrono::steady_clock::now();
    if (args.layout == "one-node") {
      agentmem::NaiveDiskGraphBuilder::build(base, args.index_path,
                                             build_config);
    } else {
      agentmem::PackedDiskGraphBuilder::build(base, args.index_path,
                                              build_config);
    }
    const auto build_end = std::chrono::steady_clock::now();
    const double build_seconds =
        std::chrono::duration<double>(build_end - build_start).count();
    std::cout << "graph_build_seconds=" << build_seconds << "\n";
  }

  if (args.layout == "one-node") {
    agentmem::NaiveDiskGraphIndex index(args.index_path);
    return run_graph_with_index(index, args, base, queries);
  }

  agentmem::PackedDiskGraphIndex index(args.index_path);
  index.configure_cache(args.cache_policy, args.cache_pages);
  return run_graph_with_index(index, args, base, queries);
}

void print_optional_stats(const std::string& prefix,
                          const std::vector<double>& values) {
  if (values.empty()) {
    return;
  }
  const auto stats = agentmem::summarize_latency(values);
  std::cout << prefix << "_avg=" << stats.avg_ms << "\n";
  std::cout << prefix << "_p95=" << stats.p95_ms << "\n";
  std::cout << prefix << "_p99=" << stats.p99_ms << "\n";
}

double sum_values(const std::vector<double>& values) {
  double sum = 0.0;
  for (const double value : values) {
    sum += value;
  }
  return sum;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);

    agentmem::VectorSet base;
    agentmem::VectorSet queries;
    std::vector<std::vector<std::uint32_t>> truth;

    if (args.synthetic) {
      auto data = agentmem::generate_synthetic(args.synthetic_config);
      base = std::move(data.base);
      queries = std::move(data.queries);
    std::cout << "mode=synthetic\n";
    std::cout << "synthetic_workload=" << args.synthetic_config.workload << "\n";
    std::cout << "session_length=" << args.synthetic_config.session_length << "\n";
    } else {
      base = agentmem::load_fvecs(args.base_path, args.base_limit);
      queries = agentmem::load_fvecs(args.query_path, args.query_limit);
      if (!args.truth_path.empty()) {
        truth = agentmem::load_ivecs(args.truth_path, queries.size());
      }
      std::cout << "mode=fvecs\n";
    }

    if (base.dim != queries.dim) {
      throw std::runtime_error("Base and query dimensions do not match");
    }

    std::cout << "engine=" << args.engine << "\n";
    std::cout << "base_count=" << base.size() << "\n";
    std::cout << "query_count=" << queries.size() << "\n";
    std::cout << "dim=" << base.dim << "\n";
    std::cout << "k=" << args.k << "\n";

    RunOutput output;
    if (args.engine == "exact") {
      output = run_exact(base, queries, args.k);
    } else {
      output = run_graph(args, base, queries);
    }

    if (!output.dynamic_truth.empty()) {
      truth = output.dynamic_truth;
      std::cout << "truth=dynamic_exact_bruteforce\n";
    } else if (truth.empty()) {
      if (args.engine == "exact") {
        truth = results_as_truth(output.results);
        std::cout << "truth=exact_self\n";
      } else {
        truth = exact_truth(base, queries, args.k);
        std::cout << "truth=exact_bruteforce\n";
      }
    } else {
      std::cout << "truth=file\n";
    }

    const auto latency = agentmem::summarize_latency(output.latencies_ms);
    const double recall = agentmem::recall_at_k(output.results, truth, args.k);
    const double one_minus_recall = 1.0 - recall;
    const std::size_t measured_queries = output.results.size();

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "recall_at_" << args.k << "=" << recall << "\n";
    std::cout << "one_minus_recall_at_" << args.k << "=" << one_minus_recall
              << "\n";
    std::cout << "qps="
              << agentmem::queries_per_second(measured_queries,
                                              output.elapsed_seconds)
              << "\n";
    if (output.operation_count > 0) {
      std::cout << "ops_per_second="
                << agentmem::queries_per_second(output.operation_count,
                                                output.elapsed_seconds)
                << "\n";
      std::cout << "measured_queries=" << measured_queries << "\n";
      std::cout << "insert_count=" << output.insert_count << "\n";
    }
    std::cout << "avg_latency_ms=" << latency.avg_ms << "\n";
    std::cout << "p50_latency_ms=" << latency.p50_ms << "\n";
    std::cout << "p95_latency_ms=" << latency.p95_ms << "\n";
    std::cout << "p99_latency_ms=" << latency.p99_ms << "\n";
    print_optional_stats("ssd_reads_per_query", output.ssd_reads);
    print_optional_stats("page_cache_requests_per_query",
                         output.cache_requests);
    print_optional_stats("page_cache_hits_per_query", output.cache_hits);
    print_optional_stats("page_cache_misses_per_query", output.cache_misses);
    print_optional_stats("path_cache_requests_per_query",
                         output.path_cache_requests);
    print_optional_stats("path_cache_hits_per_query", output.path_cache_hits);
    print_optional_stats("graph_expanded_per_query", output.expanded);
    print_optional_stats("graph_visited_per_query", output.visited);
    print_optional_stats("insert_latency_ms", output.insert_latencies_ms);
    print_optional_stats("query_compaction_ms", output.query_compaction_ms);
    if (!output.ssd_reads.empty()) {
      const auto read_stats = agentmem::summarize_latency(output.ssd_reads);
      std::cout << "io_amplification_reads_per_result="
                << (read_stats.avg_ms / static_cast<double>(args.k)) << "\n";
    }
    const double total_cache_requests = sum_values(output.cache_requests);
    if (total_cache_requests > 0.0) {
      std::cout << "page_cache_hit_rate="
                << (sum_values(output.cache_hits) / total_cache_requests)
                << "\n";
    }
    const double total_path_requests = sum_values(output.path_cache_requests);
    if (total_path_requests > 0.0) {
      std::cout << "path_cache_hit_rate="
                << (sum_values(output.path_cache_hits) / total_path_requests)
                << "\n";
    }
    if (output.operation_count > 0) {
      std::cout << "delta_active_size=" << output.delta_active_size << "\n";
      std::cout << "delta_sealed_size=" << output.delta_sealed_size << "\n";
      std::cout << "wal_records=" << output.wal_records << "\n";
      std::cout << "wal_bytes=" << output.wal_bytes << "\n";
      std::cout << "compaction_ops=" << output.compaction_ops << "\n";
      std::cout << "compaction_vectors=" << output.compaction_vectors << "\n";
      std::cout << "compaction_io_bytes="
                << output.compaction_io_bytes << "\n";
      std::cout << "compaction_seconds=" << output.compaction_seconds << "\n";
      std::cout << "compaction_skipped_sla="
                << output.compaction_skipped_sla << "\n";
      std::cout << "queries_with_compaction="
                << output.queries_with_compaction << "\n";
      std::cout << "compaction_work_ms_per_query="
                << (measured_queries == 0
                        ? 0.0
                        : (output.compaction_seconds * 1000.0) /
                              static_cast<double>(measured_queries))
                << "\n";
      std::cout << "compaction_interference_ms_per_query="
                << (measured_queries == 0
                        ? 0.0
                        : sum_values(output.query_compaction_ms) /
                              static_cast<double>(measured_queries))
                << "\n";
    }
    std::cout << "elapsed_seconds=" << output.elapsed_seconds << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
