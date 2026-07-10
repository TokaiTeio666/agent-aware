#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "agent_aware/core/types.h"

namespace agent_aware {

struct VamanaBuildConfig {
  std::size_t max_degree = 32;
  std::size_t search_width = 200;
  double alpha = 1.2;
  std::uint32_t seed = 42;
  bool add_reverse_edges = true;
  std::vector<std::uint32_t> deleted_ids;
};

struct VamanaBuildStats {
  std::uint32_t medoid = 0;
  std::size_t reverse_edges_added = 0;
  std::size_t reverse_prune_count = 0;
  std::size_t prune_count = 0;
  double avg_degree = 0.0;
  std::size_t max_degree = 0;
  double candidate_avg = 0.0;
  double candidate_p95 = 0.0;
  double candidate_p99 = 0.0;
  std::vector<std::size_t> candidate_counts;
};

struct VamanaGraph {
  std::uint32_t medoid = 0;
  std::vector<std::vector<std::uint32_t>> adjacency;
  VamanaBuildStats stats;
};

class VamanaBuilder {
 public:
  explicit VamanaBuilder(VamanaBuildConfig config = {});

  VamanaGraph build(const VectorSet& base) const;

  std::uint32_t find_medoid(const VectorSet& base) const;

  std::vector<std::uint32_t> search_for_construction(
      const VectorSet& base,
      const std::vector<std::vector<std::uint32_t>>& graph,
      const float* query, std::uint32_t entry_id,
      std::uint32_t excluded_id = UINT32_MAX) const;

  std::vector<std::uint32_t> robust_prune(
      const VectorSet& base, std::uint32_t node_id,
      const std::vector<std::uint32_t>& candidates) const;

  void insert(const VectorSet& base,
              std::vector<std::vector<std::uint32_t>>& graph,
              std::uint32_t node_id, std::uint32_t entry_id,
              VamanaBuildStats* stats = nullptr) const;

 private:
  std::vector<char> deleted_mask(std::size_t count) const;
  std::vector<std::vector<std::uint32_t>> initial_graph(
      const VectorSet& base, const std::vector<char>& deleted) const;
  std::vector<std::uint32_t> build_order(std::size_t count) const;
  void add_reverse_edge(const VectorSet& base,
                        std::vector<std::vector<std::uint32_t>>& graph,
                        std::uint32_t source, std::uint32_t target,
                        VamanaBuildStats* stats) const;
  void finalize_stats(const std::vector<std::vector<std::uint32_t>>& graph,
                      VamanaBuildStats& stats) const;

  VamanaBuildConfig config_;
};

}  // namespace agent_aware
