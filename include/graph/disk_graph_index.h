#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/io_stats.h"
#include "core/prefetch_adaptive_controller.h"
#include "core/pq_encoder.h"
#include "core/types.h"

namespace agent_aware {

using PqAdcModel = PQEncoder;

struct TopologyPageCandidate {
  std::uint32_t page_id = 0;
  std::size_t min_pq_rank = 0;
  std::size_t candidate_count = 0;
};

std::vector<TopologyPageCandidate> rank_topology_pages(
    const std::vector<std::uint32_t>& ranked_page_ids,
    std::size_t candidate_window);

struct DiskGraphBuildConfig {
  std::size_t degree = 32;
  std::size_t page_size = 4096;
  std::uint32_t random_seed = 42;
  std::vector<std::uint32_t> deleted_ids;
  const PqAdcModel* pq_model = nullptr;
};

struct DiskGraphBuildResult {
  std::uint32_t medoid = 0;
};

struct DiskGraphSearchConfig {
  std::size_t top_k = 10;
  std::size_t search_width = 64;
  std::size_t beam_width = 0;
  std::size_t entry_count = 1;
  std::vector<std::uint32_t> seed_ids;
  const PqAdcModel* pq_model = nullptr;
  bool adc_enable = false;
  std::size_t rerank_topk = 0;
  std::string prefetch_policy = "none";
  bool prefetch_adaptive = true;
  std::size_t prefetch_top_k = 2;
  std::size_t prefetch_max_inflight = 2;
  std::size_t prefetch_max_pages_per_query = 0;
  std::string prefetch_trace_path;
  std::string run_id = "manual";
  std::uint64_t query_id = 0;
};

struct DiskGraphSearchStats {
  std::size_t node_reads = 0;
  std::size_t expanded = 0;
  std::size_t visited = 0;
  std::uint64_t page_requests = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t pinned_hits = 0;
  std::uint64_t demand_reads = 0;
  std::size_t unique_nodes_accessed = 0;
  std::size_t unique_pages_touched = 0;
  std::size_t unique_demand_pages = 0;
  std::uint64_t duplicate_skipped = 0;
  std::uint64_t submitted_reads = 0;
  std::uint64_t completed_reads = 0;
  std::size_t page_requests_before_dedup = 0;
  std::size_t page_requests_after_dedup = 0;
  std::size_t page_cache_hits = 0;
  std::size_t page_cache_misses = 0;
  std::size_t page_cache_evictions = 0;
  std::size_t page_cache_promotions = 0;
  std::size_t distance_direct_calls = 0;
  std::size_t io_submits = 0;
  std::size_t io_completions = 0;
  std::size_t io_submit_syscalls = 0;
  std::size_t uring_submit_count = 0;
  std::size_t uring_cqe_count = 0;
  std::size_t page_dedup_requests = 0;
  std::size_t page_dedup_hits = 0;
  std::size_t duplicate_pages_eliminated = 0;
  std::size_t batch_count = 0;
  std::size_t batch_expanded = 0;
  std::size_t max_batch_size = 0;
  std::size_t beam_unique_pages = 0;
  std::size_t beam_cached_pages = 0;
  std::size_t same_page_node_reuse = 0;
  double adc_table_build_us = 0.0;
  std::size_t rerank_reads = 0;
  std::size_t pq_filter_reject_count = 0;
  std::size_t pq_filter_accept_count = 0;
  double demand_read_wait_us = 0.0;
  double search_mutex_wait_us = 0.0;
  std::size_t unique_prefetch_pages = 0;
  std::size_t prefetch_candidate_nodes = 0;
  std::size_t prefetch_candidate_pages = 0;
  std::size_t prefetch_submitted = 0;
  std::size_t prefetch_submit_batches = 0;
  std::size_t prefetch_ready_hit = 0;
  std::size_t prefetch_pending_hit = 0;
  std::size_t prefetch_useful_pages = 0;
  std::size_t prefetch_wasted_pages = 0;
  std::size_t prefetch_skip_cached = 0;
  std::size_t prefetch_skip_pending = 0;
  std::size_t prefetch_skip_seen = 0;
  std::size_t prefetch_skip_budget = 0;
  std::size_t demand_priority_blocks_prefetch = 0;
  std::size_t prefetch_adaptive_pause_count = 0;
  std::size_t prefetch_adaptive_final_top_k = 0;
  std::size_t prefetch_adaptive_final_lead = 1;
  double prefetch_saved_wait_us = 0.0;
  double prefetch_planner_cpu_us = 0.0;
};

struct DiskGraphSearchResult {
  std::vector<SearchResult> topk;
  DiskGraphSearchStats stats;
};

struct DiskGraphMetadata {
  std::uint32_t format_version = 0;
  std::uint64_t vector_count = 0;
  std::uint32_t dim = 0;
  std::uint32_t degree = 0;
  std::uint32_t page_size = 0;
  std::uint64_t records_offset = 0;
  std::uint64_t directory_offset = 0;
  std::uint64_t page_count = 0;
  std::uint32_t nodes_per_page = 1;
  std::uint32_t pq_subspaces = 0;
  std::uint32_t pq_centroids = 0;
  std::uint32_t pq_dim = 0;
  std::uint64_t pq_offset = 0;
  std::uint64_t pq_bytes = 0;
  std::uint64_t pq_offsets_offset = 0;
  std::uint64_t pq_offsets_bytes = 0;
  std::uint64_t pq_codebook_offset = 0;
  std::uint64_t pq_codebook_bytes = 0;
  std::uint64_t pq_codes_offset = 0;
  std::uint64_t pq_codes_bytes = 0;
  std::uint64_t index_bytes = 0;
};

class AsyncPageReader;
class DiskPageCodec;
class QueryPageSession;

class PackedDiskGraphBuilder {
 public:
  static DiskGraphBuildResult build(const VectorSet& base,
                                    const std::string& path,
                                    const DiskGraphBuildConfig& config);
};

class PackedDiskGraphIndex {
 public:
  explicit PackedDiskGraphIndex(const std::string& path);
  ~PackedDiskGraphIndex();

  const DiskGraphMetadata& metadata() const {
    return metadata_;
  }
  const std::vector<std::uint32_t>& node_in_degrees();

  void configure_cache(const std::string& policy, std::size_t capacity_pages,
                       bool protect_hot_pages = false,
                       std::size_t hot_degree_threshold = 0);
  void configure_io(const std::string& mode, std::size_t batch_size,
                    std::size_t io_depth = 1);

  const DiskGraphIoStatus& io_status() const;
  const PqAdcModel& pq_model() const { return pq_model_; }
  std::size_t cache_capacity_pages() const;
  std::size_t cache_resident_pages() const;

  DiskGraphSearchResult search_one(const float* query,
                                   const DiskGraphSearchConfig& config);

 private:
  friend class DiskPageCodec;
  friend class QueryPageSession;
  struct SearchState;

  struct DiskNode {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> neighbors;
    std::vector<float> vector;
    std::size_t vector_offset = 0;
  };

  struct DecodedPage {
    std::uint32_t page_id = 0;
    std::vector<char> bytes;
    std::vector<DiskNode> nodes;
  };

  struct CacheEntry {
    DecodedPage page;
    std::uint64_t last_access = 0;
    std::uint64_t frequency = 0;
    bool protected_queue = false;
    std::uint64_t hot_score = 0;
    std::uint64_t pin_count = 0;
  };

  DecodedPage read_page(std::uint32_t page_id, DiskGraphSearchStats& stats);
  DecodedPage decode_page(std::uint32_t page_id,
                          std::vector<char> page) const;
  const DecodedPage& load_page(std::uint32_t page_id,
                               DiskGraphSearchStats& stats);
  const DecodedPage* lookup_cached_page(std::uint32_t page_id,
                                        DiskGraphSearchStats& stats);
  const DecodedPage& store_cached_page(DecodedPage page,
                                       DiskGraphSearchStats& stats);
  bool cache_enabled() const;
  bool evict_one_page(DiskGraphSearchStats* stats);
  bool evict_one_page_locked(DiskGraphSearchStats* stats);
  double cache_score(const CacheEntry& entry) const;
  std::uint64_t page_hot_score(const DecodedPage& page) const;
  bool pin_if_cached(std::uint32_t page_id);
  void unpin_if_cached(std::uint32_t page_id);
  void ensure_node_in_degrees();
  const DiskNode& find_node_in_page(const DecodedPage& page,
                                    std::uint32_t node_id) const;
  const float* vector_data(const DecodedPage& page,
                           const DiskNode& node) const;
  float compute_distance_direct(const float* query, const DecodedPage& page,
                                const DiskNode& node) const;
  bool is_two_queue_policy() const;
  bool is_graph_aware_cache_policy() const;
  std::unique_ptr<SearchState> initialize_search_state(
      const float* query, const DiskGraphSearchConfig& config,
      DiskGraphSearchResult& output);
  std::vector<std::uint32_t> collect_rerank_pages(
      const SearchState& state) const;
  std::vector<SearchResult> select_candidate_batch(
      SearchState& state, std::size_t effective_beam_width) const;
  void expand_candidate_batch(SearchState& state,
                              const std::vector<SearchResult>& batch,
                              std::size_t effective_beam_width);
  void finalize_topk(SearchState& state);

  std::string path_;
  std::ifstream input_;
  std::unique_ptr<AsyncPageReader> page_reader_;
  DiskGraphMetadata metadata_;
  PqAdcModel pq_model_;
  std::vector<std::uint32_t> node_to_page_;
  std::string cache_policy_ = "none";
  std::size_t cache_capacity_pages_ = 0;
  bool cache_protect_hot_pages_ = false;
  std::size_t cache_hot_degree_threshold_ = 0;
  std::uint64_t cache_clock_ = 0;
  std::unordered_map<std::uint32_t, CacheEntry> page_cache_;
  DecodedPage scratch_page_;
  std::vector<std::uint32_t> node_in_degrees_;
  bool node_in_degrees_ready_ = false;
  mutable std::mutex cache_mutex_;
  mutable std::mutex search_mutex_;
  PrefetchAdaptiveController prefetch_controller_;
};

}  // namespace agent_aware
