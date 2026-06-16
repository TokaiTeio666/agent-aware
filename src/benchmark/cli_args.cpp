#include "agentmem/benchmark/cli_args.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace agentmem::benchmark {
namespace {

void print_usage(const char* program) {
  std::cout
      << "AgentMem-Flow: vector retrieval baseline and SSD graph baseline\n\n"
      << "Usage:\n"
      << "  " << program << " --engine memory --synthetic [--base-count N]\n"
      << "              [--query-count N] [--dim D] [--clusters C] [--k K]\n"
      << "  " << program << " --engine graph --synthetic --build-index\n"
      << "              [--index path/v1_graph.idx] [--graph-degree R]\n"
      << "              [--layout one-node|packed]\n"
      << "              [--packing random|bfs|coaccess]\n"
      << "              [--search-width W] [--entry-count E]\n"
      << "              [--routing-sample-count S]\n"
      << "  " << program << " --engine memory|graph --base path/base.fvecs\n"
      << "              --query path/query.fvecs [--truth path/groundtruth.ivecs]\n\n"
      << "  " << program << " --engine memory|graph --sift-dir path/to/sift\n\n"
      << "Options:\n"
      << "  --engine NAME        memory, exact alias, or graph, default exact\n"
      << "  --sift-dir DIR       Official SIFT dir with sift_base.fvecs,\n"
      << "                       sift_query.fvecs, and sift_groundtruth.ivecs\n"
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
      << "                       or hotpath\n"
      << "  --hotpath-train-queries N Sample queries for hotpath packing\n"
      << "  --hotpath-search-width N  Training graph expansions/query\n"
      << "  --hotpath-entry-count N   Training graph entry count\n"
      << "  --run-type NAME      smoke, cold, or warm, default smoke\n"
      << "  --warmup-runs N      Unmeasured warmup passes before measurement\n"
      << "  --cache-policy NAME  none, lru, 2q, or graph-aware-2q\n"
      << "  --cache-pages N      Global packed page cache capacity, default 0\n"
      << "  --cache-budget-bytes N  Derive packed cache pages from byte budget\n"
      << "  --cache-protect-hot-pages 0|1  Protect pages with high-degree nodes\n"
      << "  --cache-hot-degree-threshold N Degree threshold for hot-page protection\n"
      << "  --path-cache-policy NAME  none, exact, routed, simhash, pq-prefix, or simhash-pq\n"
      << "  --path-cache-capacity N   Query path cache entries, default 0\n"
      << "  --path-cache-hit-search-width N  Search width on path hit\n"
      << "  --query-signature-policy NAME  routed, simhash, pq-prefix, or simhash-pq\n"
      << "  --simhash-bits N     SimHash signature bits, default 16, max 32\n"
      << "  --pq-prefix-subspaces N   PQ prefix subspaces, default 4\n"
      << "  --pq-prefix-centroids N   PQ prefix centroids/subspace, default 16\n"
      << "  --pq-prefix-train-iterations N  PQ prefix k-means passes, default 4\n"
      << "  --workload-mode NAME read-only or mixed, default read-only\n"
      << "  --agent-workload NAME random, hotspot, similar, session-local, or recent-memory\n"
      << "  --operation-count N  Mixed workload operations, default query count\n"
      << "  --write-ratio N      Mixed workload insert percentage, default 0\n"
      << "  --delete-ratio N     Delete percentage within update ops, default 0\n"
      << "  --wal PATH           Delta insert WAL path, default build/v5_delta.wal\n"
      << "  --wal-replay         Replay existing WAL into delta before workload\n"
      << "  --delta-index-policy NAME  flat or ivf-flat, default flat\n"
      << "  --delta-ivf-centroids N    Delta IVF centroid count, default 32\n"
      << "  --delta-ivf-probes N       Delta IVF probes/query, default 16\n"
      << "  --delta-ivf-train-iterations N  Delta IVF k-means passes, default 6\n"
      << "  --delta-ivf-rebuild-interval N  Inserts between IVF rebuilds, default 32\n"
      << "  --compaction-policy NAME none, aggressive, or sla, default none\n"
      << "                            stream-merge runs LSM merge after workload\n"
      << "  --stream-merge-index PATH  Output path for merged FreshVamana LTI\n"
      << "  --delta-compaction-threshold N  Active delta size to compact\n"
      << "  --compaction-batch-size N       Vectors moved per compaction tick\n"
      << "  --compaction-work-us N          Simulated compaction I/O time/tick\n"
      << "  --compaction-io-mode NAME       time or file, default time\n"
      << "  --compaction-io-path PATH       File path for V6 file I/O compaction\n"
      << "  --compaction-io-bytes-per-vector N  Bytes written per compacted vector\n"
      << "  --sla-p99-ms X       P99 budget for SLA compaction, default 1.0\n"
      << "  --graph-degree R     Graph out-degree, default 32\n"
      << "  --graph-build-policy NAME  exact, approx-rp, lsh-rp, lsh-vamana, or vamana\n"
      << "  --approx-projections N     Random projection count for approx-rp\n"
      << "  --approx-window N          Projection-rank window/ring fallback\n"
      << "  --approx-random-samples N  Extra random construction candidates\n"
      << "  --approx-candidate-limit N Max candidates/vector, default 256, 0 disables cap\n"
      << "  --candidate-limit N Alias for --approx-candidate-limit\n"
      << "  --lsh-tables N       LSH tables for lsh-rp, default 8\n"
      << "  --lsh-bits N         SimHash bits/table for lsh-rp, default 14\n"
      << "  --lsh-probe-radius N Multi-probe Hamming radius 0 or 1, default 0\n"
      << "  --lsh-bucket-limit N Max ids sampled/bucket for lsh-rp, default 64\n"
      << "  --robust-prune-alpha X     FreshVamana RobustPrune alpha, default 1.2\n"
      << "  --reverse-edge-patch 0|1   Add reverse-edge patch after pruning, default 1\n"
      << "  --prune-passes N           Reverse/prune patch passes, default 1\n"
      << "  --page-size BYTES    Fixed node page size, default 4096\n"
      << "  --search-width W     Max graph node expansions/query, default 64\n"
      << "  --search-early-stop  Stop graph search when frontier is worse than Top-K\n"
      << "  --search-early-stop-min N  Min expansions before early stop, default 0\n"
      << "  --adaptive-early-stop      Enable patience-based adaptive stop\n"
      << "  --early-stop-patience N    Stagnant expansions before stop, default 16\n"
      << "  --early-stop-eps X         Relative Top-K improvement threshold\n"
      << "  --min-expansions N         Adaptive stop lower bound, default 64\n"
      << "  --max-expansions N         Adaptive stop upper bound, default 192\n"
      << "  --entry-count E      Evenly spaced graph entry points, default 32\n"
      << "  --routing-sample-count S  Resident sampled vectors for seed routing,\n"
      << "                            default 256, use 0 to disable\n"
      << "  --coaccess-sessions N      Agent-style trace sessions, default 64\n"
      << "  --coaccess-trace-length N  Agent-style trace length, default 32\n"
      << "  --pq-enable [0|1]    Train resident PQ codes for graph candidates\n"
      << "  --pq-m N             PQ subspaces, default 8\n"
      << "  --pq-ks N            PQ centroids/subspace, default 256\n"
      << "  --pq-train-limit N   PQ training sample limit, default 100000\n"
      << "  --pq-train-iterations N  PQ k-means passes, default 4\n"
      << "  --pq-code-path PATH  Reserved PQ codebook/code persistence path\n"
      << "  --adc-enable [0|1]   Use ADC lookup tables during graph search\n"
      << "  --rerank-topk N      Raw-vector rerank candidates, default 0\n"
      << "  --io-mode NAME       pread, odirect, or io_uring, default pread\n"
      << "  --io-batch-size N    Requested async I/O batch size, default 1\n"
      << "  --io-depth N         Max async page reads in flight, default 1\n"
      << "  --prefetch-width N   Frontier pages to prefetch, 0 uses io-depth\n"
      << "  --prefetch-policy NAME  none, frontier, next-hop, or frontier-next-hop\n"
      << "  --page-dedup 0|1     Deduplicate cached/pending/seen pages, default 1\n"
      << "  --same-page-reuse 0|1 Count same-page decoded node reuse, default 1\n"
      << "  --page-coalesce 0|1  Sort/dedup frontier page prefetch batches, default 1\n"
      << "  --allow-io-fallback  Permit odirect/io_uring to fall back to pread\n"
      << "  --memory-budget-ratio X  Resident memory budget/raw vectors, default 0.20\n"
      << "  --memory-budget-bytes N  Hard resident memory budget in bytes\n"
      << "  --enforce-memory-budget  Fail if resident engine memory exceeds budget\n"
      << "  --allow-over-budget-for-debug  Report but do not fail over-budget runs\n"
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

bool parse_bool01(const std::string& value, const std::string& name) {
  const std::size_t parsed = parse_size(value, name);
  if (parsed > 1) {
    throw std::runtime_error(name + " must be 0 or 1");
  }
  return parsed != 0;
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

std::string join_path(const std::string& dir, const std::string& name) {
  if (dir.empty()) {
    return name;
  }
  const char last = dir.back();
  if (last == '/' || last == '\\') {
    return dir + name;
  }
  return dir + "/" + name;
}

}  // namespace

bool is_memory_engine(const std::string& engine) {
  return engine == "memory" || engine == "exact";
}

Args parse_args(int argc, char** argv) {
  // 解析与校验集中在入口，避免索引实现层重复处理命令行语义。
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
    } else if (opt == "--sift-dir") {
      args.sift_dir = require_value(opt);
      args.base_path = join_path(args.sift_dir, "sift_base.fvecs");
      args.query_path = join_path(args.sift_dir, "sift_query.fvecs");
      args.truth_path = join_path(args.sift_dir, "sift_groundtruth.ivecs");
      args.synthetic = false;
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
    } else if (opt == "--hotpath-train-queries") {
      args.hotpath_train_queries = parse_size(require_value(opt), opt);
    } else if (opt == "--hotpath-search-width") {
      args.hotpath_search_width = parse_size(require_value(opt), opt);
    } else if (opt == "--hotpath-entry-count") {
      args.hotpath_entry_count = parse_size(require_value(opt), opt);
    } else if (opt == "--run-type") {
      args.run_type = require_value(opt);
    } else if (opt == "--warmup-runs") {
      args.warmup_runs = parse_size(require_value(opt), opt);
    } else if (opt == "--cache-policy") {
      args.cache_policy = require_value(opt);
    } else if (opt == "--cache-pages") {
      args.cache_pages = parse_size(require_value(opt), opt);
    } else if (opt == "--cache-budget-bytes") {
      args.cache_budget_bytes = parse_size(require_value(opt), opt);
    } else if (opt == "--cache-protect-hot-pages") {
      args.cache_protect_hot_pages = parse_bool01(require_value(opt), opt);
    } else if (opt == "--cache-hot-degree-threshold") {
      args.cache_hot_degree_threshold = parse_size(require_value(opt), opt);
    } else if (opt == "--path-cache-policy") {
      args.path_cache_policy = require_value(opt);
    } else if (opt == "--path-cache-capacity") {
      args.path_cache_capacity = parse_size(require_value(opt), opt);
    } else if (opt == "--path-cache-hit-search-width") {
      args.path_cache_hit_search_width = parse_size(require_value(opt), opt);
    } else if (opt == "--query-signature-policy") {
      args.query_signature_policy = require_value(opt);
    } else if (opt == "--simhash-bits") {
      args.simhash_bits = parse_size(require_value(opt), opt);
    } else if (opt == "--pq-prefix-subspaces") {
      args.pq_prefix_subspaces = parse_size(require_value(opt), opt);
    } else if (opt == "--pq-prefix-centroids") {
      args.pq_prefix_centroids = parse_size(require_value(opt), opt);
    } else if (opt == "--pq-prefix-train-iterations") {
      args.pq_prefix_train_iterations = parse_size(require_value(opt), opt);
    } else if (opt == "--workload-mode") {
      args.workload_mode = require_value(opt);
    } else if (opt == "--agent-workload") {
      args.agent_workload_mode = require_value(opt);
    } else if (opt == "--operation-count") {
      args.operation_count = parse_size(require_value(opt), opt);
    } else if (opt == "--write-ratio") {
      args.write_ratio_percent = parse_size(require_value(opt), opt);
    } else if (opt == "--delete-ratio") {
      args.delete_ratio_percent = parse_size(require_value(opt), opt);
    } else if (opt == "--wal") {
      args.wal_path = require_value(opt);
    } else if (opt == "--wal-replay") {
      args.wal_replay = true;
    } else if (opt == "--delta-index-policy") {
      args.delta_index_policy = require_value(opt);
    } else if (opt == "--delta-ivf-centroids") {
      args.delta_ivf_centroids = parse_size(require_value(opt), opt);
    } else if (opt == "--delta-ivf-probes") {
      args.delta_ivf_probes = parse_size(require_value(opt), opt);
    } else if (opt == "--delta-ivf-train-iterations") {
      args.delta_ivf_train_iterations = parse_size(require_value(opt), opt);
    } else if (opt == "--delta-ivf-rebuild-interval") {
      args.delta_ivf_rebuild_interval = parse_size(require_value(opt), opt);
    } else if (opt == "--compaction-policy") {
      args.compaction_policy = require_value(opt);
    } else if (opt == "--stream-merge-index") {
      args.stream_merge_index_path = require_value(opt);
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
    } else if (opt == "--graph-build-policy") {
      args.graph_build_policy = require_value(opt);
    } else if (opt == "--approx-projections") {
      args.approx_projections = parse_size(require_value(opt), opt);
    } else if (opt == "--approx-window") {
      args.approx_window = parse_size(require_value(opt), opt);
    } else if (opt == "--approx-random-samples") {
      args.approx_random_samples = parse_size(require_value(opt), opt);
    } else if (opt == "--approx-candidate-limit") {
      args.approx_candidate_limit = parse_size(require_value(opt), opt);
    } else if (opt == "--candidate-limit") {
      args.approx_candidate_limit = parse_size(require_value(opt), opt);
    } else if (opt == "--lsh-tables") {
      args.lsh_tables = parse_size(require_value(opt), opt);
    } else if (opt == "--lsh-bits") {
      args.lsh_bits = parse_size(require_value(opt), opt);
    } else if (opt == "--lsh-probe-radius") {
      args.lsh_probe_radius = parse_size(require_value(opt), opt);
    } else if (opt == "--lsh-bucket-limit") {
      args.lsh_bucket_limit = parse_size(require_value(opt), opt);
    } else if (opt == "--robust-prune-alpha") {
      args.robust_prune_alpha = parse_double(require_value(opt), opt);
    } else if (opt == "--reverse-edge-patch") {
      args.reverse_edge_patch = parse_bool01(require_value(opt), opt);
    } else if (opt == "--prune-passes") {
      args.prune_passes = parse_size(require_value(opt), opt);
    } else if (opt == "--page-size") {
      args.page_size = parse_size(require_value(opt), opt);
    } else if (opt == "--search-width") {
      args.search_width = parse_size(require_value(opt), opt);
    } else if (opt == "--search-early-stop") {
      args.search_early_stop = true;
    } else if (opt == "--search-early-stop-min") {
      args.search_early_stop_min_expansions = parse_size(require_value(opt), opt);
    } else if (opt == "--adaptive-early-stop") {
      args.adaptive_early_stop = true;
    } else if (opt == "--early-stop-patience") {
      args.early_stop_patience = parse_size(require_value(opt), opt);
    } else if (opt == "--early-stop-eps") {
      args.early_stop_eps = parse_double(require_value(opt), opt);
    } else if (opt == "--min-expansions") {
      args.min_expansions = parse_size(require_value(opt), opt);
    } else if (opt == "--max-expansions") {
      args.max_expansions = parse_size(require_value(opt), opt);
    } else if (opt == "--entry-count") {
      args.entry_count = parse_size(require_value(opt), opt);
    } else if (opt == "--routing-sample-count") {
      args.routing_sample_count = parse_size(require_value(opt), opt);
    } else if (opt == "--coaccess-sessions") {
      args.coaccess_sessions = parse_size(require_value(opt), opt);
    } else if (opt == "--coaccess-trace-length") {
      args.coaccess_trace_length = parse_size(require_value(opt), opt);
    } else if (opt == "--pq-enable") {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        args.pq_enable = parse_bool01(argv[++i], opt);
      } else {
        args.pq_enable = true;
      }
    } else if (opt == "--pq-m") {
      args.pq_m = parse_size(require_value(opt), opt);
    } else if (opt == "--pq-ks") {
      args.pq_ks = parse_size(require_value(opt), opt);
    } else if (opt == "--pq-train-limit") {
      args.pq_train_limit = parse_size(require_value(opt), opt);
    } else if (opt == "--pq-train-iterations") {
      args.pq_train_iterations = parse_size(require_value(opt), opt);
    } else if (opt == "--pq-code-path") {
      args.pq_code_path = require_value(opt);
    } else if (opt == "--adc-enable") {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        args.adc_enable = parse_bool01(argv[++i], opt);
      } else {
        args.adc_enable = true;
      }
    } else if (opt == "--rerank-topk") {
      args.rerank_topk = parse_size(require_value(opt), opt);
    } else if (opt == "--io-mode") {
      args.io_mode = require_value(opt);
    } else if (opt == "--io-batch-size") {
      args.io_batch_size = parse_size(require_value(opt), opt);
    } else if (opt == "--io-depth") {
      args.io_depth = parse_size(require_value(opt), opt);
    } else if (opt == "--prefetch-width") {
      args.prefetch_width = parse_size(require_value(opt), opt);
    } else if (opt == "--prefetch-policy") {
      args.prefetch_policy = require_value(opt);
    } else if (opt == "--page-dedup") {
      args.page_dedup = parse_bool01(require_value(opt), opt);
    } else if (opt == "--same-page-reuse") {
      args.same_page_reuse = parse_bool01(require_value(opt), opt);
    } else if (opt == "--page-coalesce") {
      args.page_coalesce = parse_bool01(require_value(opt), opt);
    } else if (opt == "--allow-io-fallback") {
      args.allow_io_fallback = true;
    } else if (opt == "--memory-budget-ratio") {
      args.memory_budget_ratio = parse_double(require_value(opt), opt);
    } else if (opt == "--memory-budget-bytes") {
      args.memory_budget_bytes = parse_size(require_value(opt), opt);
    } else if (opt == "--enforce-memory-budget") {
      args.enforce_memory_budget = true;
    } else if (opt == "--allow-over-budget-for-debug") {
      args.allow_over_budget_for_debug = true;
    } else {
      throw std::runtime_error("Unknown option: " + opt);
    }
  }

  if (!is_memory_engine(args.engine) && args.engine != "graph") {
    throw std::runtime_error("--engine must be memory, exact, or graph");
  }
  if (args.layout != "one-node" && args.layout != "packed") {
    throw std::runtime_error("--layout must be one-node or packed");
  }
  if (args.packing_strategy != "random" && args.packing_strategy != "bfs" &&
      args.packing_strategy != "coaccess" &&
      args.packing_strategy != "hotpath") {
    throw std::runtime_error(
        "--packing must be random, bfs, coaccess, or hotpath");
  }
  if (args.run_type != "smoke" && args.run_type != "cold" &&
      args.run_type != "warm" && args.run_type != "dev" &&
      args.run_type != "final") {
    throw std::runtime_error("--run-type must be smoke, cold, warm, dev, or final");
  }
  if (args.cache_policy != "none" && args.cache_policy != "lru" &&
      args.cache_policy != "2q" && args.cache_policy != "graph-aware-2q" &&
      args.cache_policy != "agent") {
    throw std::runtime_error(
        "--cache-policy must be none, lru, 2q, or graph-aware-2q");
  }
  if (args.path_cache_policy == "reuse" ||
      args.path_cache_policy == "exact" ||
      args.path_cache_policy == "routed") {
    args.query_signature_policy = "routed";
  } else if (args.path_cache_policy == "simhash" ||
             args.path_cache_policy == "pq-prefix" ||
             args.path_cache_policy == "simhash-pq") {
    args.query_signature_policy = args.path_cache_policy;
  } else if (args.path_cache_policy != "none") {
    throw std::runtime_error(
        "--path-cache-policy must be none, exact, routed, simhash, pq-prefix, or simhash-pq");
  }
  if (args.query_signature_policy != "routed" &&
      args.query_signature_policy != "simhash" &&
      args.query_signature_policy != "pq-prefix" &&
      args.query_signature_policy != "simhash-pq") {
    throw std::runtime_error(
        "--query-signature-policy must be routed, simhash, pq-prefix, or simhash-pq");
  }
  if (args.simhash_bits == 0 || args.simhash_bits > 32) {
    throw std::runtime_error("--simhash-bits must be between 1 and 32");
  }
  if (args.pq_prefix_subspaces == 0) {
    throw std::runtime_error("--pq-prefix-subspaces must be positive");
  }
  if (args.pq_prefix_centroids < 2 || args.pq_prefix_centroids > 256) {
    throw std::runtime_error("--pq-prefix-centroids must be between 2 and 256");
  }
  if (args.pq_prefix_train_iterations == 0) {
    throw std::runtime_error("--pq-prefix-train-iterations must be positive");
  }
  if (args.workload_mode != "read-only" && args.workload_mode != "mixed") {
    throw std::runtime_error("--workload-mode must be read-only or mixed");
  }
  if (args.agent_workload_mode != "random" &&
      args.agent_workload_mode != "hotspot" &&
      args.agent_workload_mode != "similar" &&
      args.agent_workload_mode != "session-local" &&
      args.agent_workload_mode != "recent-memory") {
    throw std::runtime_error(
        "--agent-workload must be random, hotspot, similar, session-local, or recent-memory");
  }
  if (args.workload_mode == "mixed" && args.engine != "graph") {
    throw std::runtime_error("--workload-mode mixed currently requires --engine graph");
  }
  if (args.write_ratio_percent > 100) {
    throw std::runtime_error("--write-ratio must be between 0 and 100");
  }
  if (args.delete_ratio_percent > 100) {
    throw std::runtime_error("--delete-ratio must be between 0 and 100");
  }
  if (args.delta_index_policy != "flat" &&
      args.delta_index_policy != "ivf-flat") {
    throw std::runtime_error("--delta-index-policy must be flat or ivf-flat");
  }
  if (args.delta_ivf_centroids == 0) {
    throw std::runtime_error("--delta-ivf-centroids must be positive");
  }
  if (args.delta_ivf_probes == 0) {
    throw std::runtime_error("--delta-ivf-probes must be positive");
  }
  if (args.delta_ivf_train_iterations == 0) {
    throw std::runtime_error("--delta-ivf-train-iterations must be positive");
  }
  if (args.delta_ivf_rebuild_interval == 0) {
    throw std::runtime_error("--delta-ivf-rebuild-interval must be positive");
  }
  if (args.compaction_policy != "none" &&
      args.compaction_policy != "aggressive" &&
      args.compaction_policy != "sla" &&
      args.compaction_policy != "stream-merge") {
    throw std::runtime_error(
        "--compaction-policy must be none, aggressive, sla, or stream-merge");
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
  if (args.graph_degree == 0) {
    throw std::runtime_error("--graph-degree must be positive");
  }
  if (args.graph_build_policy != "exact" &&
      args.graph_build_policy != "approx-rp" &&
      args.graph_build_policy != "lsh-rp" &&
      args.graph_build_policy != "lsh-vamana" &&
      args.graph_build_policy != "vamana") {
    throw std::runtime_error(
        "--graph-build-policy must be exact, approx-rp, lsh-rp, "
        "lsh-vamana, or vamana");
  }
  if (args.graph_build_policy == "approx-rp" ||
      args.graph_build_policy == "lsh-rp" ||
      args.graph_build_policy == "lsh-vamana") {
    if (args.approx_projections == 0) {
      throw std::runtime_error("--approx-projections must be positive");
    }
    if (args.approx_window == 0) {
      throw std::runtime_error("--approx-window must be positive");
    }
    if (args.approx_candidate_limit != 0 &&
        args.approx_candidate_limit < args.graph_degree) {
      throw std::runtime_error(
          "--approx-candidate-limit must be 0 or at least --graph-degree");
    }
  }
  if (args.graph_build_policy == "lsh-rp" ||
      args.graph_build_policy == "lsh-vamana") {
    if (args.lsh_tables == 0) {
      throw std::runtime_error("--lsh-tables must be positive");
    }
    if (args.lsh_bits == 0 || args.lsh_bits > 24) {
      throw std::runtime_error("--lsh-bits must be between 1 and 24");
    }
    if (args.lsh_probe_radius > 1) {
      throw std::runtime_error(
          "--lsh-probe-radius currently supports only 0 or 1");
    }
    if (args.lsh_bucket_limit == 0) {
      throw std::runtime_error("--lsh-bucket-limit must be positive");
    }
  }
  if (args.robust_prune_alpha < 1.0) {
    throw std::runtime_error("--robust-prune-alpha must be at least 1.0");
  }
  if (args.prune_passes == 0) {
    throw std::runtime_error("--prune-passes must be positive");
  }
  if (args.hotpath_train_queries == 0 || args.hotpath_search_width == 0 ||
      args.hotpath_entry_count == 0) {
    throw std::runtime_error("hotpath training parameters must be positive");
  }
  if (args.early_stop_patience == 0 || args.max_expansions == 0) {
    throw std::runtime_error("adaptive early-stop limits must be positive");
  }
  if (args.min_expansions > args.max_expansions) {
    throw std::runtime_error("--min-expansions must not exceed --max-expansions");
  }
  if (args.early_stop_eps < 0.0) {
    throw std::runtime_error("--early-stop-eps must be non-negative");
  }
  if (args.pq_m == 0 || args.pq_ks < 2 || args.pq_ks > 256 ||
      args.pq_train_limit == 0 || args.pq_train_iterations == 0) {
    throw std::runtime_error("PQ parameters are invalid");
  }
  if (args.adc_enable && !args.pq_enable) {
    throw std::runtime_error("--adc-enable requires --pq-enable");
  }
  if (args.io_mode != "pread" && args.io_mode != "odirect" &&
      args.io_mode != "io_uring") {
    throw std::runtime_error("--io-mode must be pread, odirect, or io_uring");
  }
  if (args.io_batch_size == 0) {
    throw std::runtime_error("--io-batch-size must be positive");
  }
  if (args.io_depth == 0) {
    throw std::runtime_error("--io-depth must be positive");
  }
  if (args.prefetch_policy != "none" &&
      args.prefetch_policy != "frontier" &&
      args.prefetch_policy != "next-hop" &&
      args.prefetch_policy != "frontier-next-hop") {
    throw std::runtime_error(
        "--prefetch-policy must be none, frontier, next-hop, or frontier-next-hop");
  }
  if (args.memory_budget_ratio <= 0.0 || args.memory_budget_ratio > 1.0) {
    throw std::runtime_error("--memory-budget-ratio must be in (0, 1]");
  }

  return args;
}

}  // namespace agentmem::benchmark
