#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentmem/types.h"

namespace agentmem {

struct DiskGraphBuildStats {
  std::size_t hotpath_train_queries = 0;
  std::size_t hotpath_unique_visited_nodes = 0;
  std::size_t hotpath_top_node_visit_count = 0;
};

class PqAdcModel {
 public:
  void train(const VectorSet& base, std::size_t subspaces,
             std::size_t centroids, std::size_t train_limit,
             std::size_t iterations, std::uint32_t seed);

  bool enabled() const {
    return !codes_.empty();
  }

  std::size_t subspaces() const {
    return subspaces_;
  }

  std::size_t centroids() const {
    return centroids_;
  }

  std::size_t code_bytes() const {
    return codes_.size();
  }

  std::size_t codebook_bytes() const {
    return codebooks_.size() * sizeof(float);
  }

  std::size_t offset_bytes() const {
    return offsets_.size() * sizeof(std::size_t);
  }

  std::vector<float> build_adc_table(const float* query) const;
  float adc_distance(std::uint32_t id, const std::vector<float>& table) const;

 private:
  std::size_t subspaces_ = 0;
  std::size_t centroids_ = 0;
  std::size_t dim_ = 0;
  std::vector<std::size_t> offsets_;
  std::vector<float> codebooks_;
  std::vector<std::uint8_t> codes_;
};

struct DiskGraphBuildConfig {
  std::size_t degree = 16;
  std::size_t page_size = 4096;
  std::string build_policy = "exact";
  std::string packing_strategy = "one-node";
  std::uint32_t random_seed = 42;
  std::size_t approx_projections = 8;
  std::size_t approx_window = 24;
  std::size_t approx_random_samples = 32;
  std::size_t approx_candidate_limit = 512;
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
};

struct DiskGraphSearchStats {
  std::size_t node_reads = 0;
  std::size_t expanded = 0;
  std::size_t visited = 0;
  std::size_t page_requests = 0;
  std::size_t page_cache_hits = 0;
  std::size_t page_cache_misses = 0;
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

  void configure_cache(const std::string& policy, std::size_t capacity_pages);
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
  };

  DecodedPage read_page(std::uint32_t page_id, DiskGraphSearchStats& stats);
  DecodedPage decode_page(std::uint32_t page_id,
                          const std::vector<char>& page) const;
  const DecodedPage& load_page(std::uint32_t page_id,
                               DiskGraphSearchStats& stats);
  const DecodedPage* lookup_cached_page(std::uint32_t page_id,
                                        DiskGraphSearchStats& stats);
  const DecodedPage& store_cached_page(DecodedPage page);
  bool cache_enabled() const;
  void evict_one_page();
  double cache_score(const CacheEntry& entry) const;

  std::string path_;
  std::ifstream input_;
  std::unique_ptr<DiskPageReader> page_reader_;
  DiskGraphMetadata metadata_;
  std::vector<std::uint32_t> node_to_page_;
  std::string cache_policy_ = "none";
  std::size_t cache_capacity_pages_ = 0;
  std::uint64_t cache_clock_ = 0;
  std::unordered_map<std::uint32_t, CacheEntry> page_cache_;
  DecodedPage scratch_page_;
};

}  // namespace agentmem
