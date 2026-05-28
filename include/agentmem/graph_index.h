#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentmem/types.h"

namespace agentmem {

struct DiskGraphBuildConfig {
  std::size_t degree = 16;
  std::size_t page_size = 4096;
  std::string packing_strategy = "one-node";
  std::uint32_t random_seed = 42;
  std::size_t coaccess_sessions = 64;
  std::size_t coaccess_trace_length = 32;
};

struct DiskGraphSearchConfig {
  std::size_t top_k = 10;
  std::size_t search_width = 64;
  std::size_t entry_count = 32;
  std::vector<std::uint32_t> seed_ids;
};

struct DiskGraphSearchStats {
  std::size_t node_reads = 0;
  std::size_t expanded = 0;
  std::size_t visited = 0;
  std::size_t page_requests = 0;
  std::size_t page_cache_hits = 0;
  std::size_t page_cache_misses = 0;
};

struct DiskGraphSearchResult {
  std::vector<SearchResult> topk;
  DiskGraphSearchStats stats;
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

class NaiveDiskGraphBuilder {
 public:
  static void build(const VectorSet& base, const std::string& path,
                    const DiskGraphBuildConfig& config);
};

class NaiveDiskGraphIndex {
 public:
  explicit NaiveDiskGraphIndex(const std::string& path);

  const DiskGraphMetadata& metadata() const {
    return metadata_;
  }

  DiskGraphSearchResult search_one(const float* query,
                                   const DiskGraphSearchConfig& config);

 private:
  struct DiskNode {
    std::uint32_t id = 0;
    std::vector<float> vector;
    std::vector<std::uint32_t> neighbors;
  };

  DiskNode read_node(std::uint32_t id);

  std::string path_;
  std::ifstream input_;
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

  const DiskGraphMetadata& metadata() const {
    return metadata_;
  }

  void configure_cache(const std::string& policy, std::size_t capacity_pages);

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

  DecodedPage read_page(std::uint32_t page_id);
  const DecodedPage& load_page(std::uint32_t page_id,
                               DiskGraphSearchStats& stats);
  void evict_one_page();
  double cache_score(const CacheEntry& entry) const;

  std::string path_;
  std::ifstream input_;
  DiskGraphMetadata metadata_;
  std::vector<std::uint32_t> node_to_page_;
  std::string cache_policy_ = "none";
  std::size_t cache_capacity_pages_ = 0;
  std::uint64_t cache_clock_ = 0;
  std::unordered_map<std::uint32_t, CacheEntry> page_cache_;
  DecodedPage scratch_page_;
};

}  // namespace agentmem
