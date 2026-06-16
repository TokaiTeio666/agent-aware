#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "agentmem/data/dataset.h"

namespace agentmem::benchmark {

struct Args {  // 汇总 CLI 参数，后续实验路径只传递这一份配置。
  std::string engine = "exact";
  std::string base_path;
  std::string query_path;
  std::string truth_path;
  std::string sift_dir;
  std::string index_path = "build/v1_graph.idx";
  std::string layout = "one-node";
  std::string packing_strategy = "coaccess";
  std::string run_type = "smoke";
  std::string cache_policy = "none";
  std::string path_cache_policy = "none";
  std::string query_signature_policy = "routed";
  std::string agent_workload_mode = "random";
  std::string workload_mode = "read-only";
  std::string wal_path = "build/v5_delta.wal";
  std::string delta_index_policy = "flat";
  std::string compaction_policy = "none";
  std::string compaction_io_mode = "time";
  std::string compaction_io_path = "build/v6_compaction_io.bin";
  std::string graph_build_policy = "exact";
  std::string io_mode = "pread";
  std::string stream_merge_index_path;
  std::size_t base_limit = 0;
  std::size_t query_limit = 0;
  std::size_t k = 10;
  std::size_t graph_degree = 32;
  std::size_t page_size = 4096;
  std::size_t search_width = 64;
  std::size_t entry_count = 32;
  std::size_t routing_sample_count = 256;
  std::size_t warmup_runs = 0;
  std::size_t cache_pages = 0;
  std::size_t cache_budget_bytes = 0;
  std::size_t cache_hot_degree_threshold = 0;
  std::size_t path_cache_capacity = 0;
  std::size_t path_cache_hit_search_width = 32;
  std::size_t simhash_bits = 16;
  std::size_t pq_prefix_subspaces = 4;
  std::size_t pq_prefix_centroids = 16;
  std::size_t pq_prefix_train_iterations = 4;
  std::size_t operation_count = 0;
  std::size_t write_ratio_percent = 0;
  std::size_t delete_ratio_percent = 0;
  std::size_t delta_ivf_centroids = 32;
  std::size_t delta_ivf_probes = 16;
  std::size_t delta_ivf_train_iterations = 6;
  std::size_t delta_ivf_rebuild_interval = 32;
  std::size_t delta_compaction_threshold = 64;
  std::size_t compaction_batch_size = 16;
  std::size_t compaction_work_us = 0;
  std::size_t compaction_io_bytes_per_vector = 0;
  std::size_t coaccess_sessions = 64;
  std::size_t coaccess_trace_length = 32;
  std::size_t hotpath_train_queries = 200;
  std::size_t hotpath_search_width = 128;
  std::size_t hotpath_entry_count = 32;
  std::size_t approx_projections = 8;
  std::size_t approx_window = 24;
  std::size_t approx_random_samples = 32;
  std::size_t approx_candidate_limit = 256;
  std::size_t prune_passes = 1;
  std::size_t lsh_tables = 8;
  std::size_t lsh_bits = 14;
  std::size_t lsh_probe_radius = 0;
  std::size_t lsh_bucket_limit = 64;
  double robust_prune_alpha = 1.2;
  double sla_p99_ms = 1.0;
  std::size_t early_stop_patience = 16;
  double early_stop_eps = 0.001;
  std::size_t min_expansions = 64;
  std::size_t max_expansions = 192;
  std::size_t pq_m = 8;
  std::size_t pq_ks = 256;
  std::size_t pq_train_limit = 100000;
  std::size_t pq_train_iterations = 4;
  std::size_t rerank_topk = 0;
  std::size_t io_batch_size = 1;
  std::size_t io_depth = 1;
  std::size_t prefetch_width = 0;
  std::size_t prefetch_depth = 1;
  std::size_t memory_budget_bytes = 0;
  double memory_budget_ratio = 0.20;
  std::string pq_code_path;
  std::string prefetch_policy = "frontier-next-hop";
  bool synthetic = true;
  bool build_index = false;
  bool wal_replay = false;
  bool search_early_stop = false;
  std::size_t search_early_stop_min_expansions = 0;
  bool adaptive_early_stop = false;
  bool pq_enable = false;
  bool adc_enable = false;
  bool enforce_memory_budget = false;
  bool allow_over_budget_for_debug = false;
  bool allow_io_fallback = false;
  bool page_dedup = true;
  bool same_page_reuse = true;
  bool page_coalesce = true;
  bool cache_protect_hot_pages = false;
  bool reverse_edge_patch = true;
  bool release_raw_base_after_prepare = false;
  agentmem::SyntheticConfig synthetic_config;
};

bool is_memory_engine(const std::string& engine);

Args parse_args(int argc, char** argv);

}  // namespace agentmem::benchmark
