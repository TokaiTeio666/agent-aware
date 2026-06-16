#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentmem/core/pq_encoder.h"
#include "agentmem/core/types.h"

namespace agentmem {

struct DiskGraphBuildStats {
  std::size_t hotpath_train_queries = 0;
  std::size_t hotpath_unique_visited_nodes = 0;
  std::size_t hotpath_top_node_visit_count = 0;
  std::size_t reverse_edges_added = 0;
  std::size_t reverse_prune_count = 0;
  double avg_degree = 0.0;
  std::size_t max_degree = 0;
  double graph_candidate_avg = 0.0;
  double graph_candidate_p95 = 0.0;
  double graph_candidate_p99 = 0.0;
  std::vector<std::size_t> candidate_counts;
};

class PqAdcModel : public PQEncoder {};

struct DiskGraphBuildConfig {
  std::size_t degree = 32;
  std::size_t page_size = 4096;
  std::string build_policy = "exact";
  std::string packing_strategy = "one-node";
  std::uint32_t random_seed = 42;
  std::size_t approx_projections = 8;
  std::size_t approx_window = 24;
  std::size_t approx_random_samples = 32;
  std::size_t approx_candidate_limit = 256;
  bool reverse_edge_patch = true;
  std::size_t prune_passes = 1;
  std::size_t lsh_tables = 8;
  std::size_t lsh_bits = 14;
  std::size_t lsh_probe_radius = 0;
  std::size_t lsh_bucket_limit = 64;
  double robust_prune_alpha = 1.2;
  std::vector<std::uint32_t> deleted_ids;
  std::size_t coaccess_sessions = 64;
  std::size_t coaccess_trace_length = 32;
  const VectorSet* hotpath_queries = nullptr;
  std::size_t hotpath_train_queries = 200;
  std::size_t hotpath_search_width = 128;
  std::size_t hotpath_entry_count = 32;
  DiskGraphBuildStats* stats = nullptr;
};

struct DiskGraphSearchConfig {
  std::size_t top_k = 10;
  std::size_t search_width = 64;
  std::size_t entry_count = 32;
  std::vector<std::uint32_t> seed_ids;
  bool early_stop = false;
  std::size_t early_stop_min_expansions = 0;
  bool adaptive_early_stop = false;
  std::size_t early_stop_patience = 16;
  double early_stop_eps = 0.001;
  std::size_t min_expansions = 64;
  const PqAdcModel* pq_model = nullptr;
  bool adc_enable = false;
  std::size_t rerank_topk = 0;
  std::size_t prefetch_width = 0;
  std::string prefetch_policy = "frontier-next-hop";
  bool page_dedup = true;
  bool same_page_reuse = true;
  bool page_coalesce = true;
};

struct DiskGraphSearchStats {
  std::size_t node_reads = 0;
  std::size_t expanded = 0;
  std::size_t visited = 0;
  std::size_t page_requests = 0;
  std::size_t page_requests_before_dedup = 0;
  std::size_t page_requests_after_dedup = 0;
  std::size_t page_cache_hits = 0;
  std::size_t page_cache_misses = 0;
  std::size_t page_cache_evictions = 0;
  std::size_t page_cache_promotions = 0;
  std::size_t io_submits = 0;
  std::size_t io_completions = 0;
  std::size_t io_submit_syscalls = 0;
  std::size_t io_prefetches = 0;
  std::size_t io_prefetch_hits = 0;
  std::size_t io_prefetch_waits = 0;
  std::size_t io_pending_pages_peak = 0;
  std::size_t prefetch_submitted_pages = 0;
  std::size_t prefetch_useful_pages = 0;
  std::size_t prefetch_wasted_pages = 0;
  std::size_t demand_read_waits = 0;
  double demand_read_wait_us = 0.0;
  std::size_t page_dedup_requests = 0;
  std::size_t page_dedup_hits = 0;
  std::size_t same_page_node_reuse = 0;
  double adc_table_build_us = 0.0;
  std::size_t rerank_reads = 0;
  std::size_t pq_filter_reject_count = 0;
  std::size_t pq_filter_accept_count = 0;
};

struct DiskGraphSearchResult {
  std::vector<SearchResult> topk;
  DiskGraphSearchStats stats;
};

struct DiskGraphIoStatus {
  std::string requested_mode = "pread";
  std::string effective_mode = "pread";
  std::string fallback_reason;
  bool direct_enabled = false;
  bool io_uring_enabled = false;
  std::size_t batch_size = 1;
  std::size_t depth = 1;
};

struct DiskGraphMetadata {
  std::uint64_t vector_count = 0;
  std::uint32_t dim = 0;
  std::uint32_t degree = 0;
  std::uint32_t page_size = 0;
  std::uint64_t records_offset = 0;
  std::uint64_t directory_offset = 0;
  std::uint64_t page_count = 0;
  std::uint32_t nodes_per_page = 1;
};

class DiskPageReader;

class NaiveDiskGraphBuilder {
 public:
  static void build(const VectorSet& base, const std::string& path,
                    const DiskGraphBuildConfig& config);
};

class NaiveDiskGraphIndex {
 public:
  explicit NaiveDiskGraphIndex(const std::string& path);
  ~NaiveDiskGraphIndex();

  const DiskGraphMetadata& metadata() const {
    return metadata_;
  }

  void configure_io(const std::string& mode, std::size_t batch_size,
                    std::size_t io_depth = 1);

  const DiskGraphIoStatus& io_status() const;

  DiskGraphSearchResult search_one(const float* query,
                                   const DiskGraphSearchConfig& config);

 private:
  struct DiskNode {
    std::uint32_t id = 0;
    std::vector<float> vector;
    std::vector<std::uint32_t> neighbors;
  };

  DiskNode read_node(std::uint32_t id, DiskGraphSearchStats& stats);

  std::string path_;
  std::ifstream input_;
  std::unique_ptr<DiskPageReader> page_reader_;
  DiskGraphMetadata metadata_;
};

class PackedDiskGraphBuilder {
 public:
  static void build(const VectorSet& base, const std::string& path,
                    const DiskGraphBuildConfig& config);
};

class PackedDiskGraphIndex {
 public:
  explicit PackedDiskGraphIndex(const std::string& path);
  ~PackedDiskGraphIndex();

  const DiskGraphMetadata& metadata() const {
    return metadata_;
  }

  void configure_cache(const std::string& policy, std::size_t capacity_pages,
                       bool protect_hot_pages = false,
                       std::size_t hot_degree_threshold = 0);
  void configure_io(const std::string& mode, std::size_t batch_size,
                    std::size_t io_depth = 1);

  const DiskGraphIoStatus& io_status() const;

  DiskGraphSearchResult search_one(const float* query,
                                   const DiskGraphSearchConfig& config);

 private:
  struct DiskNode {
    std::uint32_t id = 0;
    std::vector<float> vector;
    std::vector<std::uint32_t> neighbors;
  };

  struct DecodedPage {
    std::uint32_t page_id = 0;
    std::vector<DiskNode> nodes;
  };

  struct CacheEntry {
    DecodedPage page;
    std::uint64_t last_access = 0;
    std::uint64_t frequency = 0;
    bool protected_queue = false;
    std::uint64_t hot_score = 0;
  };

  DecodedPage read_page(std::uint32_t page_id, DiskGraphSearchStats& stats);
  DecodedPage decode_page(std::uint32_t page_id,
                          const std::vector<char>& page) const;
  const DecodedPage& load_page(std::uint32_t page_id,
                               DiskGraphSearchStats& stats);
  const DecodedPage* lookup_cached_page(std::uint32_t page_id,
                                        DiskGraphSearchStats& stats);
  const DecodedPage& store_cached_page(DecodedPage page,
                                       DiskGraphSearchStats& stats);
  bool cache_enabled() const;
  void evict_one_page(DiskGraphSearchStats* stats);
  double cache_score(const CacheEntry& entry) const;
  std::uint64_t page_hot_score(const DecodedPage& page) const;
  bool is_two_queue_policy() const;
  bool is_graph_aware_cache_policy() const;

  std::string path_;
  std::ifstream input_;
  std::unique_ptr<DiskPageReader> page_reader_;
  DiskGraphMetadata metadata_;
  std::vector<std::uint32_t> node_to_page_;
  std::string cache_policy_ = "none";
  std::size_t cache_capacity_pages_ = 0;
  bool cache_protect_hot_pages_ = false;
  std::size_t cache_hot_degree_threshold_ = 0;
  std::uint64_t cache_clock_ = 0;
  std::unordered_map<std::uint32_t, CacheEntry> page_cache_;
  DecodedPage scratch_page_;
};

}  // namespace agentmem
